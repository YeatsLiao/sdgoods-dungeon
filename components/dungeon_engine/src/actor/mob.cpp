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
    /* 越深的普通怪随层微涨（首领不吃这条，它们自带数值） */
    if (level && !(spec && (spec->flags & MF_BOSS))) acc += level->depth;
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

int Mob::act()
{
    Game& g = Game::instance();
    Hero* h = g.hero;
    Level* lv = level;
    if (!h || !lv) return 1;

    act_buffs();
    if (!is_alive()) return 1;
    if (has_buff(Buff::PARALYSIS)) return 1;

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
        step_away(this, h->x, h->y);
        return 1;
    }

    /* 首领：每 12 回合叫一次援兵 */
    if (spec && (spec->flags & MF_SUMMONER) && (g.game_time % 12) == 0) {
        summon_minion(this, g.ui_rng);
    }

    if (state == Mob::WANDERING) {
        if (los && dist <= see_range) { state = Mob::HUNTING; }
        else if (rand_int(4) == 0) {
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
        if (lv->distance(x, y, home_x, home_y) > 10) step_toward(this, home_x, home_y);
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
        if (!step_away(this, h->x, h->y)) state = Mob::HUNTING;
        return 1;
    }

    step_toward(this, h->x, h->y);
    return 1;
}

void Mob::damage(int dmg, const char* src)
{
    Game& g = Game::instance();
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

}  /* namespace dg */
