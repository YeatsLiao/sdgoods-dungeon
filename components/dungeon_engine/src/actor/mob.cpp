/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mob.cpp —— 怪物 AI（物种表驱动：唤醒 / 游荡 / 追击 / 远程 / 召唤 / 偷窃 / 分裂）
 *
 * 上游是「一怪一个类」，本移植把差异压进 MobSpec.flags：行为位短路判定，
 * 新增怪物只改表不改代码。移动全部登记 from_x/from_y + move_anim，渲染层
 * 据此做亚 tile 插值，玩家看到的是「滑过来」而不是「跳过来」。
 */
#include "dg_types.h"
#include "actor/mob_spec.h"
#include "item/item_def.h"
#include "gfx/gfx.h"
#include "fov/fov.h"
#include "rng/java_random.h"
#include "esp_log.h"
#include <cstdio>

static const char *TAG = "dg.mob";

namespace dg {

int Mob::rand_int(int bound)
{
    if (bound <= 0) return 0;
    return Game::instance().ui_rng->nextInt(bound);
}

int Mob::damageRoll()
{
    /* 上游 Mob.damageRoll 基本都走 Random.NormalIntRange(min,max) 三角分布 */
    return rndNormalRange(Game::instance().ui_rng, attack_min, attack_max);
}

int Mob::attackSkill(Actor* target)
{
    (void)target;
    int acc = spec ? spec->acc : 20;
    /* 越深的普通怪随层微涨（首领不吃这条，它们自带数值）*/
    if (level && !(spec && (spec->flags & MF_BOSS))) acc += level->depth;
    /* 野兽人狂暴（上游 Rabid Brute）：血量低于一半时命中大涨 */
    if (spec && spec->sheet == (uint8_t)gfx::SH_MOB_BRUTE && hp * 2 < hp_max) acc += 10;
    return acc;
}

int Mob::defenseSkill(Actor* target)
{
    (void)target;
    return defense;
}

bool Mob::surprised_by(Actor* target)
{
    if (!target) return false;
    return state == SLEEPING && (target->x != x || target->y != y);
}

/* 朝 (hx,hy) 贪心迈一步（8 向）。返回是否真的动了。 */
static bool step_toward(Mob* m, int hx, int hy)
{
    Level* lv = m->level;
    int dx = hx - m->x, dy = hy - m->y;
    int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    struct Candidate { int x, y; };
    Candidate cand[3] = { { m->x + sx, m->y + sy }, { m->x + sx, m->y }, { m->x, m->y + sy } };
    for (int i = 0; i < 3; i++) {
        int nx = cand[i].x, ny = cand[i].y;
        if (nx == m->x && ny == m->y) continue;
        if (!lv->passable(nx, ny)) continue;
        if (lv->at(nx, ny).actor) continue;
        lv->at(m->x, m->y).actor = nullptr;
        m->from_x = m->x; m->from_y = m->y;
        m->move_anim = 0;
        m->set_pos(nx, ny);
        lv->at(nx, ny).actor = m;
        return true;
    }
    return false;
}

static bool step_away(Mob* m, int hx, int hy)
{
    Level* lv = m->level;
    int dx = m->x - hx, dy = m->y - hy;
    int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    int ox[3] = { m->x + sx, m->x + sx, m->x };
    int oy[3] = { m->y + sy, m->y, m->y + sy };
    for (int i = 0; i < 3; i++) {
        if (!lv->passable(ox[i], oy[i]) || lv->at(ox[i], oy[i]).actor) continue;
        lv->at(m->x, m->y).actor = nullptr;
        m->from_x = m->x; m->from_y = m->y;
        m->move_anim = 0;
        m->set_pos(ox[i], oy[i]);
        lv->at(ox[i], oy[i]).actor = m;
        return true;
    }
    return false;
}

/* 远程一击：光束 + 直接伤害（不经过相邻判定） */
static void ranged_attack(Mob* m, Hero* h)
{
    Game& g = Game::instance();
    int ak = m->attackSkill(h);
    int df = h->defenseSkill();
    uint16_t beam_color = 0x0710;                      /* 青蓝：法术 */
    if (m->spec && m->spec->sheet == (uint8_t)gfx::SH_MOB_GNOLL) beam_color = 0x8200;  /* 棕：投矛 */
    g.add_beam(m->x, m->y, h->x, h->y, beam_color);
    /* 上游远程命中同样 acuRoll vs defRoll（magic 只放大 acu 权重，此处简化为直 roll） */
    if (rollHit(g.ui_rng, ak, df)) {
        h->damage(m->damageRoll(), m->name_key);
        /* 蜘蛛（Spinner）：远程命中吐丝缠腿 → 英雄定身 ROOTS */
        if (m->spec && m->spec->sheet == (uint8_t)gfx::SH_MOB_SPINNER) {
            h->add_buff(Buff::ROOTS, 3);
            g.log("蜘蛛吐丝缠住了你的腿！");
        }
    } else {
        g.add_float(h->x, h->y, "MISS", 0xAD55);
    }
    g.sfx(DG_SFX_ZAP);
    g.anim_running = true;
}

/* 召唤（首领专属）：在附近空格拉一只同层的杂兵 */
static void summon_minion(Mob* m, JavaRandom* r)
{
    Game& g = Game::instance();
    Level* lv = m->level;
    const MobSpec* s = mob_pick_for_depth(r, lv->depth);
    if (!s) return;
    static const int dx8[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
    static const int dy8[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
    int k = r->nextInt(8);
    for (int i = 0; i < 8; i++) {
        int nx = m->x + dx8[(k + i) & 7], ny = m->y + dy8[(k + i) & 7];
        if (!lv->passable(nx, ny) || lv->at(nx, ny).actor) continue;
        Mob* minion = g.alloc_mob();
        if (!minion) return;
        *minion = *m;                        /* 先借字段，再按杂兵物种覆盖 */
        minion->spec = s;
        minion->sheet = s->sheet;
        minion->hp_max = s->hp; minion->hp = s->hp;
        minion->attack_min = s->atk_min; minion->attack_max = s->atk_max;
        minion->defense = s->def; minion->xp_in_kill = s->xp;
        minion->see_range = s->see;
        minion->alignment = 1;
        minion->name_key = s->name;
        minion->state = Mob::HUNTING;
        minion->anim_seed = (uint8_t)r->nextInt(256);
        minion->flash_ticks = 0;
        minion->move_anim = 255;
        minion->first_buff = nullptr;
        minion->flags = 0;
        minion->level = lv;
        minion->from_x = minion->x = nx;
        minion->from_y = minion->y = ny;
        minion->home_x = nx; minion->home_y = ny;
        lv->at(nx, ny).actor = minion;
        lv->add_actor(minion);
        g.log("它叫来了援兵！");
        g.sfx(DG_SFX_ZAP);
        return;
    }
}

/* 瞬间移动（萨满专属）：受击后闪到半径 4 内的可通行空格，拉开身位 */
static void teleport_near(Mob* m, JavaRandom* r)
{
    Game& g = Game::instance();
    Level* lv = m->level;
    for (int attempt = 0; attempt < 16; attempt++) {
        int nx = m->x + r->nextInt(9) - 4;
        int ny = m->y + r->nextInt(9) - 4;
        if (nx == m->x && ny == m->y) continue;
        if (nx < 0 || ny < 0 || nx >= DG_MAP_W || ny >= DG_MAP_H) continue;
        if (!lv->passable(nx, ny) || lv->at(nx, ny).actor) continue;
        g.add_beam(m->x, m->y, nx, ny, 0x8FE3);       /* 紫白：瞬移闪光 */
        lv->at(m->x, m->y).actor = nullptr;
        m->from_x = nx; m->from_y = ny; m->move_anim = 255;  /* 瞬移不做插值 */
        m->set_pos(nx, ny);
        lv->at(nx, ny).actor = m;
        m->home_x = nx; m->home_y = ny;               /* 新家，别立刻走回去 */
        g.log("萨满化作一团紫光，闪到别处。");
        g.sfx(DG_SFX_ZAP);
        return;
    }
}

int Mob::act()
{
    Game& g = Game::instance();
    Hero* h = g.hero;
    Level* lv = level;
    if (!h || !lv) return 1;

    act_buffs();
    if (!is_alive()) return 1;
    if (has_buff(Buff::PARALYSIS)) return 1;
    if (has_buff(Buff::SLEEP)) return 1;           /* 沉睡：受击才醒（见 damage） */
    const bool rooted = has_buff(Buff::ROOTS);     /* 定身：不能移动，但贴身仍可攻击 */

    /* 腐蚀之胶（Goo）：脱离受击 10 回合后自我愈合，逼玩家持续输出 */
    if (spec && spec->sheet == (uint8_t)gfx::SH_MOB_GOO && hp < hp_max &&
        (int)(g.game_time - last_hurt_time) >= 10) {
        int heal = hp_max / 10;
        if (heal < 1) heal = 1;
        hp += heal;
        if (hp > hp_max) hp = hp_max;
        g.add_float(x, y, "+", 0x7FE0);
        g.log("腐蚀之胶正在愈合——别停下攻击！");
    }

    int dist = lv->distance(x, y, h->x, h->y);
    /* 视线：英雄能看见这只怪 ⇔ 怪也能看见英雄（同一张 FOV 位图，天然对称） */
    bool los = lv->at(x, y).vis_current;
    if (h->has_buff(Buff::INVISIBILITY) && !lv->at(x, y).vis_magical) los = false;
    if (spec && (spec->flags & MF_STEALTHY)) los = los && dist <= 3;

    if (state == SLEEPING) {
        if (spec && (spec->flags & MF_AGGRO) && dist <= 4) {
            state = Mob::HUNTING;
        } else if (los && dist <= see_range) {
            state = Mob::HUNTING;
            g.log("它醒了。");
        } else {
            return 1;
        }
    }

    if (has_buff(Buff::FRIGHT)) {
        if (!rooted) step_away(this, h->x, h->y);
        return 1;
    }

    /* 首领：受召后每 10 回合叫一次援兵（冷却式，不再绑全局时间取模）*/
    if (spec && (spec->flags & MF_SUMMONER) &&
        (int)(g.game_time - last_summon_time) >= 10) {
        summon_minion(this, g.ui_rng);
        last_summon_time = g.game_time;
    }

    if (state == Mob::WANDERING) {
        if (los && dist <= see_range) { state = Mob::HUNTING; }
        else if (!rooted && rand_int(4) == 0) {
            /* 在home 4 格内随机飘，别把怪拉穿全图 */
            int nx = x + rand_int(3) - 1;
            int ny = y + rand_int(3) - 1;
            if (lv->passable(nx, ny) && !lv->at(nx, ny).actor &&
                lv->distance(nx, ny, home_x, home_y) <= 5) {
                lv->at(x, y).actor = nullptr;
                from_x = x; from_y = y; move_anim = 0;
                set_pos(nx, ny);
                lv->at(nx, ny).actor = this;
            }
        }
        return 1;
    }

    if (state != Mob::HUNTING) {
        /* 远离 home 太远的游荡怪慢慢走回去，避免全场怪堆在英雄脚边 */
        if (lv->distance(x, y, home_x, home_y) > 10) { if (!rooted) step_toward(this, home_x, home_y); }
        return 1;
    }

    if (!los && dist > see_range + 3) {
        state = Mob::WANDERING;
        return 1;
    }

    /* 远程优先：视距内、有视线、3~6 格之间开火 */
    if (spec && (spec->flags & MF_RANGED) && los && dist >= 3 && dist <= 6 &&
        rand_int(100) < 55) {
        ranged_attack(this, h);
        return 1;
    }

    if (dist <= 1) {
        int ak = attackSkill(h);
        int df = h->defenseSkill();
        if (rollHit(g.ui_rng, ak, df)) {
            h->damage(damageRoll(), name_key);
            if (spec && (spec->flags & MF_THIEF) && h->gold > 0 && rand_int(100) < 30) {
                int steal = 5 + rand_int(h->gold / 10 + 1);
                if (steal > h->gold) steal = h->gold;
                h->gold -= steal;
                char buf[40];
                snprintf(buf, sizeof(buf), "它摸走了你 %d 金币！", steal);
                g.log(buf);
                state = Mob::FLEEING;
            }
            if (spec && spec->sheet == (uint8_t)gfx::SH_MOB_SNAKE && rand_int(100) < 35) {
                h->add_buff(Buff::POISON, 6);
                g.log("毒液顺着伤口爬了上来。");
            }
        } else {
            g.add_float(h->x, h->y, "MISS", 0xAD55);
        }
        return 1;
    }

    if (state == Mob::FLEEING) {
        if (rooted) return 1;
        if (!step_away(this, h->x, h->y)) state = Mob::HUNTING;
        return 1;
    }

    if (!rooted) step_toward(this, h->x, h->y);
    return 1;
}

void Mob::damage(int dmg, const char* src)
{
    Game& g = Game::instance();
    /* 受击惊醒：清 SLEEP buff 并转入追击（SLEEPING 状态在 act() 里自然处理）*/
    if (has_buff(Buff::SLEEP)) remove_buff(Buff::SLEEP);
    /* 记录受击时刻（Goo 愈合判定依赖），并让萨满概率瞬移拉开身位 */
    last_hurt_time = g.game_time;
    if (spec && spec->sheet == (uint8_t)gfx::SH_MOB_SHAMAN && hp - dmg > 0 &&
        g.ui_rng->nextInt(100) < 40) {
        teleport_near(this, g.ui_rng);
    }
    /* 分裂怪：掉血后概率在相邻格吐出一只小号（上游 Slime / Swarm 手感） */
    if (spec && (spec->flags & MF_SPLITTER) && hp - dmg >= 2 && g.ui_rng->nextInt(100) < 30) {
        Level* lv = level;
        static const int dx8[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
        static const int dy8[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
        for (int i = 0; i < 8; i++) {
            int nx = x + dx8[i], ny = y + dy8[i];
            if (!lv->passable(nx, ny) || lv->at(nx, ny).actor) continue;
            Mob* child = g.alloc_mob();
            if (!child) break;
            *child = *this;
            child->hp = child->hp_max = (hp > 4) ? 4 : 2;
            child->attack_min = 1; child->attack_max = 3;
            child->xp_in_kill = 1;
            child->state = Mob::HUNTING;
            child->first_buff = nullptr;
            child->move_anim = 255;
            child->flash_ticks = 0;
            child->anim_seed = (uint8_t)(g.ui_rng->nextInt(256));
            child->from_x = child->x = nx;
            child->from_y = child->y = ny;
            child->home_x = nx; child->home_y = ny;
            lv->at(nx, ny).actor = child;
            lv->add_actor(child);
            g.log("它裂开了！");
            break;
        }
    }
    Actor::damage(dmg, src);
}

void Mob::die()
{
    Game& g = Game::instance();
    const MobSpec* s = spec;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s倒下了。", s ? s->name : "它");
    g.log(buf);
    if (s && (s->flags & MF_BOSS)) {
        g.sfx(DG_SFX_WIN);
        g.log("首领被击倒！地上滚出一件好东西。");
    }
    /* 掉落：金币必掉（按 loot 等级放大），装备按概率 */
    int q = s ? s->loot : 0;
    int gold = 0;
    if (q >= 3) gold = 60 + g.ui_rng->nextInt(120);
    else if (q == 2) gold = 20 + g.ui_rng->nextInt(40);
    else if (q == 1) gold = 5 + g.ui_rng->nextInt(20);
    if (gold > 0) {
        Item* it = g.alloc_item();
        if (it) {
            fill_item(it, Item::K_GOLD, 0, 0);
            it->qty = gold;
            it->x = x; it->y = y;
            it->on_drop(level);
        }
    }
    int chance = (q >= 3) ? 100 : (q == 2 ? 45 : (q == 1 ? 18 : 6));
    if (g.ui_rng->nextInt(100) < chance) {
        Item* drop = roll_drop(q >= 3 ? 3 : q, level ? level->depth : 0, g.ui_rng);
        if (drop) {
            drop->x = x; drop->y = y;
            drop->on_drop(level);
        }
    }
    Actor::die();
    g.free_mob(this);
}

/* ===== M3 取证：怪物专属 AI + 首领机制自检（串口 'k' 触发，直接打机读日志）=====
 * 不依赖串口导航（三个首领层逐个走位太脆），而是直接把相关怪物拉进
 * 可复现语境里跑关键分支，逐项 PASS/FAIL 走 ESP_LOGI，play_test 从日志里抢。 */
static Mob* m3_find_by_sheet(int sheet)
{
    Game& g = Game::instance();
    Level* lv = g.level;
    if (!lv) return nullptr;
    for (int i = 1; i < lv->actor_count; i++) {          /* [0] 永远是英雄 */
        Mob* m = static_cast<Mob*>(lv->actors[i]);
        if (m && m->is_alive() && m->sheet == sheet) return m;
    }
    return nullptr;
}

void Game::debug_m3_selftest()
{
    Game& g = *this;
    if (scene != DG_SCENE_IN_GAME) g.new_game(DG_CLASS_WARRIOR, 20261006u);
    if (!hero || !level) { ESP_LOGE("dg.m3", "M3 SELFTEST no-hero/level"); return; }

    /* 探活期间让英雄不阵亡，免得 probe 中途 die() 改场景 */
    hero->hp_max = 100000; hero->hp = 100000;
    ESP_LOGI("dg.m3", "M3 SELFTEST BEGIN");

    auto warp = [&](int d0) {
        depth = d0; level->depth = d0;
        level->generate(seed, d0);
        hero->first_buff = nullptr;
        hero->set_pos(level->entrance_pos % DG_MAP_W, level->entrance_pos / DG_MAP_W);
        hero->from_x = hero->x; hero->from_y = hero->y; hero->move_anim = 255;
        path_len = path_head = 0;
        g.spawn_level_content();
        recalc_fov();
    };
    auto bring_next_to = [&](Mob* m) -> bool {
        static const int dx8[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
        static const int dy8[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
        if (level->at(m->x, m->y).actor == m) level->at(m->x, m->y).actor = nullptr;
        for (int i = 0; i < 8; i++) {
            int nx = hero->x + dx8[i], ny = hero->y + dy8[i];
            if (!level->passable(nx, ny) || level->at(nx, ny).actor) continue;
            m->from_x = m->x = nx; m->from_y = m->y = ny; m->move_anim = 255;
            m->home_x = nx; m->home_y = ny;
            level->at(nx, ny).actor = m;
            return true;
        }
        return false;
    };

    /* ---- 1. Goo 愈合：脱受击 ≥10 回合→回血；刚被打→不回 ---- */
    warp(3); game_time = 500;                              /* 4F */
    Mob* goo = m3_find_by_sheet((int)gfx::SH_MOB_GOO);
    if (goo) {
        bring_next_to(goo);
        goo->state = Mob::HUNTING;
        goo->hp = goo->hp_max - 25;
        goo->last_hurt_time = game_time - 20;
        int before = goo->hp; goo->act(); int after = goo->hp;
        ESP_LOGI("dg.m3", "M3 GOO_HEAL %d->%d %s", before, after, after > before ? "PASS" : "FAIL");
        goo->hp = goo->hp_max - 25;
        goo->last_hurt_time = game_time;                   /* 刚被打 */
        int b2 = goo->hp; goo->act(); int a2 = goo->hp;
        ESP_LOGI("dg.m3", "M3 GOO_NOHEAL %d->%d %s", b2, a2, a2 == b2 ? "PASS" : "FAIL");
    } else ESP_LOGE("dg.m3", "M3 GOO no-boss FAIL");

    /* ---- 2. 首领召唤（矮人之王 12F，MF_SUMMONER）：冷却到→叫援兵；刚叫过→冷却中 ---- */
    warp(11); game_time = 800;                             /* 12F */
    Mob* king = m3_find_by_sheet((int)gfx::SH_MOB_KING);
    if (king) {
        bring_next_to(king);
        king->state = Mob::HUNTING;
        king->last_summon_time = game_time - 30;
        int before = level->actor_count; king->act(); int after = level->actor_count;
        ESP_LOGI("dg.m3", "M3 SUMMON %d->%d %s", before, after, after > before ? "PASS" : "FAIL");
        int b2 = level->actor_count; king->act(); int a2 = level->actor_count;
        ESP_LOGI("dg.m3", "M3 SUMMON_COOLDOWN %d->%d %s", b2, a2, a2 == b2 ? "PASS" : "FAIL");
    } else ESP_LOGE("dg.m3", "M3 KING no-boss FAIL");

    /* ---- 3. 野兽人狂暴：半血以下命中 +10 ---- */
    {
        Mob* b = g.alloc_mob();
        if (b) {
            const MobSpec* s = &MOB_SPECS[MOB_BRUTE];
            b->spec = s; b->sheet = s->sheet; b->name_key = s->name;
            b->hp_max = s->hp; b->hp = s->hp; b->level = level; b->alignment = 1;
            int acc_full = b->attackSkill(hero);
            b->hp = s->hp / 2 - 1;
            int acc_rage = b->attackSkill(hero);
            ESP_LOGI("dg.m3", "M3 BRUTE_RAGE %d->%d %s", acc_full, acc_rage, acc_rage == acc_full + 10 ? "PASS" : "FAIL");
            g.free_mob(b);
        }
    }

    /* ---- 4. 萨满受击瞬移：多次受击，位置变过即 PASS ---- */
    {
        Mob* sm = g.alloc_mob();
        if (sm) {
            const MobSpec* s = &MOB_SPECS[MOB_SHAMAN];
            sm->spec = s; sm->sheet = s->sheet; sm->name_key = s->name;
            sm->hp_max = s->hp; sm->hp = s->hp; sm->level = level; sm->alignment = 1;
            bring_next_to(sm);
            bool moved = false;
            for (int t = 0; t < 40 && !moved; t++) {
                int ox = sm->x, oy = sm->y;
                sm->hp = sm->hp_max;                        /* 保持存活，专测瞬移 */
                sm->damage(3, "probe");
                if (sm->x != ox || sm->y != oy) moved = true;
            }
            ESP_LOGI("dg.m3", "M3 SHAMAN_TELEPORT %s", moved ? "PASS" : "FAIL");
            if (level->at(sm->x, sm->y).actor == sm) level->at(sm->x, sm->y).actor = nullptr;
            g.free_mob(sm);
        }
    }

    /* ---- 5. 蜘蛛结网：远程命中上 ROOTS ---- */
    {
        Mob* sp = g.alloc_mob();
        if (sp) {
            const MobSpec* s = &MOB_SPECS[MOB_SPINNER];
            sp->spec = s; sp->sheet = s->sheet; sp->name_key = s->name;
            sp->hp_max = s->hp; sp->hp = s->hp; sp->level = level; sp->alignment = 1;
            bring_next_to(sp);
            hero->remove_buff(Buff::ROOTS);
            bool rooted = false;
            for (int t = 0; t < 40 && !rooted; t++) {
                ranged_attack(sp, hero);
                if (hero->has_buff(Buff::ROOTS)) rooted = true;
            }
            ESP_LOGI("dg.m3", "M3 SPINNER_ROOTS %s", rooted ? "PASS" : "FAIL");
            hero->remove_buff(Buff::ROOTS);
            if (level->at(sp->x, sp->y).actor == sp) level->at(sp->x, sp->y).actor = nullptr;
            g.free_mob(sp);
        }
    }

    /* 收尾：回一层干净开局，不破坏玩家现场 */
    warp(0); game_time = 1;
    hero->hp_max = 20; hero->hp = 20;
    ESP_LOGI("dg.m3", "M3 SELFTEST END");
}

}  /* namespace dg */
