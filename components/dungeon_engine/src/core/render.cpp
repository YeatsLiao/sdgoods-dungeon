/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * render.cpp —— 上游风真视觉渲染（缝合墙 / 水 / 门 / 楼梯 + 精灵动画 + 特效）
 *
 * 对齐上游 DungeonTileSheet 的「缝合」结论：墙不是一格一贴图，而是按四邻是否
 * 实体在 FLAT / RAISED / OVERHANG 三段里挑格，楼梯口/门套到凸墙上；水按四邻
 * 是否同为水做掩码变体。段选择不可能 100% 复刻上游 16 位内部掩码表（那是
 * 256 格全预烘焙），但取它「墙成块、边缘有收口」的观感即可，代价是一段纯逻辑。
 *
 * 帧结构：地形层（tile 对齐）→ 掉落层（items.png 色键叠加）→ 怪物/英雄层
 * （精灵帧，带亚 tile 平滑位移 + 受击闪白）→ 光束层 → 飘字层。记忆态
 * （explored 但当前不可见）只画地形暗化副本，不画实体（对齐上游「看不见就不显示」）。
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

/* 把一格地形映射到 tileset 格子号。返回 -1 表示不画（空气）。
 *
 * 严格对齐上游 DungeonTerrainTilemap.getTileVisual + DungeonTileSheet：上游用
 * 两张叠加 tilemap（flat 底 + raised 墙面罩层）表达墙体高度感；本移植是单
 * 缓冲一层，所以把「一个墙格最终该长什么样」折叠成一次判定：
 *   · 墙 + 南面是地  → RAISED 墙面（getRaisedWallTile：+1 右开 / +2 左开）
 *   · 墙 + 南面是墙但下对角有地 → OVERHANG 悬挑（+1 右下开 / +2 左下开）
 *   · 墙 + 南面是墙但左右有地   → INTERNAL 内墙（+1 右开 / +8 左开）
 *   · 墙 + 四周皆墙            → FLAT 整块墙
 * ALT / DECO 变体用生成期算好的 variant 哈希按上游 getVisualWithAlts 的口径挑。
 * 之前把 RAISED 判成「北面开放」（上下反了）、OVERHANG 用错掩码、水缝合位取反，
 * 是墙看起来「面朝天花板、水边全是毛刺」的根因。 */
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
    case DG_TERR_SECRET:
    case DG_TERR_STATUE: {
        const bool south_floor = !lv->solid(x, y + 1);
        const bool east_floor  = !lv->solid(x + 1, y);
        const bool west_floor  = !lv->solid(x - 1, y);
        const bool se_floor    = !lv->solid(x + 1, y + 1);
        const bool sw_floor    = !lv->solid(x - 1, y + 1);

        /* 雕像走专属格（上游 RAISED_STATUE / FLAT_STATUE） */
        if (terr == DG_TERR_STATUE) {
            return south_floor ? DTS_RAISED_STATUE : DTS_FLAT_STATUE;
        }

        if (south_floor) {
            /* RAISED：正立面。基准按 alt/deco 选，再叠左右开放掩码（+1右/+2左） */
            int base = deco ? DTS_RAISED_WALL_DECO
                     : alt  ? DTS_RAISED_WALL_ALT
                     :        DTS_RAISED_WALL;
            int m = (east_floor ? 1 : 0) | (west_floor ? 2 : 0);
            return base + m;
        }
        if (se_floor || sw_floor) {
            /* OVERHANG：墙体下缘对角见地，画悬挑檐（+1右下/+2左下） */
            int base = deco ? DTS_OVERHANG_DECO : DTS_WALLS_OVERHANG;
            int m = (se_floor ? 1 : 0) | (sw_floor ? 2 : 0);
            return base + m;
        }
        if (east_floor || west_floor) {
            /* INTERNAL：夹在墙块里、左右见缝（+1右开/+8左开） */
            int m = (east_floor ? 1 : 0) | (west_floor ? 8 : 0);
            return DTS_WALLS_INTERNAL + m;
        }
        /* 四面皆墙：一整块平墙 */
        if (deco) return DTS_FLAT_WALL_DECO;
        return alt ? DTS_FLAT_WALL_ALT : DTS_FLAT_WALL;
    }

    case DG_TERR_FLOOR:
        if (deco) return alt ? DTS_FLOOR_DECO_ALT : DTS_FLOOR_DECO;
        if (variant % 5 == 0) return DTS_FLOOR_ALT_1;
        if (variant % 5 == 1) return DTS_FLOOR_ALT_2;
        return DTS_FLOOR;

    case DG_TERR_ENTRY:
        return DTS_ENTRANCE;
    case DG_TERR_EXIT:
        /* 楼梯口若南面是墙，上游把出口垫到悬挑下（EXIT_UNDERHANG）*/
        return lv->solid(x, y + 1) ? DTS_EXIT_UNDERHANG : DTS_EXIT;
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
    if (h->flash_ticks > 0)
        gfx::blit_flash(fb, W, H, dx, dy, px, sw, 16, 0, 12, 15);
    else
        gfx::blit_key(fb, W, H, dx, dy, px, sw, 16, 0, 12, 15);

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

    /* 5. 特效层 */
    draw_beams(g, fb, W, H);
    draw_floats(g, fb, W, H);
}

}  /* namespace render */
}  /* namespace dg */
