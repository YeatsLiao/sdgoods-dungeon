/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dg_audio.c —— 程序化混音（地牢氛围 + 上游手感拟音 SFX）
 *
 * 一条 FreeRTOS 任务持续把 16kHz 立体声 PCM 推到 BSP 的 I2S 通道
 * （sdgoods_audio_stream_write 用 portMAX_DELAY 阻塞，任务因此被 DMA 消耗
 * 速率自然节流，无需自己算 sleep）。所有声音都是运行时算出来的正弦/方/锯/
 * 三角波，不读任何音频文件。
 */
#include "dg_audio.h"
#include "dungeon_api.h"
#include "sdgoods_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static const char *TAG = "dg.audio";

#define SR           16000        /* 采样率，与 BSP I2S 固定一致 */
#define CHUNK        256          /* 每块帧数（16ms） */
#define MAX_VOICE    6

/* ---- 波形编号 ---- */
enum { W_SINE = 0, W_SQUARE, W_SAW, W_TRI };

/* ---- SFX 定义表（下标 = dg_sfx_id_t）---- */
typedef struct {
    uint16_t f0;        /* 起始频率 Hz */
    uint16_t f1;        /* 结束频率 Hz（==f0 为不扫频） */
    uint16_t dur_ms;    /* 时长 */
    uint8_t  wave;
    uint8_t  amp;       /* 0..255 峰值 */
} SfxDef;

static const SfxDef k_sfx[DG_SFX_COUNT] = {
    /* NONE  */ {   0,   0,   0, W_SINE,   0 },
    /* STEP  */ { 180, 120,  45, W_TRI,   60 },
    /* HIT   */ { 320, 160,  90, W_SAW,  130 },
    /* MISS  */ { 900, 500,  70, W_SQUARE, 40 },
    /* HURT  */ { 240,  90, 160, W_SAW,  150 },
    /* KILL  */ { 500, 120, 220, W_TRI,  140 },
    /* DIE   */ { 300,  60, 700, W_SAW,  160 },
    /* PICKUP*/ { 600, 950, 110, W_TRI,  120 },
    /* GOLD  */ { 980,1320, 140, W_SQUARE, 90 },
    /*LEVELUP*/ { 520,1040, 320, W_TRI,  150 },
    /* DRINK */ { 400, 700, 180, W_SINE, 110 },
    /* SCROLL*/ { 700, 300, 200, W_SQUARE, 55 },
    /* DOOR  */ { 200, 320, 180, W_TRI,  110 },
    /*LOCKED */ { 150, 140, 120, W_SQUARE,120 },
    /* STAIRS*/ { 420, 200, 260, W_TRI,  120 },
    /* TRAP  */ { 880, 220, 200, W_SAW,  160 },
    /* CHEST */ { 300, 620, 240, W_TRI,  130 },
    /* SELECT*/ { 700, 900,  60, W_SINE, 100 },
    /* ERROR */ { 200, 160, 120, W_SQUARE,110 },
    /* WIN   */ { 520,1560, 600, W_TRI,  160 },
    /* ZAP   */ {1200, 300, 150, W_SAW,  130 },
};

/* ---- 发声中的音效实例 ---- */
typedef struct {
    volatile bool active;
    float    phase;
    float    f0, f1;
    int      left;      /* 剩余样本 */
    int      total;     /* 总样本（算包络） */
    uint8_t  wave;
    float    amp;       /* 0..1 */
} Voice;

static Voice          s_voice[MAX_VOICE];
static volatile bool  s_run = false;
static volatile bool  s_ambient = true;
static float          s_amb_phase = 0.0f;
static uint32_t       s_amb_beat = 0;
static int16_t        s_buf[CHUNK * 2];   /* 立体声交织 */

/* 单波形的一个样本，phase ∈ [0,1) */
static inline float osc(float phase, int wave)
{
    switch (wave) {
    case W_SQUARE: return phase < 0.5f ? 1.0f : -1.0f;
    case W_SAW:    return 2.0f * phase - 1.0f;
    case W_TRI:    return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    default:       return sinf(2.0f * (float)M_PI * phase);
    }
}

/* 线性 AHDSR-lite：快起 + 后半段淡出 */
static inline float envelope(int elapsed, int total)
{
    float t = (float)elapsed / (float)total;
    float a = t < 0.05f ? (t / 0.05f) : 1.0f;
    float r = t > 0.6f ? (1.0f - (t - 0.6f) / 0.4f) : 1.0f;
    if (r < 0) r = 0;
    return a * r;
}

void dg_audio_play_sfx(int id)
{
    if (id <= DG_SFX_NONE || id >= DG_SFX_COUNT) return;
    const SfxDef* d = &k_sfx[id];
    if (d->dur_ms == 0 || d->amp == 0) return;
    int slot = -1;
    /* 优先空槽；都占则抢最早的（宁欠不丢，让最新动作有反馈） */
    for (int i = 0; i < MAX_VOICE; i++) if (!s_voice[i].active) { slot = i; break; }
    if (slot < 0) {
        int min_left = 0x7fffffff;
        for (int i = 0; i < MAX_VOICE; i++)
            if (s_voice[i].left < min_left) { min_left = s_voice[i].left; slot = i; }
    }
    Voice* v = &s_voice[slot];
    int total = (int)((int64_t)d->dur_ms * SR / 1000);
    v->f0 = d->f0; v->f1 = d->f1;
    v->wave = d->wave;
    v->amp = d->amp / 255.0f;
    v->total = total;
    v->left = total;
    v->phase = 0.0f;
    v->active = true;
}

void dg_audio_set_ambient(bool on) { s_ambient = on; }

/* 地牢氛围：55Hz 与 55.3Hz 两列微失谐正弦叠出缓慢拍动的低频 drone，
 * 音量压得很低（0.05），只给空间感不抢音效。 */
static float ambient_sample(void)
{
    if (!s_ambient) return 0.0f;
    float p1 = s_amb_phase;
    float p2 = s_amb_phase * (55.3f / 55.0f);
    float drone = 0.5f * sinf(2 * (float)M_PI * p1) + 0.5f * sinf(2 * (float)M_PI * p2);
    s_amb_phase += 55.0f / SR;
    if (s_amb_phase >= 1.0f) s_amb_phase -= 1.0f;
    s_amb_beat++;
    return drone * 0.05f;
}

static void mix_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "mix task start");
    while (s_run) {
        int vol = sdgoods_audio_get_volume();       /* 0..100 */
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        float master = vol / 100.0f;

        for (int i = 0; i < CHUNK; i++) {
            float acc = ambient_sample();

            for (int vi = 0; vi < MAX_VOICE; vi++) {
                Voice* v = &s_voice[vi];
                if (!v->active) continue;
                int elapsed = v->total - v->left;
                float t = (float)elapsed / (float)v->total;
                float freq = v->f0 + (v->f1 - v->f0) * t;
                v->phase += freq / SR;
                if (v->phase >= 1.0f) v->phase -= 1.0f;
                acc += osc(v->phase, v->wave) * v->amp * envelope(elapsed, v->total);
                if (--v->left <= 0) v->active = false;
            }

            int16_t s = (int16_t)(acc * master * 12000.0f);
            if (s > 32000) s = 32000;
            if (s < -32000) s = -32000;
            s_buf[i * 2] = s;
            s_buf[i * 2 + 1] = s;
        }
        sdgoods_audio_stream_write(s_buf, CHUNK);
    }
    ESP_LOGI(TAG, "mix task exit");
    vTaskDelete(NULL);
}

void dg_audio_start(void)
{
    if (s_run) return;
    if (sdgoods_audio_stream_open() != ESP_OK) {
        ESP_LOGW(TAG, "stream open failed, audio disabled");
        return;
    }
    memset(s_voice, 0, sizeof(s_voice));
    s_amb_phase = 0.0f;
    s_amb_beat = 0;
    s_run = true;
    BaseType_t ok = xTaskCreatePinnedToCore(mix_task, "dg_mix", 4096, NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "mix task create fail");
        s_run = false;
        sdgoods_audio_stream_close();
    }
}

void dg_audio_stop(void)
{
    if (!s_run) return;
    s_run = false;
    /* mix_task 在下一块写完 while 退出后自删；稍等它冲到静音再关通道 */
    vTaskDelay(pdMS_TO_TICKS(40));
    sdgoods_audio_stream_close();
}
