/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dungeon.cpp —— Game 状态机核心（v0.4：场景流 / 回合 / 池 / 特效 / HUD）
 *
 * 渲染拆到 render.cpp（见 core/render.h），本文件只管「世界怎么往前走」：
 *   回合流：英雄一步（走/砍/开门/开箱/踩陷阱/拾取）→ 饥饿与回血 → 视野重算
 *           → 怪物回合 → 死亡/下楼判定；自动行走与动画在 tick() 里按时钟推进。
 *   内存红线：Actor / Item / Buff 全部走 Game 内静态池，hot path 零 heap 分配；
 *   下楼时只回收「地上的」实体，英雄背包与身上的装备绝不回收（旧版直接 memset
 *   位图会把这些指针变成悬垂，是本版本修掉的一个真实隐患）。
 */
#include "dg_types.h"
#include "core/render.h"
#include "item/item_def.h"
#include "actor/mob_spec.h"
#include "rng/java_random.h"
#include "gfx/gfx.h"
#include "fov/fov.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <new>

static const char *TAG = "dg.game";

namespace dg {

/* ===== 静态内存池（架构红线：hot path 不 heap）===== */
static Mob    s_mob_pool[Game::kMaxMob];
static bool   s_mob_used[Game::kMaxMob];
static Item   s_item_pool[Game::kMaxItem];
static bool   s_item_used[Game::kMaxItem];
static Buff   s_buff_pool[Game::kMaxBuff];
static bool   s_buff_used[Game::kMaxBuff];

/* 主视窗 fb（PSRAM 常驻，逻辑 176×176×2B = 60.5KB） */
static uint16_t* s_fb = nullptr;

static constexpr int kFovRadius = 6;

static const char* k_cls_name[DG_CLASS_COUNT] = { "战士", "法师", "盗贼", "猎手" };

/* ===== Game 生命周期 ===== */
Game::Game() {
    rng      = new JavaRandom(0);
    ui_rng   = new JavaRandom(0);
    hero     = new Hero();
    level    = new Level();
    memset(log_lines, 0, sizeof(log_lines));
}
Game::~Game() { delete rng; delete ui_rng; delete hero; delete level; }

Game& Game::instance() { static Game g; return g; }

void Game::init() {
    ESP_LOGI(TAG, "Game::init");
    assets::initialize();
    gfx::load();
    save::initialize();
    scene = DG_SCENE_TITLE;
}

/* ===== 消息 log（环形定长实体拷贝，非指针）===== */
void Game::log(const char* text) {
    if (!text) return;
    int n = (int)strlen(text);
    if (n >= kLogLen) n = kLogLen - 1;
    memcpy(log_lines[log_head], text, n);
    log_lines[log_head][n] = '\0';
    log_head = (log_head + 1) % kLogLines;
    ESP_LOGI(TAG, "log: %s", text);
}

/* ===== 池分配 / 回收 ===== */
Mob* Game::alloc_mob() {
    for (int i = 0; i < kMaxMob; i++) {
        if (!s_mob_used[i]) {
            s_mob_used[i] = true;
            return new (&s_mob_pool[i]) Mob();
        }
    }
    return nullptr;
}
void Game::free_mob(Mob* m) {
    if (!m) return;
    int idx = (int)(m - s_mob_pool);
    if (idx >= 0 && idx < kMaxMob) s_mob_used[idx] = false;
}
Item* Game::alloc_item() {
    for (int i = 0; i < kMaxItem; i++) {
        if (!s_item_used[i]) {
            s_item_used[i] = true;
            return new (&s_item_pool[i]) Item();
        }
    }
    return nullptr;
}
void Game::free_item(Item* it) {
    if (!it) return;
    int idx = (int)(it - s_item_pool);
    if (idx >= 0 && idx < kMaxItem) s_item_used[idx] = false;
}
Buff* Game::alloc_buff() {
    for (int i = 0; i < kMaxBuff; i++) {
        if (!s_buff_used[i]) { s_buff_used[i] = true; return &s_buff_pool[i]; }
    }
    return nullptr;
}
void Game::free_buff(Buff* b) {
    if (!b) return;
    int idx = (int)(b - s_buff_pool);
    if (idx >= 0 && idx < kMaxBuff) s_buff_used[idx] = false;
}

/* 英雄背包 / 身上引用的物品不能被下楼回收，逐个核对 */
static bool held_by_hero(Item* it) {
    if (!it) return false;
    Hero* h = Game::instance().hero;
    if (!h) return false;
    for (int i = 0; i < h->inv_count; i++) if (h->inventory[i] == it) return true;
    return it == h->equipped_weapon || it == h->equipped_armor || it == h->equipped_ring;
}

/* ===== 掉落 / 构造 ===== */
Item* Game::make_item(int kind, int sub) {
    Item* it = alloc_item();
    if (!it) return nullptr;
    fill_item(it, kind, sub, 1);
    return it;
}
Item* Game::drop_random_item(int x, int y, int quality_hint) {
    Item* it = roll_drop(quality_hint, depth, ui_rng);
    if (!it) return nullptr;
    it->x = x; it->y = y;
    it->on_drop(level);
    return it;
}

/* ===== 本层内容投放 ===== */
void Game::spawn_level_content() {
    /* 回收上一级残留：怪物全清、地上物品清（背包/身上保留）、buff 全清 */
    for (int i = 0; i < kMaxMob; i++)  s_mob_used[i] = false;
    for (int i = 0; i < kMaxItem; i++)  if (s_item_used[i] && !held_by_hero(&s_item_pool[i]))
                                            s_item_used[i] = false;
    for (int i = 0; i < kMaxBuff; i++)  s_buff_used[i] = false;

    level->actor_count = 0;
    level->hero = hero;
    level->actors[level->actor_count++] = hero;
    hero->level = level;
    hero->first_buff = nullptr;
    level->at(hero->x, hero->y).actor = hero;

    const int d1 = depth + 1;
    /* 怪物：数量随层增，物种按层挑 */
    int want = 4 + d1 / 2;
    if (want > kMaxMob - 4) want = kMaxMob - 4;
    int spawned = 0;
    for (int tries = 0; tries < 400 && spawned < want; tries++) {
        int x = rng->nextInt(DG_MAP_W), y = rng->nextInt(DG_MAP_H);
        Tile& t = level->at(x, y);
        if (!level->passable(x, y) || t.actor || t.item) continue;
        if (level->distance(x, y, level->entrance_pos % DG_MAP_W,
                            level->entrance_pos / DG_MAP_W) < 7) continue;
        const MobSpec* s = mob_pick_for_depth(rng, depth);
        if (!s) break;
        Mob* m = alloc_mob();
        if (!m) break;
        m->spec = s;
        m->sheet = s->sheet;
        m->name_key = s->name;
        m->hp_max = s->hp; m->hp = s->hp;
        m->attack_min = s->atk_min; m->attack_max = s->atk_max;
        m->defense = s->def; m->xp_in_kill = s->xp;
        m->see_range = s->see;
        m->speed = s->speed;                     /* 1/16 定点直接取物种表（蟹/蝠=32 双倍速）*/
        m->alignment = 1;                       /* ENEMY */
        m->state = (rng->nextInt(100) < 60) ? Mob::SLEEPING : Mob::WANDERING;
        m->level = level;
        m->first_buff = nullptr;
        m->flash_ticks = 0;
        m->move_anim = 255;
        m->anim_seed = (uint8_t)rng->nextInt(256);
        m->from_x = m->x = x; m->from_y = m->y = y;
        m->home_x = x; m->home_y = y;
        t.actor = m;
        level->actors[level->actor_count++] = m;
        spawned++;
    }

    /* 首领：4F/8F/12F 各一只，放最靠出口处 */
    const MobSpec* boss = mob_boss_for_depth(depth);
    if (boss) {
        int bx = level->exit_pos % DG_MAP_W, by = level->exit_pos / DG_MAP_W;
        /* 找一个出口附近的空地 */
        for (int r = 1; r <= 4 && boss; r++) {
            bool placed = false;
            for (int dy = -r; dy <= r && !placed; dy++) {
                for (int dx = -r; dx <= r && !placed; dx++) {
                    int nx = bx + dx, ny = by + dy;
                    if (!level->passable(nx, ny) || level->at(nx, ny).actor) continue;
                    Mob* m = alloc_mob();
                    if (!m) break;
                    m->spec = boss; m->sheet = boss->sheet; m->name_key = boss->name;
                    m->hp_max = boss->hp; m->hp = boss->hp;
                    m->attack_min = boss->atk_min; m->attack_max = boss->atk_max;
                    m->defense = boss->def; m->xp_in_kill = boss->xp;
                    m->see_range = boss->see; m->alignment = 1;
                    m->speed = boss->speed;
                    m->state = Mob::SLEEPING; m->level = level;
                    m->first_buff = nullptr; m->flash_ticks = 0; m->move_anim = 255;
                    m->anim_seed = (uint8_t)rng->nextInt(256);
                    m->from_x = m->x = nx; m->from_y = m->y = ny;
                    m->home_x = nx; m->home_y = ny;
                    level->at(nx, ny).actor = m;
                    level->actors[level->actor_count++] = m;
                    placed = true;
                }
                if (placed) break;
            }
        }
        log("你感到一股危险的气息……");
    }

    /* 地表杂物：金币 + 少量吃喝 */
    int gold_piles = 2 + rng->nextInt(2);
    for (int g = 0; g < gold_piles; g++) {
        for (int tries = 0; tries < 60; tries++) {
            int x = rng->nextInt(DG_MAP_W), y = rng->nextInt(DG_MAP_H);
            Tile& t = level->at(x, y);
            if (!level->passable(x, y) || t.actor || t.item) continue;
            Item* it = alloc_item();
            if (!it) break;
            fill_item(it, Item::K_GOLD, 0, 0);
            it->qty = 8 + rng->nextInt(15) + depth * 4;
            it->x = x; it->y = y;
            it->on_drop(level);
            break;
        }
    }
    int litter = 1 + rng->nextInt(2);
    for (int k = 0; k < litter; k++) drop_random_item(rng->nextInt(DG_MAP_W),
                                                     rng->nextInt(DG_MAP_H), 0);

    ESP_LOGI(TAG, "spawn depth %d: %d actors", depth, level->actor_count);
}

/* ===== 新局 / 场景流 ===== */
void Game::new_game(int hero_class, uint32_t seed) {
    ESP_LOGI(TAG, "new_game class=%d seed=0x%08x", hero_class, (unsigned)seed);
    this->seed = seed;
    rng->setSeed(seed);
    ui_rng->setSeed(seed ^ 0x9E3779B9u);
    game_time = 0;
    anim_ms = 0;
    depth = 0;
    path_len = path_head = 0;
    anim_running = false;
    selected_class = hero_class;

    for (int i = 0; i < kMaxMob; i++)  s_mob_used[i] = false;
    for (int i = 0; i < kMaxItem; i++) s_item_used[i] = false;
    for (int i = 0; i < kMaxBuff; i++) s_buff_used[i] = false;

    Hero* h = hero;
    h->x = h->y = h->from_x = h->from_y = 0;
    h->first_buff = nullptr;
    h->flash_ticks = 0; h->move_anim = 255; h->anim_seed = 0;
    h->hp = h->hp_max = 20; h->str = 10; h->lvl = 1; h->exp = 0; h->gold = 0;
    /* 上游 Hero 基础值：HT=20 / STR=10 / attackSkill=10 / defenseSkill=5，
     * 职业差异靠「起手装备」而非属性堆叠（见下方 switch）。命中公式已改为
     * rollHit(acu,def)，若还沿用旧的 acc=50 会让所有怪近乎必中、失去上游手感。 */
    h->attack_skill = 10; h->defense_skill = 5;
    h->energy = Hero::kMaxEnergy; h->keys = 0; h->has_amulet = false;
    h->cls = (dg_class_t)hero_class;
    h->sheet = (uint8_t)(gfx::SH_HERO_WARRIOR + hero_class);
    h->alignment = 2;                            /* ALLY（英雄自己）*/
    h->name_key = k_cls_name[hero_class];
    h->inv_count = 0;
    h->equipped_weapon = h->equipped_armor = h->equipped_ring = nullptr;
    for (int i = 0; i < DG_MAX_INVENTORY; i++) h->inventory[i] = nullptr;

    /* 职业不再改基础属性（上游 Hero 四职业 HT/STR/acc/def 完全相同，
     * 差异全在起手装备与职业技能——那超出本移植范围）。职业只决定
     * 精灵图（SH_HERO_*）与招牌武器，见下方起手物品。 */

    /* 起手物品 */
    auto give = [&](int kind, int sub, int tier) -> Item* {
        Item* it = alloc_item();
        if (!it) return nullptr;
        fill_item(it, kind, sub, tier);
        if (h->inv_count < DG_MAX_INVENTORY) h->inventory[h->inv_count++] = it;
        else free_item(it);
        return it;
    };
    /* 起手物品（对齐上游：人人一件布甲 + 职业招牌武器 + 一口吃的 + 一瓶药）。
     * 法师近身靠赤手（上游法师也 MeleeWeapon 打，这里给法杖远程），
     * 战士/盗贼/猎人给 1 档近战。STR=10 正好持得住 1 档（STRReq=10）。 */
    give(Item::K_FOOD, FD_RATION, 1);
    give(Item::K_ARMOR, 0, 1);                    /* 旧布甲：开局保命 */
    if (hero_class == DG_CLASS_MAGE)      give(Item::K_WAND, WD_BOLT, 1);
    else                                  give(Item::K_WEAPON, 0, 1);
    give(Item::K_POTION, POT_HEAL, 1);

    /* 自动穿戴起手武器 + 护甲（STR=10 与 1 档 STRReq=10 持平，不算穿不动） */
    for (int i = 0; i < h->inv_count; i++) {
        Item* it = h->inventory[i];
        if (it && it->kind == Item::K_WEAPON && !h->equipped_weapon) { h->equip(it); }
        else if (it && it->kind == Item::K_ARMOR && !h->equipped_armor) { h->equip(it); }
    }

    bool ok = level->generate(seed, 0);
    if (!ok) { /* 极端坏 seed：退回一个稳妥 seed 重试一次 */
        ESP_LOGW(TAG, "generate fail, reseed");
        seed ^= 0x12345;
        rng->setSeed(seed);
        ok = level->generate(seed, 0);
    }
    this->seed = seed;
    level->depth = 0;
    h->set_pos(level->entrance_pos % DG_MAP_W, level->entrance_pos / DG_MAP_W);
    h->from_x = h->x; h->from_y = h->y;

    spawn_level_content();
    recalc_fov();

    scene = DG_SCENE_IN_GAME;
    fb_dirty = true;
    log("欢迎来到次元地牢 —— 找到 12 层的护身符就能通关。");
}

void Game::goto_title() { scene = DG_SCENE_TITLE; fb_dirty = true; }
void Game::goto_class_select() { scene = DG_SCENE_CLASS_SELECT; fb_dirty = true; }
void Game::pick_class(int cls) {
    if (cls < 0 || cls >= DG_CLASS_COUNT) cls = 0;
    selected_class = cls;
    new_game(cls, (uint32_t)esp_random());
}
void Game::menu_action(int action, int slot) {
    switch (action) {
    case 0: save::store(slot); sfx(DG_SFX_SELECT); log("已保存。"); break;
    case 1: if (save::load(slot)) { sfx(DG_SFX_SELECT); log("读取成功。"); }
            else { sfx(DG_SFX_ERROR); log("没有这个存档。"); } break;
    case 2: goto_title(); sfx(DG_SFX_SELECT); break;
    case 3: new_game(selected_class, (uint32_t)esp_random()); break;
    default: break;
    }
    fb_dirty = true;
}

/* ===== 视野 ===== */
void Game::recalc_fov() { fov::compute(level, hero->x, hero->y, kFovRadius); }

/* ===== 英雄一步 ===== */
bool Game::hero_try_step(int gx, int gy) {
    if (scene != DG_SCENE_IN_GAME) return false;
    if (gx < 0 || gx >= DG_MAP_W || gy < 0 || gy >= DG_MAP_H) return false;
    if (gx == hero->x && gy == hero->y) return false;

    Tile& t = level->at(gx, gy);

    /* 有活物：敌人就砍 */
    if (t.actor && t.actor != hero) {
        hero->attack(t.actor);
        return true;
    }

    /* 宝箱：贴着就开，不移动 */
    if (t.terr == DG_TERR_CHEST) {
        if (t.chest_open) { log("箱子已经空了。"); sfx(DG_SFX_ERROR); return false; }
        t.chest_open = 1;
        sfx(DG_SFX_CHEST);
        drop_random_item(gx, gy, 2);
        Item* gold = alloc_item();
        if (gold) { fill_item(gold, Item::K_GOLD, 0, 0); gold->qty = 20 + rng->nextInt(40) + depth * 6;
                    gold->x = gx; gold->y = gy; gold->on_drop(level); }
        log("宝箱吱呀打开，里面滚出东西来。");
        return true;
    }

    /* 上锁的门 */
    if (t.terr == DG_TERR_LOCKED_DOOR) {
        if (hero->keys > 0) {
            hero->keys--;
            t.terr = DG_TERR_OPEN_DOOR;
            sfx(DG_SFX_DOOR);
            log("钥匙转动，门开了。");
            return true;
        }
        sfx(DG_SFX_LOCKED);
        log("门上了锁 —— 需要一把钥匙。");
        return false;
    }

    if (!level->passable(gx, gy)) return false;

    /* 真正移动 */
    level->at(hero->x, hero->y).actor = nullptr;
    hero->from_x = hero->x; hero->from_y = hero->y;
    hero->move_anim = 0;
    hero->set_pos(gx, gy);
    t.actor = hero;
    sfx(DG_SFX_STEP);

    /* 关门踩开 */
    if (t.terr == DG_TERR_DOOR) { t.terr = DG_TERR_OPEN_DOOR; sfx(DG_SFX_DOOR); }

    /* 隐藏陷阱 */
    if (t.terr == DG_TERR_TRAP && !t.trap_known) {
        t.trap_known = 1;
        int dmg = 2 + depth / 2;
        hero->damage(dmg, "trap");
        add_float(gx, gy, "TRAP", 0xF800);
        sfx(DG_SFX_TRAP);
        log("脚下一空 —— 是陷阱！");
    }

    /* 拾取 */
    if (t.item) {
        Item* it = t.item;
        if (it->kind == Item::K_GOLD) {
            hero->gold += it->qty;
            char buf[48];
            snprintf(buf, sizeof(buf), "拾取 %d 金币。（合计 %d）", it->qty, hero->gold);
            log(buf);
            sfx(DG_SFX_GOLD);
            free_item(it);
            t.item = nullptr;
        } else if (hero->pickup(it)) {
            t.item = nullptr;
        }
    }
    return true;
}

/* ===== 怪物回合（M2 真实速度调度）=====
 * 每个英雄回合给每只怪累加「有效速度」（1/16 定点），满 16 才行动一次并扣 16：
 *   speed=16 → 每英雄回合动 1 次；speed=32（蟹/蝠）→ 动 2 次；speed=8 → 每 2 回合动 1 次。
 * 有效速度 = 物种 speed × 自身 HASTE(×2)/SLOW(÷2)，再按英雄状态折算：
 *   英雄 HASTE → 怪相对慢一半（÷2）；英雄 SLOW → 怪相对快一倍（×2）。
 * act_accum 跨回合保留，故非整数倍速度（如 0.5×）的节奏能正确累积。*/
void Game::advance_mobs() {
    /* 用快照长度遍历：act 里可能 add_actor（召唤/分裂）扩大表 */
    int n = level->actor_count;
    for (int i = 1; i < n && i < level->actor_count; i++) {
        Actor* a = level->actors[i];
        if (!a || !a->is_alive()) continue;
        int eff = a->speed > 0 ? a->speed : 16;
        if (a->has_buff(Buff::HASTE)) eff *= 2;
        if (a->has_buff(Buff::SLOW))  eff /= 2;
        if (hero->has_buff(Buff::HASTE)) eff /= 2;
        if (hero->has_buff(Buff::SLOW))  eff *= 2;
        if (eff < 1) eff = 1;
        a->act_accum = (uint16_t)(a->act_accum + eff);
        int guard = 0;
        while (a->act_accum >= 16 && a->is_alive() && guard++ < 4) {
            a->act();
            a->act_accum -= 16;
        }
    }
    /* 收割尸体 */
    for (int i = level->actor_count - 1; i >= 1; i--) {
        Actor* a = level->actors[i];
        if (a && !a->is_alive()) {
            level->actors[i] = level->actors[level->actor_count - 1];
            level->actors[--level->actor_count] = nullptr;
        }
    }
}

/* ===== 回合结束：饥饿 / 回血 / 视野 / 怪 ===== */
void Game::end_turn() {
    game_time++;

    /* 英雄 buff 计时与周期结算（毒/烧掉血、加速/隐身/悬浮到期…）。
     * v0.4 漏了这一步 → 英雄 buff 永不过期、中毒不掉血，M2 补上。 */
    hero->act_buffs();
    if (scene != DG_SCENE_IN_GAME) { fb_dirty = true; return; }  /* 毒发身亡 */

    /* 饥饿 */
    if (hero->energy > 0) hero->energy--;
    if (hero->energy <= 0) {
        if (hero->hp > 1) {
            hero->hp--;
            add_float(hero->x, hero->y, "-1", 0xF800);
            anim_running = true;
        } else {
            hero->die();
        }
    } else if (hero->energy > Hero::kMaxEnergy / 2 && hero->hp < hero->hp_max &&
               (game_time % 5) == 0) {
        hero->hp++;   /* 半饱以上缓慢回血 */
    }

    recalc_fov();
    if (scene == DG_SCENE_IN_GAME) advance_mobs();
    fb_dirty = true;
}

/* ===== 下楼 ===== */
bool Game::descend_stairs() {
    if (level->at(hero->x, hero->y).terr != DG_TERR_EXIT) return false;
    if (depth >= DG_MAX_DEPTH - 1) { log("已经到底了 —— 去找护身符。"); return false; }
    depth++;
    level->generate(seed, depth);
    level->depth = depth;
    hero->set_pos(level->entrance_pos % DG_MAP_W, level->entrance_pos / DG_MAP_W);
    hero->from_x = hero->x; hero->from_y = hero->y; hero->move_anim = 255;
    path_len = path_head = 0;
    spawn_level_content();
    recalc_fov();
    sfx(DG_SFX_STAIRS);
    char buf[64];
    snprintf(buf, sizeof(buf), "你下到了 %d 层。", depth + 1);
    log(buf);
    fb_dirty = true;
    ESP_LOGI(TAG, "descend -> depth %d", depth);
    return true;
}

bool Game::take_amulet() {
    if (depth != DG_MAX_DEPTH - 1) return false;
    int ax = level->amulet_pos % DG_MAP_W, ay = level->amulet_pos / DG_MAP_W;
    Tile& t = level->at(ax, ay);
    if (t.item && t.item->kind == Item::K_AMULET) { hero->pickup(t.item); t.item = nullptr; return true; }
    return false;
}

/* ===== 输入分发 ===== */
void Game::tick() {
    int64_t now_ms = esp_timer_get_time() / 1000;
    anim_ms = (uint32_t)now_ms;
    bool dirty = false;

    /* 动画推进：move_anim 与 flash_ticks */
    if (anim_running) {
        Actor* all[65];
        int cnt = 0;
        all[cnt++] = hero;
        for (int i = 1; i < level->actor_count; i++) if (level->actors[i]) all[cnt++] = level->actors[i];
        bool any = false;
        for (int i = 0; i < cnt; i++) {
            Actor* a = all[i];
            if (a->move_anim < 255) {
                int v = a->move_anim + 48;
                if (v >= 255) { a->move_anim = 255; a->from_x = a->x; a->from_y = a->y; }
                else a->move_anim = (uint8_t)v;
                any = true;
            }
            if (a->flash_ticks > 0) { a->flash_ticks--; any = true; }
        }
        /* 飘字 / 光束按 born_ms 存活，超时自然不再被 render 画出来 */
        dirty = any;
        if (!any) anim_running = false;
    }

    /* 自动行走：每 140ms 一步（比动画稍慢，保证一步到位再走下一步） */
    if (scene == DG_SCENE_IN_GAME && path_head < path_len) {
        if (!anim_running && now_ms - (int64_t)last_step_ms >= 140) {
            last_step_ms = (uint32_t)now_ms;
            int p = path_queue[path_head++];
            if (!hero_try_step(p % DG_MAP_W, p / DG_MAP_W)) {
                path_len = path_head = 0;
            } else {
                end_turn();
                dirty = true;
                if (path_head >= path_len) { path_len = path_head = 0; }
            }
        }
    }
    if (dirty) fb_dirty = true;
}

void Game::on_tap(int gx, int gy) {
    if (scene != DG_SCENE_IN_GAME) return;
    if (gx < 0 || gx >= DG_MAP_W || gy < 0 || gy >= DG_MAP_H) return;

    int dx = gx - hero->x, dy = gy - hero->y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;

    bool acted = false;
    if (adx <= 1 && ady <= 1) {
        acted = hero_try_step(gx, gy);
    } else {
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
            char m[64];
            snprintf(m, sizeof(m), "那里过不去。（goal %d,%d passable=%d）",
                     gx, gy, (int)level->passable(gx, gy));
            log(m);
        }
    }
    if (acted) { end_turn(); fb_dirty = true; }
}

/* ===== 方向键逐格移动（M1 圆屏操控）===== */
bool Game::step(int dx, int dy) {
    if (scene != DG_SCENE_IN_GAME) return false;
    if (dx < -1) dx = -1; else if (dx > 1) dx = 1;
    if (dy < -1) dy = -1; else if (dy > 1) dy = 1;
    if (dx == 0 && dy == 0) { on_button(DG_BTN_WAIT); return true; }
    path_len = path_head = 0;                 /* 手动一步：取消自动寻路 */
    bool acted = hero_try_step(hero->x + dx, hero->y + dy);
    if (acted) {
        end_turn();
        fb_dirty = true;
        if (level->at(hero->x, hero->y).terr == DG_TERR_EXIT)
            log("脚下是向下的楼梯 —— 按下楼键继续。");
    } else {
        sfx(DG_SFX_ERROR);
        fb_dirty = true;
    }
    return acted;
}

void Game::on_long_press(int gx, int gy) {
    if (scene != DG_SCENE_IN_GAME) return;
    if (gx < 0 || gx >= DG_MAP_W || gy < 0 || gy >= DG_MAP_H) return;
    Tile& t = level->at(gx, gy);
    if (!t.vis_current && !t.explored) return;
    char buf[64];
    if (t.actor && t.vis_current && t.actor != hero) {
        Mob* m = (Mob*)t.actor;
        snprintf(buf, sizeof(buf), "%s  生命 %d/%d", m->name_key ? m->name_key : "怪物",
                 m->hp, m->hp_max);
        log(buf);
    } else if (t.item && t.vis_current) {
        snprintf(buf, sizeof(buf), "%s", t.item->name ? t.item->name : "地上有东西");
        log(buf);
    } else {
        const char* tn = "地板";
        switch ((dg_terrain_t)t.terr) {
        case DG_TERR_WATER: tn = "浅水"; break;
        case DG_TERR_GRASS: case DG_TERR_HIGH_GRASS: tn = "草丛"; break;
        case DG_TERR_EXIT: tn = "向下的楼梯"; break;
        case DG_TERR_ENTRY: tn = "向上的入口"; break;
        case DG_TERR_CHEST: tn = t.chest_open ? "空宝箱" : "宝箱"; break;
        case DG_TERR_LOCKED_DOOR: tn = "上锁的门"; break;
        case DG_TERR_WALL: tn = "墙"; break;
        case DG_TERR_PEDESTAL: tn = "一座基座"; break;
        default: break;
        }
        snprintf(buf, sizeof(buf), "%s。", tn);
        log(buf);
    }
}

void Game::on_button(dg_btn_id_t btn) {
    switch (btn) {
    case DG_BTN_WAIT:
        if (scene != DG_SCENE_IN_GAME) break;
        path_len = path_head = 0;
        /* 原地等待：满饱以上回血更快（end_turn 里被动回血 + 这里额外一次机会） */
        if (hero->energy > Hero::kMaxEnergy / 3 && hero->hp < hero->hp_max &&
            ui_rng->nextInt(100) < 40) {
            hero->hp++;
            add_float(hero->x, hero->y, "+", 0x07E0);
        }
        end_turn();
        break;
    case DG_BTN_SEARCH: {
        if (scene != DG_SCENE_IN_GAME) break;
        bool found = false;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int sx = hero->x + dx, sy = hero->y + dy;
                if (sx < 0 || sx >= DG_MAP_W || sy < 0 || sy >= DG_MAP_H) continue;
                Tile& t = level->at(sx, sy);
                if (t.terr == DG_TERR_SECRET && ui_rng->nextInt(100) < 25 + hero->lvl * 5) {
                    t.terr = DG_TERR_DOOR; found = true;
                }
                if (t.terr == DG_TERR_TRAP && !t.trap_known && ui_rng->nextInt(100) < 40) {
                    t.trap_known = 1; found = true;
                }
            }
        }
        log(found ? "你发现了什么！" : "你环顾四周，什么也没发现。");
        end_turn();
        break;
    }
    case DG_BTN_INVENTORY:
        if (scene == DG_SCENE_GAME_OVER || scene == DG_SCENE_WIN) break;
        scene = (scene == DG_SCENE_INVENTORY) ? DG_SCENE_IN_GAME : DG_SCENE_INVENTORY;
        sfx(DG_SFX_SELECT);
        fb_dirty = true;
        break;
    case DG_BTN_DESCEND:
        if (scene != DG_SCENE_IN_GAME) break;
        if (!descend_stairs()) { log("这里没有向下的楼梯。"); sfx(DG_SFX_ERROR); }
        break;
    case DG_BTN_EQUIP: {
        if (scene != DG_SCENE_IN_GAME) break;
        /* 快速装备：先补武器槽再补护甲槽，各挑背包里档位最高且当前没穿更好的 */
        Item* pick = nullptr;
        Item** cur = hero->equipped_weapon ? nullptr : &hero->equipped_weapon;
        if (!hero->equipped_weapon) {
            int bt = -1;
            for (int i = 0; i < hero->inv_count; i++) {
                Item* it = hero->inventory[i];
                if (!it || it->kind != Item::K_WEAPON || it->equipped) continue;
                if (it->tier > bt) { bt = it->tier; pick = it; }
            }
            cur = pick ? &hero->equipped_weapon : nullptr;
        }
        if (!pick && !hero->equipped_armor) {
            int bt = -1;
            for (int i = 0; i < hero->inv_count; i++) {
                Item* it = hero->inventory[i];
                if (!it || it->kind != Item::K_ARMOR || it->equipped) continue;
                if (it->tier > bt) { bt = it->tier; pick = it; }
            }
            cur = pick ? &hero->equipped_armor : nullptr;
        }
        if (pick && cur) hero->equip(pick);
        else { log("没有更好的装备可用。"); sfx(DG_SFX_ERROR); }
        fb_dirty = true;
        break;
    }
    case DG_BTN_MENU:
        if (scene == DG_SCENE_GAME_OVER || scene == DG_SCENE_WIN) {
            goto_title();
        } else {
            scene = (scene == DG_SCENE_MENU) ? DG_SCENE_IN_GAME : DG_SCENE_MENU;
            sfx(DG_SFX_SELECT);
        }
        fb_dirty = true;
        break;
    default: break;
    }
}

/* ===== 特效投递 ===== */
void Game::add_float(int x, int y, const char* text, uint16_t color) {
    /* 找一个空槽（优先最旧） */
    int slot = -1;
    for (int i = 0; i < kMaxFloats; i++) if (!floats[i].used) { slot = i; break; }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFF;
        for (int i = 0; i < kMaxFloats; i++) if (floats[i].born_ms < oldest) { oldest = floats[i].born_ms; slot = i; }
    }
    FloatText& f = floats[slot];
    f.x = (int16_t)x; f.y = (int16_t)y;
    f.born_ms = anim_ms;
    f.color = color;
    f.used = true;
    strncpy(f.text, text, sizeof(f.text) - 1);
    f.text[sizeof(f.text) - 1] = '\0';
    anim_running = true;
    fb_dirty = true;
}
void Game::add_beam(int x0, int y0, int x1, int y1, uint16_t color) {
    int slot = -1;
    for (int i = 0; i < kMaxBeams; i++) if (!beams[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;
    Beam& b = beams[slot];
    b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1;
    b.born_ms = anim_ms; b.color = color; b.used = true;
    anim_running = true;
    fb_dirty = true;
}
void Game::flash_actor(Actor* a) { if (a) { a->flash_ticks = 3; anim_running = true; fb_dirty = true; } }

/* ===== 音效队列 ===== */
void Game::sfx(int id) {
    if (id <= DG_SFX_NONE || id >= DG_SFX_COUNT) return;
    int tail = (sfx_head + sfx_count) % kMaxSfx;
    if (sfx_count < kMaxSfx) {
        sfx_ring[tail] = (uint8_t)id;
        sfx_count++;
    } else {
        /* 满：丢最旧（head 前移） */
        sfx_ring[sfx_head] = (uint8_t)id;
        sfx_head = (sfx_head + 1) % kMaxSfx;
    }
}
bool Game::pop_sfx(int* id) {
    if (sfx_count <= 0) return false;
    *id = sfx_ring[sfx_head];
    sfx_head = (sfx_head + 1) % kMaxSfx;
    sfx_count--;
    return true;
}

/* ===== 渲染出口 ===== */
const uint16_t* Game::get_tilemap_fb(int* w, int* h) {
    if (w) *w = DG_VIEWPORT_W;
    if (h) *h = DG_VIEWPORT_H;
    if (!gfx::ready() && !gfx::load()) return nullptr;   /* 无素材：UI 回退占位 */
    if (!s_fb) {
        s_fb = (uint16_t*)heap_caps_malloc(DG_VIEWPORT_W * DG_VIEWPORT_H * 2, MALLOC_CAP_SPIRAM);
        if (!s_fb) { ESP_LOGE(TAG, "fb alloc fail"); return nullptr; }
        memset(s_fb, 0, DG_VIEWPORT_W * DG_VIEWPORT_H * 2);
        fb_dirty = true;
    }
    if (scene != DG_SCENE_IN_GAME && scene != DG_SCENE_INVENTORY) return fb_dirty ? s_fb : nullptr;
    if (!fb_dirty) return nullptr;
    render::frame(*this, s_fb, DG_VIEWPORT_W, DG_VIEWPORT_H);
    fb_dirty = false;
    return s_fb;
}

/* ===== HUD / 文本 ===== */
static int hunger_state(int energy) {
    if (energy <= 0) return 3;
    int r3 = Hero::kMaxEnergy / 3;
    if (energy <= r3) return 2;
    if (energy <= r3 * 2) return 1;
    return 0;
}
void Game::get_hud(dg_hud_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->hp = hero->hp; out->hp_max = hero->hp_max;
    out->exp = hero->exp; out->exp_max = hero->maxExp();
    out->gold = hero->gold; out->str = hero->str;
    out->energy = hero->energy; out->energy_max = Hero::kMaxEnergy;
    out->depth = depth + 1; out->chapter = level->chapter();
    out->lvl = hero->lvl; out->cls = hero->cls;
    out->has_amulet = hero->has_amulet ? 1 : 0;
    out->on_stairs = (level->at(hero->x, hero->y).terr == DG_TERR_EXIT) ? 1 : 0;
    out->keys = hero->keys > 255 ? 255 : (uint8_t)hero->keys;
    out->hunger_state = (uint8_t)hunger_state(hero->energy);
}

bool Game::inv_get(int slot, dg_item_info_t* out) {
    if (slot < 0 || slot >= hero->inv_count || !out) return false;
    Item* it = hero->inventory[slot];
    if (!it) return false;
    out->kind = (int16_t)it->kind;
    out->sub = it->sub; out->qty = it->qty; out->icon = it->icon;
    out->tier = it->tier; out->str_req = it->str_req;
    out->equipped = it->equipped; out->cursed = it->cursed;
    out->name = it->name ? it->name : "?";
    return true;
}
bool Game::inv_use(int slot) {
    if (scene != DG_SCENE_IN_GAME) return false;
    bool r = hero->use(slot);
    if (r) { end_turn(); }
    fb_dirty = true;
    return r;
}
bool Game::inv_equip(int slot) {
    if (slot < 0 || slot >= hero->inv_count) return false;
    Item* it = hero->inventory[slot];
    if (!it || !it->is_equipment()) { sfx(DG_SFX_ERROR); return false; }
    bool r = (it->equipped) ? hero->unequip(it) : hero->equip(it);
    fb_dirty = true;
    return r;
}
void Game::inv_drop(int slot) {
    hero->drop(slot);
    fb_dirty = true;
}
int Game::equip_mask() {
    int m = 0;
    if (hero->equipped_weapon) m |= 1;
    if (hero->equipped_armor)  m |= 2;
    if (hero->equipped_ring)   m |= 4;
    return m;
}

int Game::get_status_text(char* buf, int cap) {
    if (scene < DG_SCENE_IN_GAME) return 0;
    return snprintf(buf, cap, "生命 %d/%d · %d层 · 金币 %d",
                    hero->hp, hero->hp_max, depth + 1, hero->gold);
}
int Game::get_message(char* buf, int cap, int index) {
    if (index < 0 || index >= kLogLines) return 0;
    int from = (log_head - 1 - index + kLogLines * 2) % kLogLines;
    const char* line = log_lines[from];
    if (!line[0]) return 0;
    return snprintf(buf, cap, "%s", line);
}
int Game::get_stats_text(char* buf, int cap) {
    if (scene < DG_SCENE_IN_GAME) return 0;
    return snprintf(buf, cap,
        "%s  Lv %d\n生命 %d/%d  经验 %d/%d\n力量 %d  命中 %d  闪避 %d\n金币 %d  钥匙 %d\n饥饿 %d/%d  深度 %dF  回合 %u",
        k_cls_name[hero->cls], hero->lvl, hero->hp, hero->hp_max, hero->exp, hero->maxExp(),
        hero->str, hero->attack_skill, hero->defense_skill,
        hero->gold, hero->keys, hero->energy, Hero::kMaxEnergy, depth + 1, (unsigned)game_time);
}

int Game::debug_dump(char* buf, int cap) {
    if (!level || !hero) return 0;
    int n = snprintf(buf, cap, "D h=%d,%d c=%d,%d s=%d/%d lv=%d g=%d d=%dF e=",
                     hero->x, hero->y, cam_x, cam_y,
                     hero->hp, hero->hp_max, hero->lvl, hero->gold, depth + 1);
    if (n < 0 || n >= cap) return 0;
    int ex = level->exit_pos % DG_MAP_W, ey = level->exit_pos / DG_MAP_W;
    int w = snprintf(buf + n, cap - n, "%d,%d i=", ex, ey);
    if (w < 0 || n + w >= cap) return 0;
    n += w;
    int items = 0, mobs = 0;
    for (int y = 0; y < DG_MAP_H && n < cap; y++) {
        for (int x = 0; x < DG_MAP_W && n < cap; x++) {
            Tile& t = level->at(x, y);
            if (t.item && items < 4) {
                w = snprintf(buf + n, cap - n, "%s%d,%d", items ? "|" : "", x, y);
                if (w < 0 || n + w >= cap) return 0;
                n += w;
                items++;
            }
            if (t.actor && t.actor != (Actor*)hero && t.vis_current && mobs < 4) {
                w = snprintf(buf + n, cap - n, "%s%d,%d", mobs ? "|" : "", x, y);
                if (w < 0 || n + w >= cap) return 0;
                n += w;
                mobs++;
            }
        }
    }
    return n;
}

/* 英雄当前 buff 列表（取证用）："type:剩余 " 序列。 */
int Game::hero_buffs(char* buf, int cap) {
    if (!hero || cap <= 0) return 0;
    static const char* kNames[Buff::TYPE_COUNT] = {
        "none","slow","haste","invis","mindvision","levitation",
        "poison","burning","sleep","paralysis","fright","roots","ointment",
    };
    int n = 0;
    for (Buff* b = hero->first_buff; b; b = b->next) {
        const char* nm = (b->type < Buff::TYPE_COUNT) ? kNames[b->type] : "?";
        int w = snprintf(buf + n, cap - n, "%s:%d ", nm, b->duration);
        if (w < 0 || n + w >= cap) break;
        n += w;
    }
    return n;
}

}  /* namespace dg */
