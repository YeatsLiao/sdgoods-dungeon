/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * level.cpp —— 章节化地图生成（房间 + 走廊 + 缝合友好整形）
 *
 * 上游是 Section/Room 分层（SewerSection → RegularRoom …），本移植取它的
 * 「形状结论」而非实现：房间矩形互不重叠 + 走廊按 unions 补连通（不强制
 * 树形，允许小环路，回合制肉鸽里环路能避免被堵死）。生成完做一次「墙整形」：
 * 把对穿有地板的孤立墙改成门 —— 这是上游 DungeonLevel  smoothing 的等价物，
 * 也是渲染器 FLAT/RAISED/OVERHANG 三段缝合看起来正常的先决条件（墙必须
 * 成块，房间必须有规整边界）。
 *
 * 确定性：generate(seed, depth) 内部 seed' = mix(seed, depth)，同一局
 * 下楼/读档重放同一张图（存档只存 seed + depth + 实体位置）。
 */
#include "dg_types.h"
#include "rng/java_random.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "dg.level";

namespace dg {

/* 房间表（生成期局部结构） */
struct Room { int x0, y0, x1, y1; };

static inline uint8_t tile_hash(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)(x * 73856093) ^ (uint32_t)(y * 19349663) ^ seed;
    h ^= h >> 13; h *= 0x5bd1e995; h ^= h >> 15;
    return (uint8_t)h;
}

/* ===== 查询 ===== */

bool Level::passable(int x, int y) const
{
    if (x < 0 || x >= DG_MAP_W || y < 0 || y >= DG_MAP_H) return false;
    dg_terrain_t t = (dg_terrain_t)tiles[x + y * DG_MAP_W].terr;
    switch (t) {
    case DG_TERR_EMPTY: case DG_TERR_WALL: case DG_TERR_SECRET:
    case DG_TERR_STATUE: case DG_TERR_LOCKED_DOOR: case DG_TERR_CHEST:
        return false;
    default:
        return true;
    }
}

bool Level::solid(int x, int y) const
{
    if (x < 0 || x >= DG_MAP_W || y < 0 || y >= DG_MAP_H) return true;  /* 图外按墙缝 */
    dg_terrain_t t = (dg_terrain_t)tiles[x + y * DG_MAP_W].terr;
    return t == DG_TERR_EMPTY || t == DG_TERR_WALL || t == DG_TERR_SECRET ||
           t == DG_TERR_STATUE;
}

bool Level::water_at(int x, int y) const
{
    if (x < 0 || x >= DG_MAP_W || y < 0 || y >= DG_MAP_H) return false;
    return tiles[x + y * DG_MAP_W].terr == DG_TERR_WATER;
}

int Level::distance(int ax, int ay, int bx, int by) const
{
    int dx = ax - bx, dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;    /* Chebyshev（8 方向）*/
}

int Level::chapter() const
{
    int c = depth / DG_CHAPTER_DEPTH;
    return c > 2 ? 2 : c;
}

/* ===== 演员表 ===== */

void Level::add_actor(Actor* a)
{
    if (actor_count >= 64) { ESP_LOGW(TAG, "actor table full"); return; }
    a->level = this;
    actors[actor_count++] = a;
}

void Level::del_actor(Actor* a)
{
    for (int i = 0; i < actor_count; i++) {
        if (actors[i] != a) continue;
        actors[i] = actors[actor_count - 1];
        actors[--actor_count] = nullptr;
        return;
    }
}

void Level::reset_view()
{
    for (int i = 0; i < LENGTH; i++) {
        Tile& t = tiles[i];
        t.vis_seen = 0;
        t.vis_magical = 0;
        t.vis_current = 0;
        t.explored = 0;
        t.trap_known = 0;
        t.chest_open = 0;
        t.item = nullptr;
        t.actor = nullptr;
    }
    actor_count = 0;
}

/* ===== 生成辅助 ===== */

static bool room_overlap(const Room& a, const Room& b)
{
    return a.x0 <= b.x1 + 1 && b.x0 <= a.x1 + 1 &&
           a.y0 <= b.y1 + 1 && b.y0 <= a.y1 + 1;
}

static int room_cx(const Room& r) { return (r.x0 + r.x1) / 2; }
static int room_cy(const Room& r) { return (r.y0 + r.y1) / 2; }

/* 在房间里随机找一个可用地板点（try 次内）。avoid_* 为必须远离的点。 */
static bool pick_in_room(Level* lv, JavaRandom& r, const Room& rm,
                         int tries, int ax, int ay, int min_d,
                         int* ox, int* oy)
{
    for (int i = 0; i < tries; i++) {
        int x = rm.x0 + r.nextInt(rm.x1 - rm.x0 + 1);
        int y = rm.y0 + r.nextInt(rm.y1 - rm.y0 + 1);
        if (lv->at(x, y).terr != DG_TERR_FLOOR) continue;
        if (lv->at(x, y).item || lv->at(x, y).actor) continue;
        if (ax >= 0 && lv->distance(x, y, ax, ay) < min_d) continue;
        *ox = x; *oy = y;
        return true;
    }
    return false;
}

/* 开敞内场：该格是 FLOOR 且四邻皆 FLOOR。固态装饰（雕像/宝箱）只放这种格，
 * 保证不落在 1 宽走廊口而切断图。 */
static bool open_field_at(Level* lv, int x, int y)
{
    if (x < 1 || y < 1 || x >= DG_MAP_W - 1 || y >= DG_MAP_H - 1) return false;
    if (lv->at(x, y).terr != DG_TERR_FLOOR) return false;
    return lv->at(x - 1, y).terr == DG_TERR_FLOOR && lv->at(x + 1, y).terr == DG_TERR_FLOOR &&
           lv->at(x, y - 1).terr == DG_TERR_FLOOR && lv->at(x, y + 1).terr == DG_TERR_FLOOR;
}

/* 洪水填充：从入口沿 passable() 真格能否走到出口（生成末尾的可达性兵底）。*/
static bool flood_exit_reachable(Level* lv)
{
    static bool vis[DG_MAP_W * DG_MAP_H];
    static int  qx[DG_MAP_W * DG_MAP_H], qy[DG_MAP_W * DG_MAP_H];
    int sx = lv->entrance_pos % DG_MAP_W, sy = lv->entrance_pos / DG_MAP_W;
    int tx = lv->exit_pos % DG_MAP_W,       ty = lv->exit_pos / DG_MAP_W;
    if (sx == tx && sy == ty) return false;
    memset(vis, 0, sizeof(vis));
    int head = 0, tail = 0;
    qx[tail] = sx; qy[tail] = sy; tail++;
    vis[sx + sy * DG_MAP_W] = true;
    static const int dx4[4] = { 0, 1, 0, -1 };
    static const int dy4[4] = { -1, 0, 1, 0 };
    while (head < tail) {
        int x = qx[head], y = qy[head]; head++;
        for (int k = 0; k < 4; k++) {
            int nx = x + dx4[k], ny = y + dy4[k];
            if (nx < 0 || ny < 0 || nx >= DG_MAP_W || ny >= DG_MAP_H) continue;
            int idx = nx + ny * DG_MAP_W;
            if (vis[idx]) continue;
            if ((nx != tx || ny != ty) && !lv->passable(nx, ny)) continue;
            vis[idx] = true; qx[tail] = nx; qy[tail] = ny; tail++;
        }
    }
    return vis[tx + ty * DG_MAP_W];
}

/* ===== 主生成 ===== */

bool Level::generate(uint32_t base_seed, int depth_)
{
    depth = depth_;
    seed = base_seed;
    /* 层间去相关：mix(seed, depth)。读档 = 同 seed + 同 depth 重放。 */
    uint32_t smix = (uint32_t)depth_ * 0x9E3779B9u;
    smix ^= (smix >> 16) * 0x21f0aaad;
    JavaRandom r((int)(base_seed ^ smix));

    memset(tiles, 0, sizeof(tiles));
    for (int i = 0; i < LENGTH; i++) tiles[i].terr = DG_TERR_EMPTY;

    /* --- 1. 房间：7~9 个矩形，彼此至少隔 1 格 --- */
    Room rooms[10];
    int room_count = 0;
    int want = 7 + r.nextInt(3);
    for (int tries = 0; tries < 120 && room_count < want; tries++) {
        int w = 4 + r.nextInt(6);
        int h = 4 + r.nextInt(6);
        int x0 = 1 + r.nextInt(DG_MAP_W - w - 2);
        int y0 = 1 + r.nextInt(DG_MAP_H - h - 2);
        Room nr = { x0, y0, x0 + w - 1, y0 + h - 1 };
        bool clash = false;
        for (int i = 0; i < room_count; i++) {
            if (room_overlap(nr, rooms[i])) { clash = true; break; }
        }
        if (clash) continue;
        for (int y = nr.y0; y <= nr.y1; y++)
            for (int x = nr.x0; x <= nr.x1; x++)
                tiles[x + y * DG_MAP_W].terr = DG_TERR_FLOOR;
        rooms[room_count++] = nr;
    }
    if (room_count < 3) return false;   /* 极端坏 seed：让调用方重试 */

    /* --- 2. 走廊：并集补连通（先生成 2 条随机短 L 走廊造环路） --- */
    int uni[10];
    for (int i = 0; i < room_count; i++) uni[i] = i;
    /* find 用局部 lambda 不便（-fno-exceptions 环境下保持 C 风格），写个小函数 */
    struct UF {
        static int find(int* p, int i) { while (p[i] != i) { p[i] = p[p[i]]; i = p[i]; } return i; }
        static void un(int* p, int a, int b) { a = find(p, a); b = find(p, b); if (a != b) p[b] = a; }
    };

    auto dig_h = [&](int x0, int x1, int y) {
        /* 方向归一：旧版没做 min/max，ax>bx 时循环体一次都不跑，房间挖不通 */
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        for (int x = x0; x <= x1; x++) {
            Tile& t = tiles[x + y * DG_MAP_W];
            if (t.terr == DG_TERR_EMPTY || t.terr == DG_TERR_WALL) t.terr = DG_TERR_FLOOR;
        }
    };
    auto dig_v = [&](int y0, int y1, int x) {
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        for (int y = y0; y <= y1; y++) {
            Tile& t = tiles[x + y * DG_MAP_W];
            if (t.terr == DG_TERR_EMPTY || t.terr == DG_TERR_WALL) t.terr = DG_TERR_FLOOR;
        }
    };
    auto connect = [&](int a, int b) {
        int ax = room_cx(rooms[a]), ay = room_cy(rooms[a]);
        int bx = room_cx(rooms[b]), by = room_cy(rooms[b]);
        if (r.nextInt(2)) { dig_h(ax, bx, ay); dig_v(ay, by, bx); }
        else              { dig_v(ay, by, ax); dig_h(ax, bx, by); }
    };

    /* 环路走廊：随机两两连 2 条（允许重复，无副作用） */
    for (int loop = 0; loop < 2; loop++) {
        int a = r.nextInt(room_count), b = r.nextInt(room_count);
        if (a == b) continue;
        connect(a, b);
    }
    /* 补全连通 */
    for (int pass = 0; pass < 40; pass++) {
        bool changed = false;
        for (int i = 0; i < room_count; i++) {
            for (int j = i + 1; j < room_count; j++) {
                if (UF::find(uni, i) == UF::find(uni, j)) continue;
                /* 只连矩形邻近的一对（距离近的走廊更短） */
                connect(i, j);
                UF::un(uni, i, j);
                changed = true;
            }
            if (changed) break;
        }
        if (!changed) break;
    }

    /* --- 3. 边界墙：所有贴地板的 EMPTY 变 WALL --- */
    for (int y = 0; y < DG_MAP_H; y++) {
        for (int x = 0; x < DG_MAP_W; x++) {
            Tile& t = tiles[x + y * DG_MAP_W];
            if (t.terr != DG_TERR_EMPTY) continue;
            static const int dx4[4] = { 0, 1, 0,-1 };
            static const int dy4[4] = { -1, 0, 1, 0 };
            for (int k = 0; k < 4; k++) {
                int nx = x + dx4[k], ny = y + dy4[k];
                if (nx < 0 || nx >= DG_MAP_W || ny < 0 || ny >= DG_MAP_H) continue;
                dg_terrain_t nt = (dg_terrain_t)tiles[nx + ny * DG_MAP_W].terr;
                if (nt != DG_TERR_EMPTY) { t.terr = DG_TERR_WALL; break; }
            }
        }
    }

    /* --- 4. 整形：对穿有地板的孤立墙改门（缝合友好的等价 smoothing） --- */
    for (int y = 1; y < DG_MAP_H - 1; y++) {
        for (int x = 1; x < DG_MAP_W - 1; x++) {
            Tile& t = tiles[x + y * DG_MAP_W];
            if (t.terr != DG_TERR_WALL) continue;
            bool up    = tiles[x + (y - 1) * DG_MAP_W].terr == DG_TERR_FLOOR;
            bool down  = tiles[x + (y + 1) * DG_MAP_W].terr == DG_TERR_FLOOR;
            bool left  = tiles[x - 1 + y * DG_MAP_W].terr == DG_TERR_FLOOR;
            bool right = tiles[x + 1 + y * DG_MAP_W].terr == DG_TERR_FLOOR;
            if ((up && down) || (left && right)) t.terr = DG_TERR_DOOR;
        }
    }

    /* --- 5. 入口 / 出口：选互距最远的两个房间 --- */
    int near_i = 0, far_i = 0, best_d = -1;
    for (int i = 0; i < room_count; i++) {
        for (int j = i + 1; j < room_count; j++) {
            int d = distance(room_cx(rooms[i]), room_cy(rooms[i]),
                             room_cx(rooms[j]), room_cy(rooms[j]));
            if (d > best_d) { best_d = d; near_i = i; far_i = j; }
        }
    }
    int ex_x = room_cx(rooms[far_i]), ex_y = room_cy(rooms[far_i]);
    int en_x = room_cx(rooms[near_i]), en_y = room_cy(rooms[near_i]);
    tiles[en_x + en_y * DG_MAP_W].terr = DG_TERR_ENTRY;
    tiles[ex_x + ex_y * DG_MAP_W].terr = DG_TERR_EXIT;
    entrance_pos = (pos_t)(en_x + en_y * DG_MAP_W);
    exit_pos     = (pos_t)(ex_x + ex_y * DG_MAP_W);

    int chapter = this->chapter();

    /* --- 5b. 特殊房间主题：从非进出口房间挑 1~3 个赋予 coherent 装饰，
     *         让每层结构可读、玩法多样（对齐上游 Garden / Statuary / Vault /
     *         TrapRoom / Pool 等 RegularRoom 子类的「结论」而非实现）。--- */
    {
        enum SpecRoomType { SPEC_GARDEN = 0, SPEC_STATUARY, SPEC_VAULT, SPEC_TRAPROOM, SPEC_POOL,
                            SPEC_TYPE_COUNT };
        int cand[10]; int cand_n = 0;
        for (int i = 0; i < room_count; i++)
            if (i != near_i && i != far_i) cand[cand_n++] = i;
        for (int i = cand_n - 1; i > 0; i--) {          /* Fisher-Yates 打乱候选房间 */
            int j = r.nextInt(i + 1);
            int t = cand[i]; cand[i] = cand[j]; cand[j] = t;
        }
        int types[SPEC_TYPE_COUNT] = { SPEC_GARDEN, SPEC_STATUARY, SPEC_VAULT, SPEC_TRAPROOM, SPEC_POOL };
        for (int i = SPEC_TYPE_COUNT - 1; i > 0; i--) {  /* 类型顺序也打乱，避免每层同拍 */
            int j = r.nextInt(i + 1);
            int t = types[i]; types[i] = types[j]; types[j] = t;
        }
        int want_spec = 1 + r.nextInt(2) + (depth_ >= DG_CHAPTER_DEPTH ? 1 : 0);   /* 1~3，深层更多 */
        if (want_spec > cand_n) want_spec = cand_n;

        /* 开敞内场判定：该格是 FLOOR 且四邻皆 FLOOR。只有这种格才能放
         * 固态装饰（雕像 / 宝箱）——保证它不落在走廊口 / 房间门口，
         * 也就绝不会把唯一通路切断（出口可达性是硬约束）。 */
        auto open_field = [&](int x, int y) -> bool {
            if (x < 1 || y < 1 || x >= DG_MAP_W - 1 || y >= DG_MAP_H - 1) return false;
            if (tiles[x + y * DG_MAP_W].terr != DG_TERR_FLOOR) return false;
            return tiles[(x - 1) + y * DG_MAP_W].terr == DG_TERR_FLOOR &&
                   tiles[(x + 1) + y * DG_MAP_W].terr == DG_TERR_FLOOR &&
                   tiles[x + (y - 1) * DG_MAP_W].terr == DG_TERR_FLOOR &&
                   tiles[x + (y + 1) * DG_MAP_W].terr == DG_TERR_FLOOR;
        };

        for (int s = 0; s < want_spec; s++) {
            const Room& rm = rooms[cand[s]];
            int w = rm.x1 - rm.x0 + 1, h = rm.y1 - rm.y0 + 1;
            int ty = types[s % SPEC_TYPE_COUNT];
            switch (ty) {
            case SPEC_GARDEN:      /* 花园：室内铺草（洞穴章掺高草），出生点附近留白 */
                for (int y = rm.y0; y <= rm.y1; y++) {
                    for (int x = rm.x0; x <= rm.x1; x++) {
                        Tile& t = tiles[x + y * DG_MAP_W];
                        if (t.terr != DG_TERR_FLOOR) continue;
                        if (distance(x, y, en_x, en_y) < 3) continue;
                        t.terr = (chapter == 2 && (tile_hash(x, y, (uint32_t)depth * 3u + s) & 3) == 0)
                                 ? DG_TERR_HIGH_GRASS : DG_TERR_GRASS;
                    }
                }
                break;
            case SPEC_STATUARY: {  /* 雕像室：在开敞内场撒 1~3 根柱（四邻皆地，不断路）*/
                int placed = 0;
                for (int tries = 0; tries < 30 && placed < 3; tries++) {
                    int sx = rm.x0 + 1 + r.nextInt(w - 2 < 1 ? 1 : w - 2);
                    int sy = rm.y0 + 1 + r.nextInt(h - 2 < 1 ? 1 : h - 2);
                    if (!open_field(sx, sy)) continue;
                    if (distance(sx, sy, en_x, en_y) < 4) continue;
                    tiles[sx + sy * DG_MAP_W].terr = DG_TERR_STATUE;
                    placed++;
                }
            } break;
            case SPEC_VAULT: {     /* 储藏 vault： clustered 2~3 个宝箱（只放开敞内场），约四成是宝箱怪 */
                int want = 2 + r.nextInt(2);
                int placed = 0;
                for (int tries = 0; tries < 40 && placed < want; tries++) {
                    int cx = rm.x0 + 1 + r.nextInt(w - 2 < 1 ? 1 : w - 2);
                    int cy = rm.y0 + 1 + r.nextInt(h - 2 < 1 ? 1 : h - 2);
                    if (!open_field(cx, cy)) continue;
                    if (distance(cx, cy, en_x, en_y) < 5) continue;
                    Tile& t = tiles[cx + cy * DG_MAP_W];
                    t.terr = DG_TERR_CHEST;
                    t.variant = (uint8_t)chapter;
                    if (depth_ >= 2 && r.nextInt(100) < 40) t.mimic = 1;
                    placed++;
                }
            } break;
            case SPEC_TRAPROOM:    /* 陷阱房：室内铺隐藏陷阱（trap_known 仍 0，踩上才揭）*/
                for (int y = rm.y0 + 1; y < rm.y1; y++) {
                    for (int x = rm.x0 + 1; x < rm.x1; x++) {
                        Tile& t = tiles[x + y * DG_MAP_W];
                        if (t.terr != DG_TERR_FLOOR) continue;
                        if (distance(x, y, en_x, en_y) < 4) continue;
                        t.terr = DG_TERR_TRAP;
                    }
                }
                break;
            case SPEC_POOL:        /* 水池：室内灌浅水，留 1 格地板边框（水可通行，只是减速）*/
                for (int y = rm.y0 + 1; y < rm.y1; y++) {
                    for (int x = rm.x0 + 1; x < rm.x1; x++) {
                        Tile& t = tiles[x + y * DG_MAP_W];
                        if (t.terr != DG_TERR_FLOOR) continue;
                        if (distance(x, y, en_x, en_y) < 3) continue;
                        t.terr = DG_TERR_WATER;
                    }
                }
                break;
            default: break;
            }
        }
    }

    /* --- 6. 上锁门：监狱章（5~8F）随机把 1~2 扇远离入口的门上锁 + 配钥匙 --- */
    int locked = 0;
    if (depth_ >= DG_CHAPTER_DEPTH && depth_ < DG_CHAPTER_DEPTH * 2) {
        for (int tries = 0; tries < 120 && locked < 2; tries++) {
            int x = r.nextInt(DG_MAP_W), y = r.nextInt(DG_MAP_H);
            Tile& t = tiles[x + y * DG_MAP_W];
            if (t.terr != DG_TERR_DOOR) continue;
            if (distance(x, y, en_x, en_y) < 8) continue;
            t.terr = DG_TERR_LOCKED_DOOR;
            locked++;
        }
    }

    /* --- 7. 秘密门：2 个（洞穴章 3 个），远离入口 --- */
    int secrets_want = depth_ >= DG_CHAPTER_DEPTH * 2 ? 3 : 2;
    int secrets = 0;
    for (int tries = 0; tries < 300 && secrets < secrets_want; tries++) {
        int x = 1 + r.nextInt(DG_MAP_W - 2), y = 1 + r.nextInt(DG_MAP_H - 2);
        Tile& t = tiles[x + y * DG_MAP_W];
        if (t.terr != DG_TERR_WALL) continue;
        bool up    = tiles[x + (y - 1) * DG_MAP_W].terr == DG_TERR_FLOOR;
        bool down  = tiles[x + (y + 1) * DG_MAP_W].terr == DG_TERR_FLOOR;
        bool left  = tiles[x - 1 + y * DG_MAP_W].terr == DG_TERR_FLOOR;
        bool right = tiles[x + 1 + y * DG_MAP_W].terr == DG_TERR_FLOOR;
        /* 与整形规则互补：这里找「只有一侧是地」的墙，藏在房间边上是上游手感 */
        if (up + down + left + right != 1) continue;
        if (distance(x, y, en_x, en_y) < 6) continue;
        t.terr = DG_TERR_SECRET;
        secrets++;
    }

    /* --- 8. 水塘 / 草皮：blob 生长，越深越多（chapter 已在 5b 前算好）--- */
    int ponds = (chapter == 0 ? 2 : chapter == 1 ? 3 : 2) + r.nextInt(2);
    for (int p = 0; p < ponds; p++) {
        int x = r.nextInt(DG_MAP_W), y = r.nextInt(DG_MAP_H);
        if (tiles[x + y * DG_MAP_W].terr != DG_TERR_FLOOR) continue;
        if (distance(x, y, en_x, en_y) < 4) continue;
        int size = 4 + r.nextInt(10);
        for (int g = 0; g < size; g++) {
            static const int dx4[4] = { 0, 1, 0,-1 };
            static const int dy4[4] = { -1, 0, 1, 0 };
            int k = r.nextInt(4);
            int nx = x + dx4[k], ny = y + dy4[k];
            if (nx < 1 || nx >= DG_MAP_W - 1 || ny < 1 || ny >= DG_MAP_H - 1) continue;
            Tile& t = tiles[nx + ny * DG_MAP_W];
            if (t.terr == DG_TERR_FLOOR) t.terr = DG_TERR_WATER;
            else if (t.terr != DG_TERR_WATER) continue;
            x = nx; y = ny;
        }
    }
    /* 草：下水道/洞穴多点；高草只在洞穴章（对齐上游 HighGrass 出现在 caves+） */
    for (int y = 1; y < DG_MAP_H - 1; y++) {
        for (int x = 1; x < DG_MAP_W - 1; x++) {
            Tile& t = tiles[x + y * DG_MAP_W];
            if (t.terr != DG_TERR_FLOOR) continue;
            if (distance(x, y, en_x, en_y) < 3) continue;   /* 保护出生点 */
            int roll = r.nextInt(100);
            if (chapter == 2 ? roll < 10 : roll < 6) {
                t.terr = (chapter == 2 && roll % 3 == 0) ? DG_TERR_HIGH_GRASS
                                                         : DG_TERR_GRASS;
            }
        }
    }

    /* --- 9. 隐藏陷阱：踩上才揭（trap_known 初始 0，地表仍是 FLOOR） --- */
    int traps = 3 + depth_ / 3;
    for (int t = 0; t < traps; t++) {
        int x = r.nextInt(DG_MAP_W), y = r.nextInt(DG_MAP_H);
        Tile& tl = tiles[x + y * DG_MAP_W];
        if (tl.terr != DG_TERR_FLOOR) continue;
        if (distance(x, y, en_x, en_y) < 5) continue;
        tl.terr = DG_TERR_TRAP;     /* 渲染按 FLOOR 画，直到 trap_known */
    }

    /* --- 10. 宝箱：远房 1 个必出；其余概率。章节换皮在渲染层用 variant --- */
    int chests = 1 + (r.nextInt(100) < 45 ? 1 : 0);
    for (int c = 0; c < chests; c++) {
        int ri = far_i;
        if (c > 0) ri = r.nextInt(room_count);
        int cx = -1, cy = -1;
        if (!pick_in_room(this, r, rooms[ri], 30, en_x, en_y, 6, &cx, &cy)) continue;
        if (!open_field_at(this, cx, cy)) continue;   /* 只放开敞内场，免得宝箱卡在走廊口断连 */
        Tile& ct = tiles[cx + cy * DG_MAP_W];
        ct.terr = DG_TERR_CHEST;
        ct.variant = (uint8_t)chapter;   /* 木/铜/银/金 按章 */
        if (depth_ >= 2 && r.nextInt(100) < 25) ct.mimic = 1;   /* 约四分之一是宝箱怪 */
    }

    /* --- 11. 装饰柱（监狱章起，房间内孤柱；四邻皆地才摆，免切通路）--- */
    if (chapter >= 1) {
        int statues = r.nextInt(3);
        for (int s = 0; s < statues; s++) {
            int ri = r.nextInt(room_count);
            int cx = -1, cy = -1;
            if (!pick_in_room(this, r, rooms[ri], 20, en_x, en_y, 5, &cx, &cy)) continue;
            if (cx < 1 || cy < 1 || cx >= DG_MAP_W - 1 || cy >= DG_MAP_H - 1) continue;
            if (tiles[(cx - 1) + cy * DG_MAP_W].terr != DG_TERR_FLOOR ||
                tiles[(cx + 1) + cy * DG_MAP_W].terr != DG_TERR_FLOOR ||
                tiles[cx + (cy - 1) * DG_MAP_W].terr != DG_TERR_FLOOR ||
                tiles[cx + (cy + 1) * DG_MAP_W].terr != DG_TERR_FLOOR) continue;
            tiles[cx + cy * DG_MAP_W].terr = DG_TERR_STATUE;
        }
    }

    /* --- 12. 视野与残留指针重置：必须在任何落格物品（护身符）之前，
     * reset_view 会把 tiles[].item 清空 --- */
    reset_view();

    /* --- 13. 12F：基座 + 护身符（通关信物，拾起即 WIN 判定在 hero.pickup） --- */
    amulet_pos = 0;
    if (depth_ == DG_MAX_DEPTH - 1) {
        tiles[ex_x + ex_y * DG_MAP_W].terr = DG_TERR_PEDESTAL;
        amulet_pos = (pos_t)(ex_x + ex_y * DG_MAP_W);
        /* 护身符放基座上而不是楼梯：下到此层去找它 */
        Item* am = Game::instance().make_item(Item::K_AMULET, 0);
        if (am) { am->x = ex_x; am->y = ex_y; am->on_drop(this); }
    }

    /* --- 14. variant 哈希：渲染期装饰概率，生成期一次算好 --- */
    for (int y = 0; y < DG_MAP_H; y++) {
        for (int x = 0; x < DG_MAP_W; x++) {
            tiles[x + y * DG_MAP_W].variant =
                (tiles[x + y * DG_MAP_W].terr == DG_TERR_CHEST)
                ? tiles[x + y * DG_MAP_W].variant            /* 宝箱 variant 已用 */
                : tile_hash(x, y, base_seed ^ (uint32_t)depth_ * 0x01000193u);
        }
    }

    /* --- 13.5 可达性兵底：装饰与上锁都完事后，洪水填充确认入口能走到出口；
     *         若不可达，依次把上锁门降回普通门、秘密门显形为门、最后把固态
     *         装饰（宝箱/雕像，含 mimic 与 STATUARY/VAULT 摆放后周边又被整形
     *         成门的边缘情形）还原为地板，直到通路打通。step 2 已保证全部房间
     *         4 向连通，纯 FLOOR 网必然可达 —— 保证「每层有可达出口」是硬约束，
     *         不会因稀 seed 软锁。--- */
    if (!flood_exit_reachable(this)) {
        for (int i = 0; i < LENGTH; i++)
            if (tiles[i].terr == DG_TERR_LOCKED_DOOR) tiles[i].terr = DG_TERR_DOOR;
        locked = 0;
        if (!flood_exit_reachable(this)) {
            for (int i = 0; i < LENGTH; i++)
                if (tiles[i].terr == DG_TERR_SECRET) tiles[i].terr = DG_TERR_DOOR;
            secrets = 0;
        }
        if (!flood_exit_reachable(this)) {
            int cleared = 0;
            for (int i = 0; i < LENGTH; i++)
                if (tiles[i].terr == DG_TERR_CHEST || tiles[i].terr == DG_TERR_STATUE) {
                    tiles[i].terr = DG_TERR_FLOOR; tiles[i].mimic = 0; cleared++;
                }
            ESP_LOGW(TAG, "depth=%d 清除固态装饰 %d 格以恢复出口可达", depth_, cleared);
        }
        ESP_LOGW(TAG, "depth=%d 出口不可达，已做可达性修复（unlock/显形/清固态）", depth_);
    }

    ESP_LOGI(TAG, "generate depth=%d rooms=%d entry=(%d,%d) exit=(%d,%d) locked=%d secret=%d",
             depth_, room_count, en_x, en_y, ex_x, ex_y, locked, secrets);
    return true;
}

}  /* namespace dg */
