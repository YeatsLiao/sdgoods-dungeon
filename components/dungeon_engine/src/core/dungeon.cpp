/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dungeon.cpp —— Game 状态机核心 + tile 渲染器（v0.2 可玩版）
 *
 * 渲染契约（与 dungeon_api.h 一致）：get_tilemap_fb 返回 352×352 RGB565
 * PSRAM fb；fb_dirty 时重绘，否则返回 NULL 让 UI 跳帧。素材缺失
 * （未烧 assets.bin）时 gfx::load() 失败 → 恒返 NULL，UI 保留棋盘占位。
 *
 * 回合流：玩家行动（走/砍/等/搜）→ 视野重算 → 全体怪物 act → 死亡/
 * 下楼判定；自动寻路在 tick() 里按 120ms/步 分步消费，每步都是完整回合。
 */
#include "dg_types.h"
#include "rng/java_random.h"
#include "gfx/gfx.h"
#include "fov/fov.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static const char *TAG = "dg.game";

namespace dg {

/* ===== 静态内存池（架构红线：hot path 不 heap）===== */
static Mob    s_mob_pool[Game::kMaxMob];
static bool   s_mob_used[Game::kMaxMob];
static Item   s_item_pool[Game::kMaxItem];
static bool   s_item_used[Game::kMaxItem];

/* 主视窗 fb（PSRAM 常驻，352×352×2B = 248KB） */
static uint16_t* s_fb = nullptr;

/* ===== 视野半径 ===== */
static constexpr int kFovRadius = 11;   /* 覆盖到圆屏可见边缘（半径 180px = 11.25 tile） */

/* ===== 地形 → 图集索引 =====
 * 常量抄自上游 core/.../tiles/DungeonTileSheet.java（WIDTH=16 列，xy 1 基）：
 *   GROUND=xy(1,1)=0 → FLOOR=0 FLOOR_DECO=1 GRASS=2 FLOOR_SP=4 FLOOR_ALT_1=6
 *   ENTRANCE=GROUND+16=16  EXIT=GROUND+17=17
 *   WATER=xy(1,3)=32  FLAT_WALL=xy(1,4)=48（+1=DECO）
 * 秘密门/关门 v0.2 以墙代画；宝箱借 PEDESTAL(20) 位。 */
static int tile_sheet_index(const Tile& t, int x, int y) {
    switch (t.terr) {
    case DG_TERR_WALL:      return ((x * 7 + y * 13) % 17 == 0) ? 49 : 48;
    case DG_TERR_EMPTY:     return -1;               /* 地图外：纯黑 */
    case DG_TERR_FLOOR: {
        int h = x * 7 + y * 13;
        if (h % 13 == 0) return 6;                   /* FLOOR_ALT_1 */
        if (h % 11 == 0) return 1;                   /* FLOOR_DECO */
        return 0;
    }
    case DG_TERR_DOOR:      return 48;               /* 关着 = 墙外观（v0.2 近似） */
    case DG_TERR_OPEN_DOOR: return 4;                /* FLOOR_SP */
    case DG_TERR_EXIT:      return 17;
    case DG_TERR_ENTRY:     return 16;
    case DG_TERR_WATER:     return 32;
    case DG_TERR_GRASS:     return 2;
    case DG_TERR_TRAP:      return 1;
    case DG_TERR_CHEST:     return 20;               /* PEDESTAL 借用 */
    case DG_TERR_SECRET:    return 48;               /* 未揭露秘密门按墙画 */
    case DG_TERR_STATUE:    return 4;
    default:                return 0;
    }
}

/* ===== Game ===== */
Game::Game() {
    rng     = new JavaRandom(0);
    ui_rng  = new JavaRandom(0);
    hero    = new Hero();
    level   = new Level();
    memset(log_lines, 0, sizeof(log_lines));
}

Game::~Game() {
    delete rng; delete ui_rng; delete hero; delete level;
}

Game& Game::instance() {
    static Game g;
    return g;
}

void Game::init() {
    ESP_LOGI(TAG, "Game::init");
    assets::initialize();
    save::initialize();
    scene = DG_SCENE_TITLE;
}

void Game::log(const char* key) {
    log_lines[log_head] = key;
    log_head = (log_head + 1) % kLogLines;
}

/* ===== 池分配 ===== */
Mob* Game::alloc_mob() {
    for (int i = 0; i < kMaxMob; i++) {
        if (!s_mob_used[i]) { s_mob_used[i] = true; return &s_mob_pool[i]; }
    }
    return nullptr;
}

static Item* alloc_item() {
    for (int i = 0; i < Game::kMaxItem; i++) {
        if (!s_item_used[i]) { s_item_used[i] = true; return &s_item_pool[i]; }
    }
    return nullptr;
}

static void free_pools() {
    memset(s_mob_used, 0, sizeof(s_mob_used));
    memset(s_item_used, 0, sizeof(s_item_used));
}

/* ===== 本层内容投放：老鼠怪组 + 金币堆（确定性：走世界 rng） ===== */
void Game::spawn_level_content() {
    free_pools();
    level->actor_count = 0;
    level->actors[level->actor_count++] = hero;   /* actors[0] 恒为英雄 */
    hero->level = level;
    level->at(hero->x, hero->y).actor = hero;

    JavaRandom& r = *rng;
    int spawned = 0;
    int tries = 0;
    int want = 4 + depth;
    if (want > 8) want = 8;
    while (spawned < want && tries++ < 300) {
        int x = r.nextInt(DG_MAP_W), y = r.nextInt(DG_MAP_H);
        Tile& t = level->at(x, y);
        if (!level->passable(x, y) || t.actor || t.item) continue;
        if (level->distance(x, y, level->entrance_pos % DG_MAP_W,
                            level->entrance_pos / DG_MAP_W) < 10) continue;
        Mob* m = alloc_mob();
        if (!m) break;
        m = new (m) Mob();                        /* 池上原位构造 */
        m->hp_max = 8; m->hp = 8;
        m->attack_min = 1; m->attack_max = 4;
        m->defense = 2; m->xp_in_kill = 2;
        m->name_key = "rat";
        m->state = Mob::SLEEPING;
        m->level = level;
        m->set_pos(x, y);
        t.actor = m;
        level->actors[level->actor_count++] = m;
        spawned++;
    }

    int gold_spawned = 0;
    for (tries = 0; gold_spawned < 3 && tries++ < 200; ) {
        int x = r.nextInt(DG_MAP_W), y = r.nextInt(DG_MAP_H);
        Tile& t = level->at(x, y);
        if (!level->passable(x, y) || t.actor || t.item) continue;
        Item* it = alloc_item();
        if (!it) break;
        it = new (it) Item();
        it->kind = Item::K_GOLD;
        it->qty = 10 + r.nextInt(20) + depth * 5;
        it->name_key = "gold";
        it->x = x; it->y = y;
        it->on_drop(level);
        gold_spawned++;
    }
    ESP_LOGI(TAG, "spawn level %d: %d rats, %d gold piles", depth, spawned, gold_spawned);
}

/* ===== 新局 / 视野 / 下楼 ===== */
void Game::new_game(int hero_class, uint32_t seed) {
    ESP_LOGI(TAG, "Game::new_game class=%d seed=0x%08x", hero_class, seed);
    this->seed = seed;
    rng->setSeed(seed);
    ui_rng->setSeed(seed ^ 0x5DEECE66DULL);       /* UI 杂项流，不污染世界序列 */
    game_time = 0;
    depth = 0;
    path_len = path_head = 0;

    hero->cls = (dg_class_t)hero_class;
    hero->hp_max = 20; hero->hp = 20;
    hero->str = 10; hero->lvl = 1; hero->exp = 0; hero->gold = 0;
    hero->level = level;

    level->generate(seed);
    level->depth = 0;
    level->hero = hero;

    hero->set_pos(level->entrance_pos % DG_MAP_W,
                  level->entrance_pos / DG_MAP_W);

    spawn_level_content();
    recalc_fov();

    scene = DG_SCENE_IN_GAME;
    fb_dirty = true;
    log("欢迎来到次元地牢！点击地面移动，走到怪物旁自动攻击。");
}

void Game::recalc_fov() {
    fov::compute(level, hero->x, hero->y, kFovRadius);
}

bool Game::descend_stairs() {
    if (level->at(hero->x, hero->y).terr != DG_TERR_EXIT) return false;
    depth++;
    uint32_t s = (uint32_t)rng->nextInt();
    level->generate(s);
    level->depth = depth;
    hero->set_pos(level->entrance_pos % DG_MAP_W,
                  level->entrance_pos / DG_MAP_W);
    path_len = path_head = 0;
    spawn_level_content();
    recalc_fov();
    fb_dirty = true;
    /* 缓冲 64B：中文消息 31B + 层数最多 11B，留足余量免 format-truncation */
    static char s_depth_msgs[16][64];
    snprintf(s_depth_msgs[depth % 16], sizeof(s_depth_msgs[0]),
             "你下到了 %d 层。空气愈发恶臭……", depth + 1);
    log(s_depth_msgs[depth % 16]);
    ESP_LOGI(TAG, "descend -> depth %d", depth);
    return true;
}

/* ===== 英雄一步：走 / 砍 / 拾取（成功 = 消耗一个回合） ===== */
bool Game::hero_try_step(int gx, int gy) {
    if (scene != DG_SCENE_IN_GAME) return false;
    if (gx < 0 || gx >= DG_MAP_W || gy < 0 || gy >= DG_MAP_H) return false;
    if (gx == hero->x && gy == hero->y) return false;

    Tile& t = level->at(gx, gy);

    /* 格子上有活物：是怪就打，是别的就止步 */
    if (t.actor && t.actor != hero) {
        if (t.actor->alignment == 1 /* ENEMY */ || t.actor->alignment == 0) {
            hero->attack(t.actor);
            return true;
        }
        return false;
    }
    if (t.actor == hero) return false;

    if (!level->passable(gx, gy)) return false;

    /* 移动 */
    level->at(hero->x, hero->y).actor = nullptr;
    hero->set_pos(gx, gy);
    t.actor = hero;
    game_time++;

    /* 踩到关着的门自动打开（秘密门揭露后也是 DOOR） */
    if (t.terr == DG_TERR_DOOR) {
        t.terr = DG_TERR_OPEN_DOOR;
        fb_dirty = true;
    }

    /* 自动拾取 */
    if (t.item) {
        Item* it = t.item;
        if (it->kind == Item::K_GOLD) {
            hero->gold += it->qty;
            static char s_gold_msgs[8][64];
            snprintf(s_gold_msgs[game_time % 8], sizeof(s_gold_msgs[0]),
                     "拾取 %d 金币。（合计 %d）", it->qty, hero->gold);
            log(s_gold_msgs[game_time % 8]);
            int idx = (int)(it - s_item_pool);
            if (idx >= 0 && idx < kMaxItem) { s_item_used[idx] = false; }
            t.item = nullptr;
        } else if (hero->pickup(it)) {
            t.item = nullptr;
        }
    }
    return true;
}

/* ===== 怪物回合 ===== */
void Game::advance_mobs() {
    for (int i = 1; i < level->actor_count; i++) {
        Actor* a = level->actors[i];
        if (!a || !a->is_alive()) continue;
        a->act();
    }
    /* 收割尸体（act 里可能同归于尽） */
    for (int i = level->actor_count - 1; i >= 1; i--) {
        Actor* a = level->actors[i];
        if (a && !a->is_alive()) {
            level->actors[i] = level->actors[level->actor_count - 1];
            level->actors[--level->actor_count] = nullptr;
        }
    }
    if (hero->hp <= 0) {
        scene = DG_SCENE_GAME_OVER;
        log("你死了……点按屏幕重新开始。");
        ESP_LOGW(TAG, "hero died at depth %d", depth);
    }
}

/* ===== 输入 ===== */
void Game::tick() {
    if (scene != DG_SCENE_IN_GAME) return;
    if (path_head >= path_len) return;            /* 无自动行走任务 */

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - (int64_t)last_step_ms < 120) return;
    last_step_ms = (uint32_t)now_ms;

    int p = path_queue[path_head++];
    int gx = p % DG_MAP_W, gy = p / DG_MAP_W;
    if (!hero_try_step(gx, gy)) {
        path_len = path_head = 0;                 /* 被打断/不可达：停车 */
        fb_dirty = true;
        return;
    }
    if (descend_stairs()) return;
    recalc_fov();
    advance_mobs();
    if (path_head >= path_len) { path_len = path_head = 0; }
    fb_dirty = true;
}

void Game::on_tap(int gx, int gy) {
    if (scene != DG_SCENE_IN_GAME) return;
    if (gx < 0 || gx >= DG_MAP_W || gy < 0 || gy >= DG_MAP_H) return;

    int dx = gx - hero->x, dy = gy - hero->y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;

    bool acted = false;
    if (adx <= 1 && ady <= 1) {
        /* 相邻 8 格（含原地）→ 走一步 / 攻击 */
        acted = hero_try_step(gx, gy);
    } else {
        /* 远处 → A* 整条路径入队，本帧先走第一步 */
        int steps[128];
        int n = pathfinder::find_path(level, hero->x, hero->y, gx, gy, steps, 128);
        if (n > 0) {
            memcpy(path_queue, steps, n * sizeof(int));
            path_len = n;
            path_head = 1;
            int64_t now_ms = esp_timer_get_time() / 1000;
            last_step_ms = (uint32_t)now_ms;
            acted = hero_try_step(steps[0] % DG_MAP_W, steps[0] / DG_MAP_W);
            if (!acted) { path_len = path_head = 0; }
        } else {
            log("那里过不去。");
            acted = false;
        }
    }

    if (acted) {
        if (descend_stairs()) return;
        recalc_fov();
        advance_mobs();
        fb_dirty = true;
    }
}

void Game::on_long_press(int gx, int gy) {
    ESP_LOGD(TAG, "inspect (%d,%d)", gx, gy);
    /* TODO: 弹检视卡（不耗回合） */
}

void Game::on_button(dg_btn_id_t btn) {
    ESP_LOGI(TAG, "button %d", (int)btn);
    switch (btn) {
    case DG_BTN_WAIT:
        if (scene != DG_SCENE_IN_GAME) break;
        game_time++;
        recalc_fov();
        advance_mobs();
        fb_dirty = true;
        break;
    case DG_BTN_SEARCH: {
        /* 搜索（v0.3）：耗一回合，揭 8 邻 + 自身格子的秘密门；
         * 成功率随等级升（原版 BaseSearch 简化） */
        if (scene != DG_SCENE_IN_GAME) break;
        game_time++;
        bool found = false;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int sx = hero->x + dx, sy = hero->y + dy;
                if (sx < 0 || sx >= DG_MAP_W || sy < 0 || sy >= DG_MAP_H) continue;
                Tile& t = level->at(sx, sy);
                if (t.terr != DG_TERR_SECRET) continue;
                if (ui_rng->nextInt(100) < 20 + hero->lvl * 5) {
                    t.terr = DG_TERR_DOOR;
                    found = true;
                }
            }
        }
        if (found) {
            log("你发现了隐藏的门！");
            recalc_fov();
        } else {
            log("你环顾四周，什么也没发现。");
        }
        advance_mobs();
        fb_dirty = true;
        break;
    }
    case DG_BTN_INVENTORY:
        if (scene == DG_SCENE_GAME_OVER) break;
        scene = (scene == DG_SCENE_INVENTORY) ? DG_SCENE_IN_GAME : DG_SCENE_INVENTORY;
        fb_dirty = true;
        break;
    case DG_BTN_MENU:
        /* GAME_OVER 后的唯一重开入口（电源键会丢进度，重开更快） */
        if (scene == DG_SCENE_GAME_OVER) {
            new_game(hero->cls, (uint32_t)esp_timer_get_time());
        } else {
            log("存档槽位将在 v0.4 提供。");
        }
        break;
    default:
        break;
    }
}

/* ===== 渲染（pull model 出口） ===== */
static void render_map(Game& g) {
    const int W = DG_VIEWPORT_W, H = DG_VIEWPORT_H;
    Level* lv = g.level;
    Hero* h = g.hero;

    /* 相机：英雄居中并 clamp（右侧留半格余量 → 13 列绘制覆盖 200px） */
    g.cam_x = h->x - (DG_VIEW_TILE_W / 2);
    g.cam_y = h->y - (DG_VIEW_TILE_H / 2);
    if (g.cam_x < 0) g.cam_x = 0;
    if (g.cam_y < 0) g.cam_y = 0;
    if (g.cam_x > DG_MAP_W - DG_VIEW_TILE_W) g.cam_x = DG_MAP_W - DG_VIEW_TILE_W;
    if (g.cam_y > DG_MAP_H - DG_VIEW_TILE_H) g.cam_y = DG_MAP_H - DG_VIEW_TILE_H;

    const uint16_t* sheet     = gfx::tile_sheet();
    const uint16_t* sheet_dim = gfx::tile_sheet_dim();

    for (int py = 0; py < DG_VIEW_TILE_H + 1; py++) {
        for (int px = 0; px < DG_VIEW_TILE_W + 1; px++) {
            int wx = g.cam_x + px, wy = g.cam_y + py;
            int dx = px * DG_TILE_PX, dy = py * DG_TILE_PX;
            if (wx >= DG_MAP_W || wy >= DG_MAP_H) {
                gfx::fill_rect(s_fb, W, H, dx, dy, DG_TILE_PX, DG_TILE_PX, 0x0000);
                continue;
            }
            Tile& t = lv->at(wx, wy);
            int idx = tile_sheet_index(t, wx, wy);
            if (idx < 0) {
                gfx::fill_rect(s_fb, W, H, dx, dy, DG_TILE_PX, DG_TILE_PX, 0x0000);
            } else if (t.vis_current) {
                gfx::blit(s_fb, W, H, dx, dy, sheet, 256,
                          (idx % 16) * DG_TILE_PX, (idx / 16) * DG_TILE_PX,
                          DG_TILE_PX, DG_TILE_PX);
            } else if (t.explored) {
                gfx::blit(s_fb, W, H, dx, dy, sheet_dim, 256,
                          (idx % 16) * DG_TILE_PX, (idx / 16) * DG_TILE_PX,
                          DG_TILE_PX, DG_TILE_PX);
            } else {
                gfx::fill_rect(s_fb, W, H, dx, dy, DG_TILE_PX, DG_TILE_PX, 0x0000);
            }
        }
    }

    /* 覆盖层 1：可见格掉落物（金币 6×6 色块，v0.3 换 items.png 真精灵） */
    for (int py = 0; py < DG_VIEW_TILE_H + 1; py++) {
        for (int px = 0; px < DG_VIEW_TILE_W + 1; px++) {
            int wx = g.cam_x + px, wy = g.cam_y + py;
            if (wx >= DG_MAP_W || wy >= DG_MAP_H) continue;
            Tile& t = lv->at(wx, wy);
            if (!t.vis_current) continue;
            if (t.item) {
                gfx::fill_rect(s_fb, W, H, px * DG_TILE_PX + 5, py * DG_TILE_PX + 7,
                               6, 5, 0xFEA0);      /* 金色 */
            } else if (t.actor && t.actor != h) {
                /* 覆盖层 2：怪物（rat 帧 0，16×16，黑透明） */
                if (gfx::rat_sheet()) {
                    gfx::blit_masked(s_fb, W, H, px * DG_TILE_PX, py * DG_TILE_PX,
                                     gfx::rat_sheet(), 256, 0, 0, 16, 16);
                }
            }
        }
    }

    /* 英雄本体：rogue 图集 idle 帧 (1,0) 12×15（HeroSprite FRAME 常量） */
    if (gfx::hero_sheet()) {
        int hx = (h->x - g.cam_x) * DG_TILE_PX + 2;
        int hy = (h->y - g.cam_y) * DG_TILE_PX + 1;
        gfx::blit_masked(s_fb, W, H, hx, hy, gfx::hero_sheet(), 256, 1, 0, 12, 15);
    }
}

const uint16_t* Game::get_tilemap_fb(int *w, int *h) {
    if (w) *w = DG_VIEWPORT_W;
    if (h) *h = DG_VIEWPORT_H;

    if (!gfx::load()) return nullptr;             /* 无素材：回退棋盘占位 */
    if (!s_fb) {
        s_fb = (uint16_t*)heap_caps_malloc(DG_VIEWPORT_W * DG_VIEWPORT_H * 2,
                                           MALLOC_CAP_SPIRAM);
        if (!s_fb) { ESP_LOGE(TAG, "fb alloc fail"); return nullptr; }
        memset(s_fb, 0, DG_VIEWPORT_W * DG_VIEWPORT_H * 2);
        fb_dirty = true;
    }
    if (scene != DG_SCENE_IN_GAME && scene != DG_SCENE_INVENTORY) {
        return fb_dirty ? s_fb : nullptr;
    }
    if (!fb_dirty) return nullptr;
    render_map(*this);
    fb_dirty = false;
    return s_fb;
}

int Game::get_status_text(char *buf, int cap) {
    if (scene < DG_SCENE_IN_GAME) return 0;
    /* 圆屏顶部弦窄（y=24 处可用宽 ~195px），状态行必须短：中文标签 + 数字 */
    return snprintf(buf, cap, "生命 %d/%d · %d层 · 金币 %d",
                    hero->hp, hero->hp_max, depth + 1, hero->gold);
}

int Game::get_message(char *buf, int cap, int index) {
    /* 从最新一条往前数 index 行 */
    int from = (log_head - 1 - index + kLogLines * 2) % kLogLines;
    const char* key = log_lines[from];
    if (!key) return 0;
    return snprintf(buf, cap, "%s", key);
}

/* 背包/状态 overlay 多行文本（LVGL 用中文子集字体渲染） */
int Game::get_stats_text(char* buf, int cap) {
    if (scene < DG_SCENE_IN_GAME) return 0;
    const char* cls_name = "战士";
    return snprintf(buf, cap,
        "%s  Lv %d\n生命 %d/%d\n经验 %d/%d\n力量 %d  命中 %d\n金币 %d\n深度 %dF  回合 %u",
        cls_name, hero->lvl, hero->hp, hero->hp_max,
        hero->exp, 5 + hero->lvl * 5,
        hero->str, hero->attack_skill,
        hero->gold, depth + 1, (unsigned)game_time);
}

/* 调试导出（串口 'v'）：一行给出定向点击所需的全部 tile 坐标。
 * 只统计已探明（explored）或当前可见的目标，避开让脚本去撞未知区。 */
int Game::debug_dump(char* buf, int cap) {
    if (!level || !hero) return 0;
    int n = snprintf(buf, cap, "D h=%d,%d c=%d,%d s=%d/%d g=%d d=%dF e=",
                     hero->x, hero->y, cam_x, cam_y,
                     hero->hp, hero->hp_max, hero->gold, depth + 1);
    if (n < 0 || n >= cap) return 0;

    int ex = -1, ey = -1;
    int items[4][2], nm = 0;
    int mobs[4][2],  nmo = 0;
    for (int y = 0; y < DG_MAP_H; y++) {
        for (int x = 0; x < DG_MAP_W; x++) {
            Tile& t = level->at(x, y);
            if (t.terr == DG_TERR_EXIT && ex < 0) { ex = x; ey = y; }
            if (t.item && nm < 4) { items[nm][0] = x; items[nm][1] = y; nm++; }
            if (t.actor && t.actor != (Actor*)hero && t.vis_current && nmo < 4) {
                mobs[nmo][0] = x; mobs[nmo][1] = y; nmo++;
            }
        }
    }
    /* 每段都卡住越界：snprintf 返回值是「本应写入的长度」，直接累加会算飞 */
    int w = snprintf(buf + n, cap - n, "%d,%d i=", ex, ey);
    if (w < 0 || n + w >= cap) return 0;
    n += w;
    for (int i = 0; i < nm; i++) {
        w = snprintf(buf + n, cap - n, "%s%d,%d", i ? "|" : "", items[i][0], items[i][1]);
        if (w < 0 || n + w >= cap) return 0;
        n += w;
    }
    w = snprintf(buf + n, cap - n, " m=");
    if (w < 0 || n + w >= cap) return 0;
    n += w;
    for (int i = 0; i < nmo; i++) {
        w = snprintf(buf + n, cap - n, "%s%d,%d", i ? "|" : "", mobs[i][0], mobs[i][1]);
        if (w < 0 || n + w >= cap) return 0;
        n += w;
    }
    return n;
}

}  /* namespace dg */
