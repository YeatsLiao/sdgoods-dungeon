/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mob.cpp —— 怪物基类（骨架）
 */
#include "dg_types.h"
#include "rng/java_random.h"

namespace dg {

int Mob::rand_int(int bound) {
    if (bound <= 0) return 0;
    return Game::instance().ui_rng->nextInt(bound);
}

int Mob::attackSkill(Actor* target) {
    (void)target;
    return 20 + (level ? level->depth * 5 : 0);
}

int Mob::defenseSkill(Actor* target) {
    (void)target;
    return defense;
}

bool Mob::surprised_by(Actor* target) {
    if (!target) return false;
    /* 骨架：SLEEPING 且玩家在身后 */
    return state == SLEEPING && (target->x != x || target->y != y);
}

int Mob::act() {
    /* 状态机：SLEEPING / WANDERING / HUNTING / FLEEING / PASSIVE
     * 骨架版：HUNTING 状态下简单朝玩家走一步，其他状态不动 */
    if (state != HUNTING) return 1;
    Hero* h = Game::instance().hero;
    if (!h) return 1;

    int dx = h->x - x, dy = h->y - y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx <= 1 && ady <= 1) {
        /* 相邻 → 攻击 */
        h->damage(attack_min + rand_int(attack_max - attack_min + 1), name_key);
        return 1;
    }
    /* 单步贪心 */
    int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    if (level->passable(x + sx, y + sy)) { x += sx; y += sy; }
    else if (level->passable(x + sx, y))  { x += sx; }
    else if (level->passable(x, y + sy))  { y += sy; }
    return 1;
}

}  /* namespace dg */
