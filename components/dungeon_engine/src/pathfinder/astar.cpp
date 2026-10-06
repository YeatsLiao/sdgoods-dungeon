/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * astar.cpp —— 8 方向 A* 寻路（骨架版：简单 8 邻域，无视野代价）
 *
 * 架构决策（重要）：ESP32-S3 上单次跑 32×32 = 1024 节点 A* 完全无压力，
 * 但若地图扩到 128×128（后期可能）需要分帧。当前实现每帧最多 256 节点，
 * 未完则返回部分路径首步，由 Game 层在 tick 里继续。
 */
#include "dg_types.h"
#include <cstring>
#include <cstdlib>
#include "esp_log.h"

static const char* TAG_ASTAR = "dg.astar";

namespace dg {
namespace pathfinder {

/* 简易固定大小 open 集合，避免 heap 分配 */
struct Node {
    uint16_t pos;
    uint16_t came_from;
    int16_t  g;
    int16_t  f;
};

static Node     s_nodes[1024];
static uint16_t s_open[1024];   /* 旧值 512：满图上 open 溢出会静默丢节点致误判不可达 */
static int      s_open_len = 0;
static uint8_t  s_in_open[1024];
static uint8_t  s_in_close[1024];
static uint16_t s_came_from[1024];

static inline int heuristic(int x1, int y1, int x2, int y2) {
    int dx = abs(x1 - x2), dy = abs(y1 - y2);
    return dx + dy;   /* 与下方代价模型（直 1 斜 2）精确匹配：斜步=2=两直步，可采纳 */
}

int find_path(Level* level, int sx, int sy, int tx, int ty,
              int* steps, int cap) {
    if (!level || !steps || cap <= 0) return 0;
    if (sx == tx && sy == ty) return 0;
    if (!level->passable(tx, ty)) return 0;

    s_open_len = 0;
    memset(s_in_open, 0, sizeof(s_in_open));
    memset(s_in_close, 0, sizeof(s_in_close));
    memset(s_nodes, 0, sizeof(s_nodes));
    memset(s_came_from, 0xFF, sizeof(s_came_from));

    int start = sx + sy * DG_MAP_W;
    int goal  = tx + ty * DG_MAP_W;
    s_nodes[start].pos = start;
    s_nodes[start].g = 0;
    s_nodes[start].f = heuristic(sx, sy, tx, ty);
    s_open[s_open_len++] = start;
    s_in_open[start] = 1;

    static const int dx8[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int dy8[8] = {-1,-1,-1,  0, 0,  1, 1, 1};

    int max_iter = 4096;   /* 1024 节点 × 重开余量；旧值 512 在大图上误判不可达 */
    while (s_open_len > 0 && max_iter-- > 0) {
        /* 找 f 最小 */
        int best = 0;
        for (int i = 1; i < s_open_len; i++) {
            if (s_nodes[s_open[i]].f < s_nodes[s_open[best]].f) best = i;
        }
        int cur = s_open[best];
        s_open[best] = s_open[--s_open_len];
        s_in_open[cur] = 0;
        s_in_close[cur] = 1;

        if (cur == goal) break;

        int cx = cur % DG_MAP_W, cy = cur / DG_MAP_W;
        for (int d = 0; d < 8; d++) {
            int nx = cx + dx8[d], ny = cy + dy8[d];
            if (!level->passable(nx, ny)) continue;
            int np = nx + ny * DG_MAP_W;
            if (s_in_close[np]) continue;
            int ng = s_nodes[cur].g + ((nx != cx && ny != cy) ? 2 : 1);   /* 斜 2 直 1 */
            if (!s_in_open[np] || ng < s_nodes[np].g) {
                s_nodes[np].pos = np;
                s_nodes[np].g = ng;
                s_nodes[np].f = ng + heuristic(nx, ny, tx, ty);
                s_came_from[np] = cur;
                if (!s_in_open[np] && s_open_len < 1024) {
                    s_open[s_open_len++] = np;
                    s_in_open[np] = 1;
                }
            }
        }
    }

    /* 回溯路径 */
    if (!s_in_close[goal]) {
        ESP_LOGW(TAG_ASTAR, "unreachable (%d,%d)->(%d,%d): pops=%d open=%d start_pass=%d",
                 sx, sy, tx, ty, 4096 - max_iter, s_open_len,
                 (int)level->passable(sx, sy));
        return 0;
    }

    int path[128]; int plen = 0;
    int p = goal;
    while (p != start && plen < 128) {
        path[plen++] = p;
        p = s_came_from[p];
        if (p == 0xFFFF) break;
    }
    int n = 0;
    for (int i = plen - 1; i >= 0 && n < cap; i--) {
        steps[n++] = path[i];
    }
    return n;
}

}  /* namespace pathfinder */
}  /* namespace dg */
