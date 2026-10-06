/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * fov.cpp —— 视野计算（逐格视线版）
 *
 * 实现选型（工程决策）：不用教科书递归阴影投射的 8 象限通用式——该式
 * 系数极易写错且半径 8 的 32×32 小图下收益为零；改为对视野圆内每格
 * 做一次高分辨率采样视线（与怪物唤醒共用同一条遮挡判定），
 * 语义 = 「能看到当且仅当视线未被挡」，可验证性优先。
 * 全量重算成本：~200 格 × ≤32 采样 ≈ 6400 次查表，<0.1ms，回合制够用。
 */
#include "fov/fov.h"

namespace dg {
namespace fov {

/* 遮挡视线的地形：实体墙、未挖通的地图外、装饰柱、关着的门 */
static inline bool blocks_light(Level* lv, int x, int y) {
    if (x < 0 || x >= DG_MAP_W || y < 0 || y >= DG_MAP_H) return true;
    dg_terrain_t t = lv->at(x, y).terr;
    return t == DG_TERR_WALL || t == DG_TERR_EMPTY ||
           t == DG_TERR_STATUE || t == DG_TERR_DOOR || t == DG_TERR_SECRET;
}

/* 从 (px,py) 到 (tx,ty) 的视线是否被挡。终点格自身不参与遮挡判定
 * （玩家贴着墙站时墙应可见）。采样 4×dmax 个点保证不漏格。 */
static bool line_blocked(Level* lv, int px, int py, int tx, int ty) {
    int dx = tx - px, dy = ty - py;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int dmax = adx > ady ? adx : ady;
    if (dmax <= 1) return false;             /* 自身 / 紧邻格恒可见 */
    int steps = dmax * 4;
    int last_cx = px, last_cy = py;
    for (int i = 1; i < steps; i++) {
        float t = (float)i / (float)steps;
        int cx = px + (int)(dx * t + (dx >= 0 ? 0.5f : -0.5f));
        int cy = py + (int)(dy * t + (dy >= 0 ? 0.5f : -0.5f));
        if (cx == last_cx && cy == last_cy) continue;
        last_cx = cx; last_cy = cy;
        if (cx == tx && cy == ty) break;     /* 到终点，终点不挡视线 */
        if (blocks_light(lv, cx, cy)) return true;
    }
    return false;
}

void compute(Level* level, int px, int py, int radius) {
    /* 清当前可见位（explored 是持久记忆，不清） */
    for (int i = 0; i < Level::LENGTH; i++) {
        level->tiles[i].vis_current = 0;
    }
    level->at(px, py).vis_current = 1;
    level->at(px, py).explored = 1;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy > radius * radius + radius) continue;  /* 近圆视野 */
            int tx = px + dx, ty = py + dy;
            if (tx < 0 || tx >= DG_MAP_W || ty < 0 || ty >= DG_MAP_H) continue;
            if (!line_blocked(level, px, py, tx, ty)) {
                level->at(tx, ty).vis_current = 1;
                level->at(tx, ty).explored = 1;
            }
        }
    }
}

bool visible_between(Level* level, int x0, int y0, int x1, int y1) {
    return !line_blocked(level, x0, y0, x1, y1);
}

}  /* namespace fov */
}  /* namespace dg */
