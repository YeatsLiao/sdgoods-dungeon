/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * gfx.cpp —— 烘焙图集加载 + blit 原语
 */
#include "gfx/gfx.h"
#include "dg_types.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>

static const char *TAG = "dg.gfx";

namespace dg {
namespace gfx {

/* RGB5 容器头（与 pack_assets.py::bake_rgb565 对偶） */
struct Rgb5Header {
    char     magic[4];   /* "RGB5" */
    uint16_t w;
    uint16_t h;
};

static const uint16_t* s_tiles     = nullptr;   /* 256×256 亮 */
static uint16_t*       s_tiles_dim = nullptr;   /* 暗化副本（记忆态） */
static const uint16_t* s_rat       = nullptr;   /* 256×64 */
static const uint16_t* s_hero      = nullptr;   /* 256×128 */

/* 从 assets 分区取一张 RGB5 烘焙图，返回像素起点（PSRAM 常驻，不释放） */
static const uint16_t* load_baked(const char* name, uint16_t* out_w, uint16_t* out_h) {
    uint32_t size = 0;
    const void* blob = assets::get_asset_by_name(name, &size);
    if (!blob) {
        ESP_LOGW(TAG, "烘焙图缺失：%s（回退棋盘占位）", name);
        return nullptr;
    }
    if (size < sizeof(Rgb5Header) ||
        memcmp(blob, "RGB5", 4) != 0) {
        ESP_LOGE(TAG, "%s 非 RGB5 容器", name);
        heap_caps_free(const_cast<void*>(blob));
        return nullptr;
    }
    Rgb5Header hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    if (size != sizeof(hdr) + (uint32_t)hdr.w * hdr.h * 2) {
        ESP_LOGE(TAG, "%s 容器尺寸不符", name);
        heap_caps_free(const_cast<void*>(blob));
        return nullptr;
    }
    if (out_w) *out_w = hdr.w;
    if (out_h) *out_h = hdr.h;
    return (const uint16_t*)((const uint8_t*)blob + sizeof(hdr));
}

bool load() {
    if (s_tiles) return true;
    if (!assets::is_loaded()) {
        ESP_LOGW(TAG, "assets 分区无素材，渲染走棋盘占位");
        return false;
    }

    s_tiles = load_baked("tiles/sewers.rgb565", nullptr, nullptr);
    s_rat   = load_baked("sprites/rat.rgb565", nullptr, nullptr);
    s_hero  = load_baked("sprites/hero.rgb565", nullptr, nullptr);
    if (!s_tiles) return false;

    /* 记忆态暗化副本：逐通道 ×9/32 ≈ 28% 亮度（一次性成本，换渲染零乘法） */
    s_tiles_dim = (uint16_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
    if (s_tiles_dim) {
        for (int i = 0; i < 256 * 256; i++) {
            uint16_t c = s_tiles[i];
            uint16_t r = (uint16_t)((((c >> 11) & 0x1F) * 9 >> 5) << 11);
            uint16_t g = (uint16_t)((((c >> 5)  & 0x3F) * 9 >> 5) << 5);
            uint16_t b = (uint16_t)(((c & 0x1F) * 9 >> 5));
            s_tiles_dim[i] = r | g | b;
        }
    }
    ESP_LOGI(TAG, "烘焙图集就绪：tiles=%p rat=%p hero=%p",
             (const void*)s_tiles, (const void*)s_rat, (const void*)s_hero);
    return true;
}

bool ready() { return s_tiles != nullptr; }

const uint16_t* tile_sheet()     { return s_tiles; }
const uint16_t* tile_sheet_dim() { return s_tiles_dim ? s_tiles_dim : s_tiles; }
const uint16_t* rat_sheet()      { return s_rat; }
const uint16_t* hero_sheet()     { return s_hero; }

/* ---- blit 原语 ---- */

void blit(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
          const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h) {
    if (!sheet || dx >= fb_w || dy >= fb_h) return;
    int x_end = dx + w > fb_w ? fb_w - dx : w;
    int y_end = dy + h > fb_h ? fb_h - dy : h;
    if (x_end <= 0 || y_end <= 0) return;
    for (int j = 0; j < y_end; j++) {
        memcpy(&fb[(dy + j) * fb_w + dx],
               &sheet[(sy + j) * sheet_w + sx],
               x_end * sizeof(uint16_t));
    }
}

void blit_masked(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
                 const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h) {
    if (!sheet) return;
    for (int j = 0; j < h; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= fb_h) continue;
        const uint16_t* src = &sheet[(sy + j) * sheet_w + sx];
        for (int i = 0; i < w; i++) {
            int tx = dx + i;
            if (tx < 0 || tx >= fb_w) continue;
            uint16_t c = src[i];
            if (c) fb[ty * fb_w + tx] = c;   /* 纯黑 = 透明（Shattered 精灵惯例） */
        }
    }
}

void fill_rect(uint16_t* fb, int fb_w, int fb_h, int dx, int dy, int w, int h,
               uint16_t color) {
    for (int j = 0; j < h; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= fb_h) continue;
        for (int i = 0; i < w; i++) {
            int tx = dx + i;
            if (tx < 0 || tx >= fb_w) continue;
            fb[ty * fb_w + tx] = color;
        }
    }
}

}  /* namespace gfx */
}  /* namespace dg */
