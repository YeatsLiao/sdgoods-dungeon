/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * render.h —— 引擎内渲染器入口（dg_types.h 之外，仅供 core 内部调用）
 *
 * 渲染与状态分离：dungeon.cpp 只管回合流 / 数据 / 特效计时，把所有「往 fb 上
 * 画东西」的活儿集中到 render.cpp。这样 tile 缝合、精灵动画、插值、飘字这些
 * 纯视觉逻辑改起来不碰游戏逻辑，反过来也不会在逻辑里误改视野位。
 */
#ifndef DG_RENDER_H
#define DG_RENDER_H

#include "dg_types.h"

namespace dg {
namespace render {

/* 把当前 Game 状态画进 176×176 逻辑 fb（native RGB565）。
 * 由 Game::get_tilemap_fb 在 fb_dirty 时调用。 */
void frame(Game& g, uint16_t* fb, int fb_w, int fb_h);

}  /* namespace render */
}  /* namespace dg */

#endif
