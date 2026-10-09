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
    /* 防御性回收：任何绕过 Actor::die 的释放路径（selftest 直接 free、
     * 层内容重置等）都不该把怪身上的 intrusive buff 链留在池子里。
     * Actor::die 已抽干并置空 first_buff，这里对它是 no-op。 */
    while (m->first_buff) {
        Buff* b = m->first_buff;
        m->first_buff = b->next;
        free_buff(b);
    }
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
    /* 回收上一级残留：怪物全清、地上物品清（背包/身上保留）、buff 全清。
     * 例外：12F 的护身符是在 generate(reset_view 之后) 才落到基座上的本层物品，
     * 不能被这条「清非持有地上物」误回收（否则槽位被后续 mob/drop 复用 → 到 12F 拿不到护身符）。*/
    for (int i = 0; i < kMaxMob; i++)  s_mob_used[i] = false;
    for (int i = 0; i < kMaxItem; i++)  if (s_item_used[i] && !held_by_hero(&s_item_pool[i])
                                            && s_item_pool[i].kind != Item::K_AMULET)
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
    case 0: if (save::store(slot)) { sfx(DG_SFX_SELECT); log("已保存。"); scene = DG_SCENE_IN_GAME; }
            else { sfx(DG_SFX_ERROR); log("保存失败。"); } break;
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
        /* 宝箱怪（M5）：开箱瞬间长出腿和牙齿，就地变成一只 ENEMY 怪；
         * 宝箱变地板让怪站得住，它下一回合自然起手。 */
        if (t.mimic) {
            t.mimic = 0; t.chest_open = 1; t.terr = DG_TERR_FLOOR;
            const MobSpec* ms = &MOB_SPECS[MOB_MIMIC];
            Mob* mk = alloc_mob();
            if (mk) {
                *mk = Mob{};
                mk->spec = ms; mk->sheet = ms->sheet; mk->name_key = ms->name;
                mk->hp_max = ms->hp; mk->hp = ms->hp;
                mk->attack_min = ms->atk_min; mk->attack_max = ms->atk_max;
                mk->defense = ms->def; mk->xp_in_kill = ms->xp; mk->see_range = ms->see;
                mk->speed = ms->speed; mk->alignment = 1; mk->state = Mob::HUNTING;
                mk->level = level; mk->first_buff = nullptr; mk->flash_ticks = 0;
                mk->move_anim = 255; mk->anim_seed = (uint8_t)rng->nextInt(256);
                mk->from_x = mk->x = gx; mk->from_y = mk->y = gy;
                mk->home_x = gx; mk->home_y = gy;
                t.actor = mk; level->add_actor(mk);
                sfx(DG_SFX_KILL);
                log("宝箱长出了腿和牙齿——是宝箱怪！");
                fb_dirty = true;
                return true;
            }
            /* 池满退化为普通宝箱 */
        }
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
            /* 金色飘字 +N：拾金此前只有音效与日志，无视觉反馈（与伤害/治疗不一致） */
            char fb[12];
            snprintf(fb, sizeof(fb), "+%d", it->qty);
            add_float(hero->x, hero->y, fb, 0xFDC0);
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
        /* 急速之戒：相当于英雄自带 HASTE，周围怪相对变慢（M4）*/
        if (hero->equipped_ring && hero->equipped_ring->sub == RG_HASTE) eff /= 2;
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

    /* 再生之戒：每 3 回合回 1 血，不要求半饱（M4）*/
    if (hero->equipped_ring && hero->equipped_ring->sub == RG_REGEN &&
        hero->hp < hero->hp_max && (game_time % 3) == 0) {
        hero->hp++;
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
        /* 飘字 / 光束按 born_ms 存活：存活期内保持 anim_running，让 fb 每帧
         * 重画（飘字上移 / 光束淡出）。此前 any 只统计移动/闪白，实体静止时
         * fb 不脏 → 飘字只画出生帧就僵住，750ms 后瞬消——打击感缺失的根因。 */
        for (int i = 0; i < kMaxFloats && !any; i++) if (floats[i].used) any = true;
        for (int i = 0; i < kMaxBeams  && !any; i++) if (beams[i].used)  any = true;
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
        /* 点英雄自身格 = 原地等待（上游 onTAP 语义）；工具栏不再有等待键 */
        if (gx == hero->x && gy == hero->y) { on_button(DG_BTN_WAIT); return; }
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
    case DG_BTN_HERO:
        if (scene == DG_SCENE_GAME_OVER || scene == DG_SCENE_WIN) break;
        scene = (scene == DG_SCENE_HERO) ? DG_SCENE_IN_GAME : DG_SCENE_HERO;
        sfx(DG_SFX_SELECT);
        fb_dirty = true;
        break;
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
    /* 出口路点（UI 画呼吸高亮）：explored = 已被揭雾记忆，不在 FOV 内也可作
     * 为路标；站在出口上时 UI 改亮 DESCEND 键并隐藏路点，避免重叠。 */
    int ex = level->exit_pos % DG_MAP_W, ey = level->exit_pos / DG_MAP_W;
    out->exit_x = (int16_t)ex; out->exit_y = (int16_t)ey;
    out->exit_seen = level->at(ex, ey).explored ? 1 : 0;
}

/* 英雄属性面板：全部现算（含戒指/护甲加成），UI 不做任何公式 */
void Game::get_stats(dg_stats_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->lvl = hero->lvl; out->str = hero->str;
    out->hp = hero->hp;   out->hp_max = hero->hp_max;
    out->exp = hero->exp; out->exp_max = hero->maxExp();
    out->atk_skill = hero->attackSkill();
    out->def_skill = hero->defenseSkill();
    if (hero->equipped_weapon) {
        out->dmg_lo = weapon_dmg_min(hero->equipped_weapon->tier);
        out->dmg_hi = weapon_dmg_max(hero->equipped_weapon->tier);
    } else {
        out->dmg_lo = 1; out->dmg_hi = 3;          /* 空手（上游口径） */
    }
    out->armor_dr = hero->armorDrMax();
    out->gold = hero->gold; out->keys = hero->keys;
    out->energy = hero->energy; out->energy_max = Hero::kMaxEnergy;
    out->hunger_state = (uint8_t)hunger_state(hero->energy);
    out->cls = hero->cls;
    Item* eq[3] = { hero->equipped_weapon, hero->equipped_armor, hero->equipped_ring };
    int16_t* icon_out[3] = { &out->wep_icon, &out->arm_icon, &out->rng_icon };
    const char** name_out[3] = { &out->wep_name, &out->arm_name, &out->rng_name };
    for (int i = 0; i < 3; i++) {
        if (!eq[i]) continue;
        *icon_out[i] = eq[i]->icon;
        *name_out[i] = item_display(eq[i]->kind, eq[i]->sub, eq[i]->tier,
                                    is_identified(eq[i]->kind, eq[i]->sub));
    }
}

bool Game::inv_get(int slot, dg_item_info_t* out) {
    if (slot < 0 || slot >= hero->inv_count || !out) return false;
    Item* it = hero->inventory[slot];
    if (!it) return false;
    out->kind = (int16_t)it->kind;
    out->sub = it->sub; out->qty = it->qty; out->icon = it->icon;
    out->tier = it->tier; out->str_req = it->str_req;
    out->equipped = it->equipped; out->cursed = it->cursed;
    /* 未鉴定的药水/卷轴/戒指/法杖显「未鉴定的X」，其余显真名 */
    out->name = item_display(it->kind, it->sub, it->tier, is_identified(it->kind, it->sub));
    return true;
}
bool Game::inv_use(int slot) {
    /* 背包面板打开时引擎场景就是 INVENTORY：此前只放行 IN_GAME，
     * 面板里点「使用」必返回 false（看起来就是按了没反应） */
    if (scene != DG_SCENE_IN_GAME && scene != DG_SCENE_INVENTORY) return false;
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

/* SC_IDENTIFY：把身上与背包里所有消耗品/戒指一次性鉴定。 */
void Game::identify_all_carried() {
    if (!hero) return;
    for (int i = 0; i < hero->inv_count; i++) {
        Item* it = hero->inventory[i];
        if (it) mark_identified(it->kind, it->sub);
    }
    if (hero->equipped_ring) mark_identified(hero->equipped_ring->kind, hero->equipped_ring->sub);
}

/* ===== M4 取证：物品全谱自检（串口 'p'）===== 逐项 ESP_LOGI 打 "M4 ... PASS/FAIL"。 */
void Game::debug_m4_selftest() {
    if (scene != DG_SCENE_IN_GAME) new_game(DG_CLASS_WARRIOR, 20261006u);
    if (!hero || !level) { ESP_LOGE("dg.m4", "M4 no-hero/level"); return; }
    hero->hp_max = 100000; hero->hp = 100000;
    const int saved_as = hero->attack_skill;
    ESP_LOGI("dg.m4", "M4 SELFTEST BEGIN");

    auto clear_mobs = [&]() {
        for (int i = 0; i < Level::LENGTH; i++)
            if (level->tiles[i].actor && level->tiles[i].actor != (Actor*)hero)
                level->tiles[i].actor = nullptr;
        level->actor_count = 1;
        level->actors[1] = nullptr;
        /* 直接复位位图会绕过 free_mob，先手动抽干池内怪的 buff 链免得泄槽 */
        for (int i = 0; i < kMaxMob; i++) {
            if (!s_mob_used[i]) continue;
            Mob* m = &s_mob_pool[i];
            while (m->first_buff) { Buff* b = m->first_buff; m->first_buff = b->next; free_buff(b); }
            s_mob_used[i] = false;
        }
    };
    auto spawn_mob = [&](const MobSpec* s) -> Mob* {
        Mob* m = alloc_mob(); if (!m) return nullptr;
        *m = Mob{};
        m->spec = s; m->sheet = s->sheet; m->name_key = s->name;
        m->hp_max = s->hp; m->hp = s->hp;
        m->attack_min = s->atk_min; m->attack_max = s->atk_max;
        m->defense = s->def; m->xp_in_kill = s->xp; m->see_range = s->see; m->speed = s->speed;
        m->alignment = 1; m->state = Mob::HUNTING; m->level = level; m->first_buff = nullptr;
        m->move_anim = 255; m->flash_ticks = 0;
        static const int dx8[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
        static const int dy8[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
        for (int i = 0; i < 8; i++) {
            int nx = hero->x + dx8[i], ny = hero->y + dy8[i];
            if (!level->passable(nx, ny) || level->at(nx, ny).actor) continue;
            m->from_x = m->x = nx; m->from_y = m->y = ny; m->home_x = nx; m->home_y = ny;
            level->at(nx, ny).actor = m; level->add_actor(m); recalc_fov();
            return m;
        }
        free_mob(m); return nullptr;
    };
    auto drop_mob = [&](Mob* m) {
        if (!m) return;
        if (level->at(m->x, m->y).actor == m) level->at(m->x, m->y).actor = nullptr;
        level->del_actor(m); free_mob(m);
    };

    /* 1. 鉴定：is_identified/mark_identified + 未鉴定泛称 / 真名 */
    {
        for (int i = 0; i < 6; i++) ident_bits[i] = 0;
        bool id0 = is_identified(Item::K_POTION, POT_HEAL);
        bool unid = strstr(item_display(Item::K_POTION, POT_HEAL, 0, id0), "\u672a\u9274\u5b9a") != nullptr;
        mark_identified(Item::K_POTION, POT_HEAL);
        bool id1 = is_identified(Item::K_POTION, POT_HEAL);
        bool real = strstr(item_display(Item::K_POTION, POT_HEAL, 0, id1), "\u6cbb\u7597") != nullptr;
        ESP_LOGI("dg.m4", "M4 IDENTIFY unid=%d real=%d %s", unid, real,
                 (!id0 && id1 && unid && real) ? "PASS" : "FAIL");
    }

    /* 2. 自动鉴定：用一次治疗药水→该 (kind,sub) 变已鉴定 */
    {
        for (int i = 0; i < 6; i++) ident_bits[i] = 0;
        hero->hp = 5;
        Item* it = alloc_item();
        if (it) {
            fill_item(it, Item::K_POTION, POT_HEAL, 0);
            item_use(hero, it);
            ESP_LOGI("dg.m4", "M4 AUTOID_ON_USE %s", is_identified(Item::K_POTION, POT_HEAL) ? "PASS" : "FAIL");
            free_item(it);
        }
    }

    /* 3. 经验药水 + gainExp 升级 */
    {
        hero->exp = 0;
        int lvl0 = hero->lvl;
        Item* it = alloc_item();
        if (it) {
            fill_item(it, Item::K_POTION, POT_EXPERIENCE, 0);
            int used = item_use(hero, it);
            ESP_LOGI("dg.m4", "M4 POT_EXP lv %d->%d %s", lvl0, hero->lvl,
                     (used == 1 && hero->lvl > lvl0) ? "PASS" : "FAIL");
            free_item(it);
        }
    }

    /* 4. SC_RAGE：HASTE + 命中上升 */
    {
        hero->remove_buff(Buff::HASTE);
        int as0 = hero->attack_skill;
        Item* it = alloc_item();
        if (it) {
            fill_item(it, Item::K_SCROLL, SC_RAGE, 0);
            int used = item_use(hero, it);
            ESP_LOGI("dg.m4", "M4 SC_RAGE haste=%d as+%d %s",
                     hero->has_buff(Buff::HASTE) ? 1 : 0, hero->attack_skill - as0,
                     (used == 1 && hero->has_buff(Buff::HASTE) && hero->attack_skill == as0 + 5) ? "PASS" : "FAIL");
            free_item(it);
        }
    }

    /* 5. SC_FEAR：周围怪上 FRIGHT */
    {
        clear_mobs();
        Mob* m = spawn_mob(&MOB_SPECS[MOB_RAT]);
        Item* it = alloc_item();
        if (m && it) {
            fill_item(it, Item::K_SCROLL, SC_FEAR, 0);
            int used = item_use(hero, it);
            ESP_LOGI("dg.m4", "M4 SC_FEAR %s", (used == 1 && m->has_buff(Buff::FRIGHT)) ? "PASS" : "FAIL");
        } else ESP_LOGE("dg.m4", "M4 SC_FEAR setup FAIL");
        drop_mob(m); if (it) free_item(it);
    }

    /* 6. SC_SLEEP：普通怪沉睡，亡灵免疫 */
    {
        clear_mobs();
        Mob* m1 = spawn_mob(&MOB_SPECS[MOB_BAT]);
        Mob* m2 = spawn_mob(&MOB_SPECS[MOB_SKELETON]);
        Item* it = alloc_item();
        if (m1 && m2 && it) {
            fill_item(it, Item::K_SCROLL, SC_SLEEP, 0);
            item_use(hero, it);
            bool ok = m1->has_buff(Buff::SLEEP) && !m2->has_buff(Buff::SLEEP);
            ESP_LOGI("dg.m4", "M4 SC_SLEEP sleep=%d undead-immune=%d %s",
                     m1->has_buff(Buff::SLEEP) ? 1 : 0, m2->has_buff(Buff::SLEEP) ? 1 : 0,
                     ok ? "PASS" : "FAIL");
        } else ESP_LOGE("dg.m4", "M4 SC_SLEEP setup FAIL");
        drop_mob(m1); drop_mob(m2); if (it) free_item(it);
    }

    /* 7. WD_FLAME / WD_CHILL */
    {
        clear_mobs();
        Mob* m = spawn_mob(&MOB_SPECS[MOB_RAT]);
        m->hp = m->hp_max = 300;              /* 不被一发秒，便于观察 buff */
        Item* it = alloc_item();
        if (m && it) {
            fill_item(it, Item::K_WAND, WD_FLAME, 0); it->qty = 3;
            item_use(hero, it);
            ESP_LOGI("dg.m4", "M4 WD_FLAME %s", m->has_buff(Buff::BURNING) ? "PASS" : "FAIL");
            fill_item(it, Item::K_WAND, WD_CHILL, 0); it->qty = 3;
            item_use(hero, it);
            ESP_LOGI("dg.m4", "M4 WD_CHILL %s",
                     (m->has_buff(Buff::ROOTS) && m->has_buff(Buff::SLOW)) ? "PASS" : "FAIL");
            free_item(it);
        } else ESP_LOGE("dg.m4", "M4 WAND setup FAIL");
        drop_mob(m);
    }

    /* 8. RG_REGEN：装备后每几回合回血 */
    {
        clear_mobs();
        Item* ring = alloc_item();
        if (ring) {
            fill_item(ring, Item::K_RING, RG_REGEN, 0);
            hero->equip(ring);
            hero->hp = hero->hp_max - 20;
            int before = hero->hp;
            for (int t = 0; t < 8 && hero->hp <= before; t++) end_turn();
            ESP_LOGI("dg.m4", "M4 RG_REGEN %d->%d %s", before, hero->hp, hero->hp > before ? "PASS" : "FAIL");
            hero->unequip(ring); free_item(ring);
        }
    }

    /* 9. RG_THORNS：近战受击反弹 */
    {
        clear_mobs();
        Mob* m = spawn_mob(&MOB_SPECS[MOB_RAT]);
        m->hp = m->hp_max = 300;              /* 别被反弹打死 */
        Item* ring = alloc_item();
        if (m && ring) {
            fill_item(ring, Item::K_RING, RG_THORNS, 0);
            hero->equip(ring);
            int mhp0 = m->hp;
            m->state = Mob::HUNTING;
            for (int t = 0; t < 6 && m->hp >= mhp0; t++) m->act();   /* 贴身普攻→反复触发反弹 */
            ESP_LOGI("dg.m4", "M4 RG_THORNS mobhp %d->%d %s", mhp0, m->hp, m->hp < mhp0 ? "PASS" : "FAIL");
            hero->unequip(ring); free_item(ring);
        } else ESP_LOGE("dg.m4", "M4 RG_THORNS setup FAIL");
        drop_mob(m);
    }

    /* 10. 附魔 proc：BLAZING 点燃 / VAMPIRIC 回血 */
    {
        clear_mobs();
        hero->attack_skill = 100000;         /* 保证命中 */
        Item* wep = alloc_item();
        Mob* m = spawn_mob(&MOB_SPECS[MOB_RAT]);
        if (m) m->hp = m->hp_max = 300;
        if (wep && m) {
            fill_item(wep, Item::K_WEAPON, 0, 1); wep->str_req = 0; wep->enchant = EN_BLAZING;
            hero->equip(wep);
            hero->attack(m);
            ESP_LOGI("dg.m4", "M4 EN_BLAZING %s", m->has_buff(Buff::BURNING) ? "PASS" : "FAIL");
            hero->unequip(wep);
        } else ESP_LOGE("dg.m4", "M4 EN_BLAZING setup FAIL");
        drop_mob(m);
        m = spawn_mob(&MOB_SPECS[MOB_RAT]);
        if (m) m->hp = m->hp_max = 300;
        if (wep && m) {
            fill_item(wep, Item::K_WEAPON, 0, 5); wep->str_req = 0; wep->enchant = EN_VAMPIRIC;
            hero->equip(wep);
            hero->hp = hero->hp_max - 200;
            int hb = hero->hp;
            hero->attack(m);
            ESP_LOGI("dg.m4", "M4 EN_VAMPIRIC hp %d->%d %s", hb, hero->hp, hero->hp > hb ? "PASS" : "FAIL");
            hero->unequip(wep);
        } else ESP_LOGE("dg.m4", "M4 EN_VAMPIRIC setup FAIL");
        hero->attack_skill = saved_as;
        drop_mob(m); if (wep) free_item(wep);
    }

    clear_mobs();
    /* 收尾：回一层干净开局 */
    level->generate(seed, 0); level->depth = 0; depth = 0;
    hero->hp_max = 20; hero->hp = 20; hero->attack_skill = saved_as;
    hero->set_pos(level->entrance_pos % DG_MAP_W, level->entrance_pos / DG_MAP_W);
    hero->from_x = hero->x; hero->from_y = hero->y; hero->move_anim = 255;
    spawn_level_content(); recalc_fov(); game_time = 1;
    ESP_LOGI("dg.m4", "M4 SELFTEST END");
}

/* ===== M5 取证：关卡生成可达性 + 房间多样 + 宝箱怪自检（串口 'y'）=====
 * 不做脆弱的真机导航，而是直接逐层 generate + 洪水填充，断言：
 *   ① 每层出口从入口可达（passable() 口径：墙/秘密门/上锁门/宝箱/雕像为阻）；
 *   ② 入口 != 出口；12F 有基座与护身符；
 *   ③ 宝箱怪功能：把临近格标成 mimic 宝箱，踩上应就地变出一只 MIMIC 怪。 */
void Game::debug_m5_selftest() {
    if (scene != DG_SCENE_IN_GAME) new_game(DG_CLASS_WARRIOR, 20261006u);
    if (!hero || !level) { ESP_LOGE("dg.m5", "M5 no-hero/level"); return; }
    const uint32_t s = seed;
    static uint8_t vis[DG_MAP_W * DG_MAP_H];
    static int qx[DG_MAP_W * DG_MAP_H], qy[DG_MAP_W * DG_MAP_H];
    int pass = 0, fail = 0;
    ESP_LOGI("dg.m5", "M5 SELFTEST BEGIN seed=%u", (unsigned)s);

    /* 给定 seed+depth 生成一层，从入口 BFS（只走 passable() 为真的格），
     * 判出口是否可达且与入口不同。 */
    auto exit_reachable = [&](uint32_t sd, int d) -> bool {
        level->generate(sd, d);
        level->depth = d;
        int sx = level->entrance_pos % DG_MAP_W, sy = level->entrance_pos / DG_MAP_W;
        int tx = level->exit_pos % DG_MAP_W,       ty = level->exit_pos / DG_MAP_W;
        if (sx == tx && sy == ty) return false;
        memset(vis, 0, sizeof(vis));
        int head = 0, tail = 0;
        qx[tail] = sx; qy[tail] = sy; tail++;
        vis[sx + sy * DG_MAP_W] = 1;
        static const int dx4[4] = { 0, 1, 0, -1 };
        static const int dy4[4] = { -1, 0, 1, 0 };
        while (head < tail) {
            int x = qx[head], y = qy[head]; head++;
            for (int k = 0; k < 4; k++) {
                int nx = x + dx4[k], ny = y + dy4[k];
                if (nx < 0 || ny < 0 || nx >= DG_MAP_W || ny >= DG_MAP_H) continue;
                int idx = nx + ny * DG_MAP_W;
                if (vis[idx]) continue;
                if ((nx != tx || ny != ty) && !level->passable(nx, ny)) continue;
                vis[idx] = 1; qx[tail] = nx; qy[tail] = ny; tail++;
            }
        }
        return vis[tx + ty * DG_MAP_W] != 0;
    };

    /* 主 seed：逐层详细打印 */
    for (int d = 0; d < DG_MAX_DEPTH; d++) {
        bool ok = exit_reachable(s, d);
        ESP_LOGI("dg.m5", "M5 depth%d %s", d + 1, ok ? "PASS" : "FAIL");
        ok ? pass++ : fail++;
    }

    /* 跨 seed 压力：8 个 seed × 12 层，任何断连都记一次（断连是稀事件，多 seed 才抓得住）*/
    int st_total = 0, st_fail = 0;
    for (int k = 0; k < 8; k++) {
        uint32_t sd = s ^ (uint32_t)(k * 0x9E3779B1u + 0x12345u);
        for (int d = 0; d < DG_MAX_DEPTH; d++) { st_total++; if (!exit_reachable(sd, d)) st_fail++; }
    }
    ESP_LOGI("dg.m5", "M5 REACH-STRESS total=%d fail=%d %s", st_total, st_fail, st_fail == 0 ? "PASS" : "FAIL");
    if (st_fail) fail++;

    /* 宝箱怪功能测试：回到当前层，在英雄旁空格里放一个 mimic 宝箱，踩上应变出怪 */
    level->generate(s, 0); level->depth = 0; depth = 0;
    hero->set_pos(level->entrance_pos % DG_MAP_W, level->entrance_pos / DG_MAP_W);
    hero->from_x = hero->x; hero->from_y = hero->y; hero->move_anim = 255;
    level->at(hero->x, hero->y).actor = hero;
    level->add_actor(hero);   /* 纳入演员表，使 MIMIC 落在 actors[1]，搜索从 i=1 才对得上 */
    int cx = -1, cy = -1;
    static const int dx8[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    static const int dy8[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    for (int i = 0; i < 8; i++) {
        int nx = hero->x + dx8[i], ny = hero->y + dy8[i];
        if (nx < 0 || ny < 0 || nx >= DG_MAP_W || ny >= DG_MAP_H) continue;
        Tile& t = level->at(nx, ny);
        if (t.terr != DG_TERR_FLOOR || t.actor || t.item) continue;
        cx = nx; cy = ny; break;
    }
    bool mimic_ok = false;
    if (cx >= 0) {
        Tile& t = level->at(cx, cy);
        t.terr = DG_TERR_CHEST; t.chest_open = 0; t.mimic = 1;
        bool stepped = hero_try_step(cx, cy);
        Mob* mk = nullptr;
        for (int i = 1; i < level->actor_count; i++) {
            Mob* m = static_cast<Mob*>(level->actors[i]);
            if (m && m->is_alive() && m->spec && m->spec->sheet == (uint8_t)gfx::SH_MOB_MIMIC) { mk = m; break; }
        }
        mimic_ok = stepped && mk != nullptr;
        ESP_LOGI("dg.m5", "M5 MIMIC_CHEST stepped=%d spawned=%d %s",
                 stepped ? 1 : 0, mk ? 1 : 0, mimic_ok ? "PASS" : "FAIL");
        if (mk) { level->at(mk->x, mk->y).actor = nullptr; level->del_actor(mk); free_mob(mk); }
    } else {
        ESP_LOGE("dg.m5", "M5 MIMIC_CHEST no-spot FAIL");
    }

    /* 收尾：回一层干净开局，清掉测试期间的 mimic/怪 */
    for (int i = 0; i < kMaxMob; i++) {
        if (!s_mob_used[i]) continue;
        Mob* m = &s_mob_pool[i];
        while (m->first_buff) { Buff* b = m->first_buff; m->first_buff = b->next; free_buff(b); }
        s_mob_used[i] = false;
    }
    level->generate(seed, 0); level->depth = 0; depth = 0;
    hero->set_pos(level->entrance_pos % DG_MAP_W, level->entrance_pos / DG_MAP_W);
    hero->from_x = hero->x; hero->from_y = hero->y; hero->move_anim = 255;
    spawn_level_content(); recalc_fov(); game_time = 1;
    ESP_LOGI("dg.m5", "M5 SUMMARY reachable=%d/%d mimic=%d %s",
             pass, DG_MAX_DEPTH, mimic_ok ? 1 : 0,
             (fail == 0 && mimic_ok) ? "ALL PASS" : "CHECK ABOVE");
}

/* M6 路径回放专用：从 (sx,sy) 沿 passable() BFS 铺父指针（目标格特殊放行）。
 * 返回可达；vis 非 0 格可回溯：起点→目标 = 反复读 par[idx] 直到起点。 */
static bool m6_flood_bfs(Level* lv, int sx, int sy, int tx, int ty,
                         uint8_t* vis, int* par_x, int* par_y)
{
    static const int dx4[4] = { 0, 1, 0, -1 };
    static const int dy4[4] = { -1, 0, 1, 0 };
    memset(vis, 0, DG_MAP_W * DG_MAP_H);
    int head = 0, tail = 0;
    static int q[DG_MAP_W * DG_MAP_H];   /* static：bsp_console 栈小，4KB 上栈会溢出 */
    q[tail++] = sx + sy * DG_MAP_W;
    vis[sx + sy * DG_MAP_W] = 1;
    while (head < tail) {
        int idx = q[head++];
        if (idx == tx + ty * DG_MAP_W) break;
        int x = idx % DG_MAP_W, y = idx / DG_MAP_W;
        for (int k2 = 0; k2 < 4; k2++) {
            int nx = x + dx4[k2], ny = y + dy4[k2];
            if (nx < 0 || ny < 0 || nx >= DG_MAP_W || ny >= DG_MAP_H) continue;
            int ni = nx + ny * DG_MAP_W;
            if (vis[ni]) continue;
            if ((nx != tx || ny != ty) && !lv->passable(nx, ny)) continue;
            vis[ni] = 1; par_x[ni] = x; par_y[ni] = y; q[tail++] = ni;
        }
    }
    return vis[tx + ty * DG_MAP_W] != 0;
}

/* ===== M6 取证：全程通关链路（串口 'u'）=====
 * 用「超配英雄」（STR=100 一击杀 / HP=200 抗 traps 与首领 / 钥匙 9）把战斗
 * 随机性从失败路径剔除，但走的全是真实游戏链路：hero_try_step（战斗/拾取/
 * 开门/陷阱/宝箱）→ descend_stairs（12 层重建+投放）→ 12F 捡护身符 → 拾取
 * 即 WIN。每层打 HP/EXP/LV/GOLD/KEYS 曲线供平衡复查；多 seed 压力验证任意局
 * 都能走通。注：本自检不是平衡实验（超配会抹平难度），平衡靠逐层曲线数据
 * 与真机 auto 冒烟交叉印证。 */
void Game::debug_m6_fullrun() {
    if (scene != DG_SCENE_IN_GAME) new_game(DG_CLASS_WARRIOR, 20261006u);
    if (!hero || !level) { ESP_LOGE("dg.m6", "M6 no-hero/level"); return; }
    const uint32_t s = seed;
    static uint8_t m6_vis[DG_MAP_W * DG_MAP_H];
    static int m6_px[DG_MAP_W * DG_MAP_H], m6_py[DG_MAP_W * DG_MAP_H];
    ESP_LOGI("dg.m6", "M6 FULLRUN BEGIN seed=%u", (unsigned)s);

    int wins = 0;
    const int seeds = 8;
    for (int k = 0; k < seeds; k++) {
        uint32_t sd = s ^ (uint32_t)(k * 0x9E3779B1u + 0x5555u);
        new_game(DG_CLASS_WARRIOR, sd);
        depth = 0; level->depth = 0;
        level->at(hero->x, hero->y).actor = hero;
        hero->str = 100; hero->hp_max = 200; hero->hp = 200; hero->keys = 9;
        int killed = 0;
        bool ok = true;
        for (int d = 0; d < DG_MAX_DEPTH && ok && scene == DG_SCENE_IN_GAME; d++) {
            /* 清场：本自检验的是 descend→护身符→WIN 链路完整性，不是战斗；
             * 先把除英雄外的演员（怪）全部回池，免得怪走上路径把导航拖成 livelock */
            for (int i = level->actor_count - 1; i >= 1; i--) {
                Actor* a = level->actors[i];
                if (!a) continue;
                if (level->at(a->x, a->y).actor == a) level->at(a->x, a->y).actor = nullptr;
                level->del_actor(a);
                free_mob(static_cast<Mob*>(a));
            }
            int tx = (int)(d < DG_MAX_DEPTH - 1 ? level->exit_pos : level->amulet_pos) % DG_MAP_W;
            int ty = (int)(d < DG_MAX_DEPTH - 1 ? level->exit_pos : level->amulet_pos) / DG_MAP_W;
            /* BFS 父指针：英雄→目标（出口/护身符格特殊放行，同 M5 口径）*/
            if (!m6_flood_bfs(level, hero->x, hero->y, tx, ty, m6_vis, m6_px, m6_py)) {
                ok = false;
                ESP_LOGE("dg.m6", "M6 seed%d floor%d 目标不可达 FAIL", k, d + 1);
                break;
            }
            /* 路径回放：先把父指针从目标回溯成有序路径，再正向逐步
             * hero_try_step（真实落子：战斗/拾取/开门/陷阱全走游戏逻辑）；
             * 被怪推离路线就重算 BFS，最多三次 */
            static int path_x[DG_MAP_W * DG_MAP_H], path_y[DG_MAP_W * DG_MAP_H];
            int plen = 0;
            int guard = DG_MAP_W * DG_MAP_H * 4;
            bool arrived = false;
            for (int rebuild = 0; rebuild < 3 && !arrived; rebuild++) {
                if (!m6_flood_bfs(level, hero->x, hero->y, tx, ty, m6_vis, m6_px, m6_py)) break;
                plen = 0;
                for (int cx = tx, cy = ty; !(cx == hero->x && cy == hero->y) && plen < DG_MAP_W * DG_MAP_H; ) {
                    path_x[plen] = cx; path_y[plen] = cy; plen++;
                    int p = m6_px[cx + cy * DG_MAP_W], q2 = m6_py[cx + cy * DG_MAP_W];
                    cx = p; cy = q2;
                }
                int pi = plen - 1;               /* 从靠近起点的端点开始走 */
                while (pi >= 0 && guard-- > 0) {
                    int gx = path_x[pi], gy = path_y[pi];
                    Tile& nt = level->at(gx, gy);
                    /* 死户残留：Mob::die 已 free_mob 并回池，hero_try_step 不管清位；
                     * 这里只把悬空的 tile.actor 标位清摸，实体回收交给 advance_mobs 收割 */
                    if (nt.actor && nt.actor != hero && !nt.actor->is_alive()) nt.actor = nullptr;
                    if (hero->x == gx && hero->y == gy) { pi--; continue; }   /* 已到位（宝箱原地开等）→下一个路点 */
                    /* 统一走 hero_try_step：前方有敌就砍，无敌就移动（不自己 attack/free，避免二次释放）*/
                    bool moved = hero_try_step(gx, gy);
                    if (moved && nt.actor && nt.actor != hero && !nt.actor->is_alive()) nt.actor = nullptr;
                    if (hero->x == gx && hero->y == gy) pi--;                 /* 真站上去才算达成路点 */
                    advance_mobs();
                    if (!moved && !(nt.actor && nt.actor != hero)) guard--;   /* 非战斗却走不通：消耗预算防死循环 */
                }
                if (hero->x == tx && hero->y == ty) { arrived = true; break; }
            }
            if (!arrived) {
                ok = false;
                ESP_LOGE("dg.m6", "M6 seed%d floor%d 未走到目标(h=%d,%d→%d,%d) FAIL",
                         k, d + 1, hero->x, hero->y, tx, ty);
                break;
            }
            ESP_LOGI("dg.m6", "M6 seed%d floor%d hp=%d/%d exp=%d lv=%d gold=%d keys=%d killed=%d %s",
                     k, d + 1, hero->hp, hero->hp_max, hero->exp, hero->lvl,
                     hero->gold, hero->keys, killed, scene == DG_SCENE_WIN ? "WIN" : "OK");
            if (d < DG_MAX_DEPTH - 1) {
                level->at(hero->x, hero->y).actor = nullptr;   /* descend 前清占位，防新层怪指向旧指针 */
                if (!descend_stairs()) { ok = false;
                    ESP_LOGE("dg.m6", "M6 seed%d floor%d 下楼失败 FAIL", k, d + 1); break; }
                level->at(hero->x, hero->y).actor = hero;
            }
        }
        bool win = hero->has_amulet && scene == DG_SCENE_WIN;
        if (win) wins++;
        ESP_LOGI("dg.m6", "M6 seed%d RESULT amulet=%d scene_win=%d %s",
                 k, hero->has_amulet ? 1 : 0, scene == DG_SCENE_WIN ? 1 : 0,
                 (ok && win) ? "PASS" : "FAIL");
    }
    /* 收尾：回主 seed 干净开局 */
    new_game(DG_CLASS_WARRIOR, s);
    ESP_LOGI("dg.m6", "M6 SUMMARY wins=%d/%d %s", wins, seeds, wins == seeds ? "ALL PASS" : "CHECK ABOVE");
}

}  /* namespace dg */
