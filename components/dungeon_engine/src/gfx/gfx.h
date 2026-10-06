/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * gfx.h —— 烘焙图集（RGB5 容器）加载与 blit 原语（引擎内部头，不出 src/）
 *
 * 架构决策（v0.2）：ESP32 侧零 PNG 解码 —— tools/pack_assets.py 已把必需
 * 图集预烘焙成 "RGB5" 容器（magic 4B + u16 w + u16 h + RGB565 LE 像素），
 * 本模块加载后常驻 PSRAM，渲染器只做 memcpy 级逐行 blit。
 * 容器格式与 pack_assets.py::bake_rgb565 严格对偶，改动需双侧同步。
 */
#ifndef DG_GFX_H
#define DG_GFX_H

#include <stdint.h>

namespace dg {
namespace gfx {

/* 懒加载：首次 load() 从 assets 分区读 3 张烘焙图 + 生成暗化副本。
 * 幂等；素材缺失 / header 不符返回 false（引擎回退骨架棋盘路径）。 */
bool load();
bool ready();

/* 256×256 下水道地形图集（16 列 × 16px/tile），未加载返回 nullptr */
const uint16_t* tile_sheet();
/* 同上的暗化版（记忆态：曾见过但当前不在 FOV 内），约 28% 亮度 */
const uint16_t* tile_sheet_dim();
/* 256×64 老鼠帧图（第 0 帧 idle 在 (0,0) 16×16），纯黑像素视为透明 */
const uint16_t* rat_sheet();
/* 256×128 英雄帧图（idle 帧在 (1,0) 12×15，HeroSprite FRAME 常量），黑色透明 */
const uint16_t* hero_sheet();

/* ---- blit 原语（目标 fb 行主序 RGB565，越界像素自动裁剪）---- */

/* 从宽 sheet_w 的图集取 (sx,sy,w,h) 区域拷到 fb 的 (dx,dy)（不透明，地形用） */
void blit(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
          const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h);

/* 同上但跳过纯黑像素（0x0000 视为透明，角色/怪物精灵用） */
void blit_masked(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
                 const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h);

/* 纯色小方块（掉落物金币占位用） */
void fill_rect(uint16_t* fb, int fb_w, int fb_h, int dx, int dy, int w, int h,
               uint16_t color);

}  /* namespace gfx */
}  /* namespace dg */

#endif
