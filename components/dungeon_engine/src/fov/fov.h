/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * fov.h —— 视野计算（引擎内部头）
 *
 * 逐格视线版：对视野圆内每格做高分辨率采样视线判定，语义 =
 * 「能看到当且仅当视线未被墙挡」。32×32 地图半径 8 全量重算
 * < 0.1ms，无需增量缓存。详见 fov.cpp 头注释的选型理由。
 */
#ifndef DG_FOV_H
#define DG_FOV_H

#include "dg_types.h"

namespace dg {
namespace fov {

/* 以 (px,py) 为中心重算 level 的 vis_current：半径内视线未被遮挡的格
 * vis_current=1 且 explored=1（持久揭雾）。 */
void compute(Level* level, int px, int py, int radius);

/* 两点间视线是否畅通（怪物唤醒判定用，不改任何视野位） */
bool visible_between(Level* level, int x0, int y0, int x1, int y1);

}  /* namespace fov */
}  /* namespace dg */

#endif
