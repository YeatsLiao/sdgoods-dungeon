/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dg_audio.h —— 地牢音频（程序化合成，无外部音频素材）
 *
 * 设备没有 mp3/ogg 解码器、也放不下上游十几 MB 音频分区，所以这里用 I2S
 * 推流 + 运行时波形合成：一条混音任务持续产 16kHz 立体声 PCM，把「环境低音
 * + 上游手感的拟音音效」实时算出来。音效不自己排期，而是 UI 每帧从引擎
 * SFX 队列 pop 出 DG_SFX_*，转成一次发声，保证「发生了什么就响什么」。
 */
#ifndef DG_AUDIO_H
#define DG_AUDIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 打开扬声器通道并起混音任务（幂等）。失败不影响游戏，只是静音。 */
void dg_audio_start(void);

/* 停混音任务 + 关功放。 */
void dg_audio_stop(void);

/* 触发一个音效（id 为 dg_sfx_id_t）。由 UI 从引擎队列取出后调用。 */
void dg_audio_play_sfx(int id);

/* 环境音开关（true = 起地牢低频氛围，false = 静音背景，音效仍响）。 */
void dg_audio_set_ambient(bool on);

#ifdef __cplusplus
}
#endif
#endif
