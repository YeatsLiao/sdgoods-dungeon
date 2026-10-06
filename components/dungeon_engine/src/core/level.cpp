/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * level.cpp —— 单层地图与程序化生成（骨架版：随机房间 + 走廊连通）
 *
 * 后续 v0.1 正式实现：Shattered 的 Room / Connector 分层 + 秘密门 +
 * 陷阱布点 + 特殊房间（商店 / 藏宝图 / 炼金）。当前只提供 tiles 数组
 * 与 entrance/exit 定位，够骨架跑起来。
 */
#include "dg_types.h"
#include "rng/java_random.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "dg.level";

namespace dg {

bool Level::passable(int x, int y) const {
    if (x < 0 || x >= DG_MAP_W || y < 0 || y >= DG_MAP_H) return false;
    dg_terrain_t t = tiles[x + y * DG_MAP_W].terr;
    return t != DG_TERR_WALL && t != DG_TERR_EMPTY && t != DG_TERR_STATUE;
}

int Level::distance(int ax, int ay, int bx, int by) const {
    int dx = ax - bx, dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;    /* Chebyshev（8 方向）*/
}

bool Level::generate(uint32_t seed) {
    ESP_LOGI(TAG, "generate seed=0x%08x", seed);
    JavaRandom r(seed);
    memset(tiles, 0, sizeof(tiles));

    /* 全部初始化为 EMPTY（不可通行的墙外） */
    for (int i = 0; i < LENGTH; i++) tiles[i].terr = DG_TERR_EMPTY;

    /* 5-8 个随机矩形房间 + 中位走廊连通 */
    int room_count = 5 + r.nextInt(4);
    struct Rect { int x0, y0, x1, y1; };
    Rect rooms[8] = {0};
    for (int i = 0; i < room_count; i++) {
        int w = 4 + r.nextInt(6);
        int h = 4 + r.nextInt(6);
        int x0 = 1 + r.nextInt(DG_MAP_W - w - 2);
        int y0 = 1 + r.nextInt(DG_MAP_H - h - 2);
        rooms[i] = { x0, y0, x0 + w - 1, y0 + h - 1 };

        /* 挖 FLOOR */
        for (int y = y0; y <= y0 + h - 1; y++) {
            for (int x = x0; x <= x0 + w - 1; x++) {
                tiles[x + y * DG_MAP_W].terr = DG_TERR_FLOOR;
            }
        }
        /* 房间外墙（若相邻是 EMPTY 就设 WALL） */
        for (int y = y0 - 1; y <= y0 + h; y++) {
            for (int x = x0 - 1; x <= x0 + w; x++) {
                if (x < 0 || x >= DG_MAP_W || y < 0 || y >= DG_MAP_H) continue;
                Tile& t = tiles[x + y * DG_MAP_W];
                if (t.terr == DG_TERR_EMPTY) t.terr = DG_TERR_WALL;
            }
        }
    }

    /* 相邻房间用 L 型走廊打通 */
    for (int i = 0; i < room_count - 1; i++) {
        int cx1 = (rooms[i].x0 + rooms[i].x1) / 2;
        int cy1 = (rooms[i].y0 + rooms[i].y1) / 2;
        int cx2 = (rooms[i+1].x0 + rooms[i+1].x1) / 2;
        int cy2 = (rooms[i+1].y0 + rooms[i+1].y1) / 2;
        int x = cx1, y = cy1;
        while (x != cx2) {
            if (tiles[x + y * DG_MAP_W].terr != DG_TERR_FLOOR)
                tiles[x + y * DG_MAP_W].terr = DG_TERR_FLOOR;
            x += (cx2 > x) ? 1 : -1;
        }
        while (y != cy2) {
            if (tiles[x + y * DG_MAP_W].terr != DG_TERR_FLOOR)
                tiles[x + y * DG_MAP_W].terr = DG_TERR_FLOOR;
            y += (cy2 > y) ? 1 : -1;
        }
    }

    /* entrance 在第一个房间中心，exit 在最后一个房间中心 */
    entrance_pos = ((rooms[0].y0 + rooms[0].y1) / 2) * DG_MAP_W
                 + ((rooms[0].x0 + rooms[0].x1) / 2);
    exit_pos     = ((rooms[room_count-1].y0 + rooms[room_count-1].y1) / 2) * DG_MAP_W
                 + ((rooms[room_count-1].x0 + rooms[room_count-1].x1) / 2);

    /* 全部标记 explored（骨架不做 FOV） */
    for (int i = 0; i < LENGTH; i++) {
        tiles[i].explored = 1;
        tiles[i].vis_current = 1;
        tiles[i].vis_seen = 1;
    }
    tiles[entrance_pos].terr = DG_TERR_ENTRY;
    tiles[exit_pos].terr = DG_TERR_EXIT;

    actor_count = 0;
    return true;
}

void Level::create_mobs_and_items() {
    /* TODO: v0.1 章节怪组；骨架版暂空 */
    ESP_LOGI(TAG, "create_mobs_and_items: TODO");
}

}  /* namespace dg */
