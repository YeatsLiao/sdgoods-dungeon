/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * render.cpp —— 上游风真视觉渲染（缝合墙 / 水 / 门 / 楼梯 + 精灵动画 + 特效）
 *
 * 对齐上游 DungeonTileSheet 的「缝合」结论：上游是三层合成——
 *   1. 地形层（DungeonTerrainTilemap）：墙格只在南面是地/门时画 RAISED 正立面，
 *      墙体暗部不画（墙区内部就是黑的）；
 *   2. 实体层：掉落/怪物/英雄；
 *   3. 墙覆盖层（DungeonWallsTilemap，叠在实体之上）：南面是墙的墙格画
 *      INTERNAL 细棱条，南面是墙的地格画 OVERHANG 悬挑檐，门楣/出口檐也在
 *      这层。之前单层选格把墙暗部画成整块 FLAT 砖、把檐画到墙上，是「围墙
 *      一大坨亮砖 + 侧缘孤零零细条」的根因。
 *
 * 帧结构：地形层（tile 对齐）→ 掉落层（items.png 色键叠加）→ 怪物/英雄层
 * （精灵帧，带亚 tile 平滑位移 + 受击闪白）→ 墙覆盖层 → 光束层 → 飘字层。
 * 记忆态（explored 但当前不可见）只画地形暗化副本，不画实体（对齐上游
 * 「看不见就不显示」）。
 */
#include "dg_types.h"
#include "core/render.h"
#include "gfx/gfx.h"
#include "dg_icons.h"
#include <cstdio>
#include <cstring>

namespace dg {
namespace render {

static const int kFloatLifeMs = 750;   /* 飘字存活 */
static const int kBeamLifeMs  = 260;   /* 光束存活 */

/* 四邻实体位（上右下左），用于缝合选格 */
static inline int solid_mask(Level* lv, int x, int y)
{
    int m = 0;
    if (lv->solid(x, y - 1)) m |= 1;   /* top    */
    if (lv->solid(x + 1, y)) m |= 2;   /* right  */
    if (lv->solid(x, y + 1)) m |= 4;   /* bottom */
    if (lv->solid(x - 1, y)) m |= 8;   /* left   */
    return m;
}

/* 上游 wallStitcheable：参与墙缝合的地形（本移植只有墙 / 暗门）。图外按墙。 */
static inline bool stitcheable(Level* lv, int x, int y)
{
    if (x < 0 || x >= DG_MAP_W || y < 0 || y >= DG_MAP_H) return true;
    dg_terrain_t t = (dg_terrain_t)lv->at(x, y).terr;
    return t == DG_TERR_WALL || t == DG_TERR_SECRET;
}

static inline bool is_door_terr(dg_terrain_t t)
{
    return t == DG_TERR_DOOR || t == DG_TERR_LOCKED_DOOR || t == DG_TERR_OPEN_DOOR;
}

/* 把一格地形映射到 tileset 格子号。返回 -1 表示不画（空气）。
 *
 * 墙格（底层）：只画正立面——上游 getRaisedWallTile 南面是墙就返回 -1
 * （暗部留给覆盖层的 INTERNAL 细棱条），南面是门画 RAISED_WALL_DOOR。
 * 之前把「四面皆墙」画成整块 FLAT 砖，导致墙区一片亮砖、和上游
 * 「暗部 + 棱条」的观感完全不同。 */
static int terrain_index(Level* lv, int x, int y, uint8_t variant)
{
    Tile& t = lv->at(x, y);
    dg_terrain_t terr = (dg_terrain_t)t.terr;

    /* variant → 装饰/变体挑拣（约 1/6 变体、1/16 装饰，接近上游随机 alts） */
    const bool alt  = (variant % 6 == 0);
    const bool deco = (variant % 16 == 0);

    switch (terr) {
    case DG_TERR_EMPTY:
        return -1;

    case DG_TERR_WALL:
    case DG_TERR_SECRET: {
        const bool east_floor = !lv->solid(x + 1, y);
        const bool west_floor = !lv->solid(x - 1, y);

        /* 雕像走专属格（上游 RAISED_STATUE / FLAT_STATUE） */
        if (terr == DG_TERR_STATUE) {
            const bool sf = !lv->solid(x, y + 1);
            return sf ? DTS_RAISED_STATUE : DTS_FLAT_STATUE;
        }

        const Tile& b = lv->at(x, y + 1);
        const bool south_door = is_door_terr((dg_terrain_t)b.terr);
        if (!south_door && lv->solid(x, y + 1))
            return -1;                      /* 墙体暗部：覆盖层补棱条 */

        /* RAISED：正立面。门上方画门框墙；其余按 alt/deco 选基准，
         * 再叠左右开放掩码（+1 右开 / +2 左开，getRaisedWallTile） */
        int m = (east_floor ? 1 : 0) | (west_floor ? 2 : 0);
        if (south_door) return DTS_RAISED_WALL_DOOR + m;
        if (deco) return DTS_RAISED_WALL_DECO + m;
        if (alt)  return DTS_RAISED_WALL_ALT + m;
        return DTS_RAISED_WALL + m;
    }

    case DG_TERR_FLOOR:
        if (deco) return alt ? DTS_FLOOR_DECO_ALT : DTS_FLOOR_DECO;
        if (variant % 5 == 0) return DTS_FLOOR_ALT_1;
        if (variant % 5 == 1) return DTS_FLOOR_ALT_2;
        return DTS_FLOOR;

    case DG_TERR_ENTRY:
        return DTS_ENTRANCE;
    case DG_TERR_EXIT:
        /* 楼梯本体始终画；南面是墙时由墙覆盖层叠 EXIT_UNDERHANG 檐
         * （之前直接换成本檐格，楼梯口会“消失”） */
        return DTS_EXIT;
    case DG_TERR_PEDESTAL:
        return DTS_PEDESTAL;

    case DG_TERR_WATER: {
        /* 上游 stitchWaterTile：邻居「仍是水(或可缝水面)」时置位，挑收口格 */
        int conn = 0;
        if (lv->water_at(x, y - 1))     conn |= DTS_WATER_TOP;
        if (lv->water_at(x + 1, y))     conn |= DTS_WATER_RIGHT;
        if (lv->water_at(x, y + 1))     conn |= DTS_WATER_BOTTOM;
        if (lv->water_at(x - 1, y))     conn |= DTS_WATER_LEFT;
        return DTS_WATER + (conn & 15);
    }

    case DG_TERR_GRASS:
        return alt ? DTS_GRASS_ALT : DTS_GRASS;
    case DG_TERR_HIGH_GRASS:
        return alt ? DTS_HIGH_GRASS_ALT : DTS_HIGH_GRASS;

    case DG_TERR_TRAP:
        /* 未发现按地板画；已揭露画特殊地板警戒纹 */
        return t.trap_known ? DTS_FLOOR_SP : DTS_FLOOR;

    case DG_TERR_DOOR:
    case DG_TERR_LOCKED_DOOR:
    case DG_TERR_OPEN_DOOR: {
        /* 门上方向决定形态：南面是墙→门在竖直走廊里画成地板侧门；
         * 否则按关/开/锁挑凸门正面（上游 getRaisedDoorTile）*/
        if (lv->solid(x, y + 1)) return DTS_RAISED_DOOR_SIDEWAYS;
        if (terr == DG_TERR_OPEN_DOOR)   return DTS_RAISED_DOOR_OPEN;
        if (terr == DG_TERR_LOCKED_DOOR) return DTS_RAISED_DOOR_LOCKED;
        return DTS_RAISED_DOOR;
    }

    case DG_TERR_CHEST:
        return DTS_FLOOR;   /* 地形层铺地板，宝箱精灵在物品层叠 */

    default:
        return DTS_FLOOR;
    }
}

/* 上游 DungeonWallsTilemap 覆盖层选格（叠在实体之上，返回 -1 不画）：
 *   墙格 + 南面是墙 → INTERNAL 细棱条（+1 右开 / +2 右下开 / +4 左下开 / +8 左开）
 *   墙格 + 南面是门 → DOOR_SIDEWAYS 门楣
 *   地格 + 南面是墙 → OVERHANG 悬挑檐（+1 右下开 / +2 左下开）；门格用门楣檐
 *   地格 + 南面是门 → DOOR_OVERHANG；出口格南面是墙 → EXIT_UNDERHANG */
static int walls_overlay_index(Level* lv, int x, int y)
{
    Tile& t = lv->at(x, y);
    dg_terrain_t terr = (dg_terrain_t)t.terr;

    if (stitcheable(lv, x, y)) {
        if (!stitcheable(lv, x, y + 1)) {
            if (is_door_terr((dg_terrain_t)lv->at(x, y + 1).terr))
                return DTS_DOOR_SIDEWAYS;
            return -1;                       /* 正立面已在底层画过 */
        }
        int m = (!stitcheable(lv, x + 1, y)         ? 1 : 0) |
                (!stitcheable(lv, x + 1, y + 1)     ? 2 : 0) |
                (!stitcheable(lv, x - 1, y + 1)     ? 4 : 0) |
                (!stitcheable(lv, x - 1, y)         ? 8 : 0);
        return DTS_WALLS_INTERNAL + m;
    }

    if (terr == DG_TERR_EXIT)
        return stitcheable(lv, x, y + 1) ? DTS_EXIT_UNDERHANG : -1;

    if (stitcheable(lv, x, y + 1)) {
        int m = (!stitcheable(lv, x + 1, y + 1) ? 1 : 0) |
                (!stitcheable(lv, x - 1, y + 1) ? 2 : 0);
        if (terr == DG_TERR_DOOR)        return DTS_DOOR_SIDEWAYS_OVERHANG_CLOSED + m;
        if (terr == DG_TERR_LOCKED_DOOR) return DTS_DOOR_SIDEWAYS_OVERHANG_LOCKED + m;
        if (terr == DG_TERR_OPEN_DOOR)   return DTS_DOOR_SIDEWAYS_OVERHANG + m;
        return DTS_WALLS_OVERHANG + m;
    }
    if (is_door_terr((dg_terrain_t)lv->at(x, y + 1).terr) &&
        lv->at(x, y + 1).terr != DG_TERR_OPEN_DOOR)
        return DTS_DOOR_OVERHANG;
    if (lv->at(x, y + 1).terr == DG_TERR_OPEN_DOOR)
        return DTS_DOOR_OVERHANG_OPEN;
    return -1;
}

/* 精灵在 tile 内的亚像素位置（move_anim 0..254 做 from→to 线性插值） */
static void sprite_xy(Actor* a, int cam_x, int cam_y, int& ox, int& oy)
{
    float t = (a->move_anim >= 255) ? 1.0f : (a->move_anim / 255.0f);
    float ix = a->from_x + (a->x - a->from_x) * t;
    float iy = a->from_y + (a->y - a->from_y) * t;
    ox = (int)((ix - cam_x) * DG_TILE_PX);
    oy = (int)((iy - cam_y) * DG_TILE_PX);
}

/* 一个怪物精灵块 */
static void draw_mob(Game& g, uint16_t* fb, int W, int H, Mob* m)
{
    Level* lv = g.level;
    Tile& t = lv->at(m->x, m->y);
    if (!t.vis_current) return;
    gfx::Sheet s = (gfx::Sheet)m->sheet;
    const uint16_t* px = gfx::px(s);
    if (!px) return;
    int sw = gfx::sheet_w(s);
    int frames = gfx::anim_frames(s);
    if (frames < 1) frames = 1;
    /* idle 帧：300ms 换一帧 + anim_seed 打散，避免全场同拍 */
    int fx = (int)((g.anim_ms / 300 + m->anim_seed) % (uint32_t)frames);

    int ox, oy;
    sprite_xy(m, g.cam_x, g.cam_y, ox, oy);
    int dx = ox, dy = oy;

    if (m->flash_ticks > 0)
        gfx::blit_flash(fb, W, H, dx, dy, px, sw, fx * 16, 0, 16, 16);
    else
        gfx::blit_key(fb, W, H, dx, dy, px, sw, fx * 16, 0, 16, 16);

    /* 血条：受伤才画，贴在脚下（上游是头顶，圆屏脚下更不挡视野） */
    if (m->hp < m->hp_max) {
        int frac = (int)((long)m->hp * 14 / m->hp_max);
        gfx::fill_rect(fb, W, H, dx + 1, dy + 15, 14, 1, 0x0000);
        gfx::fill_rect(fb, W, H, dx + 1, dy + 15, frac, 1, 0xF800);
    }
}

/* 英雄精灵 */
static void draw_hero(Game& g, uint16_t* fb, int W, int H)
{
    Hero* h = g.hero;
    gfx::Sheet s = (gfx::Sheet)h->sheet;
    const uint16_t* px = gfx::px(s);
    if (!px) {
        /* 缺英雄图集：保底画一个可见标记，别让主角"消失" */
        int ox, oy; sprite_xy(h, g.cam_x, g.cam_y, ox, oy);
        gfx::fill_rect(fb, W, H, ox + 4, oy + 2, 8, 12, 0x07E0);
        return;
    }
    int sw = gfx::sheet_w(s);
    int ox, oy;
    sprite_xy(h, g.cam_x, g.cam_y, ox, oy);
    int dx = ox + 2, dy = oy + 1;        /* 12×15 帧居中进 16 格 */
    /* 上游英雄图集是 12px 步长紧排帧（HeroSprite.FRAME_WIDTH=12），idle 帧为
     * 帧0（x=0..11）。之前误按 16px 网格取 x=16，正好横跨帧1尾+帧2头。 */
    if (h->flash_ticks > 0)
        gfx::blit_flash(fb, W, H, dx, dy, px, sw, 0, 0, 12, 15);
    else
        gfx::blit_key(fb, W, H, dx, dy, px, sw, 0, 0, 12, 15);

    /* 圆屏适配：英雄 HP 条贴着英雄脚下画（而非顶部固定条）——英雄永远在视口
     * 中心附近，中心区是圆屏唯一恒在安全区内的地方，所以血条「跟着人走」保证
     * 不被圆形边框裁掉。>1/3 绿、否则红。 */
    if (h->hp_max > 0) {
        int frac = (int)((long)h->hp * 14 / h->hp_max);
        uint16_t col = (h->hp * 3 >= h->hp_max) ? 0x07E0 : 0xF800;
        gfx::fill_rect(fb, W, H, ox + 1, oy + 15, 14, 1, 0x0000);
        gfx::fill_rect(fb, W, H, ox + 1, oy + 15, frac, 1, col);
    }
}

/* 掉落物（items.png 色键叠加） */
static void draw_item(Game& g, uint16_t* fb, int W, int H, int x, int y, Item* it)
{
    const uint16_t* px = gfx::px(gfx::SH_ITEMS);
    if (!px) return;
    int sw = gfx::sheet_w(gfx::SH_ITEMS);
    int idx = it->icon;
    int sx = (idx % DG_SHEET_COLS) * DG_SHEET_CELL;
    int sy = (idx / DG_SHEET_COLS) * DG_SHEET_CELL;
    int dx = (x - g.cam_x) * DG_TILE_PX;
    int dy = (y - g.cam_y) * DG_TILE_PX;
    gfx::blit_key(fb, W, H, dx, dy, px, sw, sx, sy, 16, 16);
}

/* 光束（激光/投矛），按年龄淡出 */
static void draw_beams(Game& g, uint16_t* fb, int W, int H)
{
    for (int i = 0; i < Game::kMaxBeams; i++) {
        Game::Beam& b = g.beams[i];
        if (!b.used) continue;
        int age = (int)(g.anim_ms - b.born_ms);
        if (age >= kBeamLifeMs) { b.used = false; continue; }
        int x0 = (b.x0 - g.cam_x) * DG_TILE_PX + 8;
        int y0 = (b.y0 - g.cam_y) * DG_TILE_PX + 8;
        int x1 = (b.x1 - g.cam_x) * DG_TILE_PX + 8;
        int y1 = (b.y1 - g.cam_y) * DG_TILE_PX + 8;
        int ddx = x1 - x0, ddy = y1 - y0;
        int adx = ddx < 0 ? -ddx : ddx, ady = ddy < 0 ? -ddy : ddy;
        int steps = adx > ady ? adx : ady;
        if (steps == 0) steps = 1;
        for (int s = 0; s <= steps; s++) {
            int px = x0 + ddx * s / steps;
            int py = y0 + ddy * s / steps;
            if (px < 0 || px >= W || py < 0 || py >= H) continue;
            fb[py * W + px] = b.color;
            if (px + 1 < W) fb[py * W + px + 1] = b.color;
        }
    }
}

/* 飘字（伤害/MISS/LV），向上飘并超时消失 */
static void draw_floats(Game& g, uint16_t* fb, int W, int H)
{
    for (int i = 0; i < Game::kMaxFloats; i++) {
        Game::FloatText& f = g.floats[i];
        if (!f.used) continue;
        int age = (int)(g.anim_ms - f.born_ms);
        if (age >= kFloatLifeMs) { f.used = false; continue; }
        int rise = age / 40;                         /* 每 40ms 上移 1px */
        int cx = (f.x - g.cam_x) * DG_TILE_PX + 8 - gfx::text_px_w(f.text) / 2;
        int cy = (f.y - g.cam_y) * DG_TILE_PX - rise;
        gfx::text_px(fb, W, H, cx, cy, f.text, f.color);
        g.anim_running = true;
    }
}

/* ===== 主渲染 ===== */
void frame(Game& g, uint16_t* fb, int W, int H)
{
    Level* lv = g.level;
    Hero* h = g.hero;
    if (!lv || !h) return;

    /* 相机：英雄居中，clamp 到地图内（视口 11×11，多画 1 行 1 列吃边缘） */
    g.cam_x = h->x - (DG_VIEW_TILE_W / 2);
    g.cam_y = h->y - (DG_VIEW_TILE_H / 2);
    if (g.cam_x < 0) g.cam_x = 0;
    if (g.cam_y < 0) g.cam_y = 0;
    if (g.cam_x > DG_MAP_W - DG_VIEW_TILE_W) g.cam_x = DG_MAP_W - DG_VIEW_TILE_W;
    if (g.cam_y > DG_MAP_H - DG_VIEW_TILE_H) g.cam_y = DG_MAP_H - DG_VIEW_TILE_H;

    int chapter = lv->chapter();
    const uint16_t* ts     = gfx::tileset(chapter);
    const uint16_t* ts_dim = gfx::tileset_dim(chapter);

    const int cols = DG_VIEW_TILE_W + 1;
    const int rows = DG_VIEW_TILE_H + 1;

    /* 1. 地形层 */
    for (int py = 0; py < rows; py++) {
        for (int px = 0; px < cols; px++) {
            int wx = g.cam_x + px, wy = g.cam_y + py;
            int dx = px * DG_TILE_PX, dy = py * DG_TILE_PX;
            if (wx < 0 || wx >= DG_MAP_W || wy < 0 || wy >= DG_MAP_H) {
                gfx::fill_rect(fb, W, H, dx, dy, DG_TILE_PX, DG_TILE_PX, 0x0000);
                continue;
            }
            Tile& t = lv->at(wx, wy);
            int idx = terrain_index(lv, wx, wy, t.variant);
            if (idx < 0 || !ts) {
                gfx::fill_rect(fb, W, H, dx, dy, DG_TILE_PX, DG_TILE_PX, 0x0000);
                continue;
            }
            int sx = (idx % DG_SHEET_COLS) * DG_SHEET_CELL;
            int sy = (idx / DG_SHEET_COLS) * DG_SHEET_CELL;
            if (t.vis_current) {
                gfx::blit(fb, W, H, dx, dy, ts, 256, sx, sy, DG_TILE_PX, DG_TILE_PX);
            } else if (t.explored || t.vis_seen) {
                gfx::blit(fb, W, H, dx, dy, ts_dim, 256, sx, sy, DG_TILE_PX, DG_TILE_PX);
            } else {
                gfx::fill_rect(fb, W, H, dx, dy, DG_TILE_PX, DG_TILE_PX, 0x0000);
            }
        }
    }

    /* 2. 掉落层（仅当前可见；宝箱用 chest 图标叠在地形上） */
    for (int py = 0; py < rows; py++) {
        for (int px = 0; px < cols; px++) {
            int wx = g.cam_x + px, wy = g.cam_y + py;
            if (wx < 0 || wx >= DG_MAP_W || wy < 0 || wy >= DG_MAP_H) continue;
            Tile& t = lv->at(wx, wy);
            if (!t.vis_current) continue;
            if (t.terr == DG_TERR_CHEST) {
                const uint16_t* ip = gfx::px(gfx::SH_ITEMS);
                if (ip) {
                    int ci = DGITEM_CHEST + (t.variant & 3);      /* 木/铜/银/金 */
                    gfx::blit_key(fb, W, H, px * DG_TILE_PX, py * DG_TILE_PX, ip,
                                  gfx::sheet_w(gfx::SH_ITEMS),
                                  (ci % DG_SHEET_COLS) * 16, (ci / DG_SHEET_COLS) * 16, 16, 16);
                }
            } else if (t.item) {
                draw_item(g, fb, W, H, wx, wy, t.item);
            }
        }
    }

    /* 3. 怪物层 */
    for (int i = 1; i < lv->actor_count; i++) {
        Actor* a = lv->actors[i];
        if (a && a->is_alive()) draw_mob(g, fb, W, H, (Mob*)a);
    }

    /* 4. 英雄层 */
    draw_hero(g, fb, W, H);

    /* 4.5 墙覆盖层（上游 DungeonWallsTilemap，叠在实体之上）：内墙棱条 /
     * 悬挑檐 / 门楣。放实体之后画，英雄贴墙北上时头部被墙正确遮住 */
    for (int py = 0; py < rows; py++) {
        for (int px = 0; px < cols; px++) {
            int wx = g.cam_x + px, wy = g.cam_y + py;
            if (wx < 0 || wx >= DG_MAP_W || wy < 0 || wy >= DG_MAP_H) continue;
            Tile& t = lv->at(wx, wy);
            const bool vis = t.vis_current;
            if (!vis && !(t.explored || t.vis_seen)) continue;
            int ov = walls_overlay_index(lv, wx, wy);
            if (ov < 0) continue;
            int sx = (ov % DG_SHEET_COLS) * DG_SHEET_CELL;
            int sy = (ov / DG_SHEET_COLS) * DG_SHEET_CELL;
            gfx::blit_key(fb, W, H, px * DG_TILE_PX, py * DG_TILE_PX,
                          vis ? ts : ts_dim, 256, sx, sy, DG_TILE_PX, DG_TILE_PX);
        }
    }

    /* 5. 特效层 */
    draw_beams(g, fb, W, H);
    draw_floats(g, fb, W, H);
}

}  /* namespace render */
}  /* namespace dg */
