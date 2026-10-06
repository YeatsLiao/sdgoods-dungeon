/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mob.cpp —— 怪物基类（v0.2：唤醒/游荡/追踪状态机 + 占位登记）
 */
#include "dg_types.h"
#include "rng/java_random.h"
#include "fov/fov.h"

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
    Hero* h = Game::instance().hero;
    if (!h || !level) return 1;

    /* 唤醒判定：英雄格在玩家视野内（vis_current）且视线直达 */
    int dist = level->distance(x, y, h->x, h->y);
    bool seen = level->at(h->x, h->y).vis_current &&
                dist <= see_range &&
                fov::visible_between(level, x, y, h->x, h->y);
    if (state == SLEEPING && seen) {
        state = HUNTING;
    } else if (state == HUNTING && !seen && dist > see_range + 2) {
        state = WANDERING;
    }

    if (state == WANDERING && rand_int(4) == 0) {
        /* 1/4 概率随机游走一格 */
        static const int dx4[4] = {-1, 1, 0, 0};
        static const int dy4[4] = {0, 0, -1, 1};
        int d = rand_int(4);
        int nx = x + dx4[d], ny = y + dy4[d];
        Tile& t = level->at(x, y);
        if (level->passable(nx, ny) && !level->at(nx, ny).actor) {
            t.actor = nullptr;
            x = nx; y = ny;
            level->at(x, y).actor = this;
        }
    }

    if (state != HUNTING) return 1;

    int dx = h->x - x, dy = h->y - y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx <= 1 && ady <= 1) {
        /* 相邻 → 攻击 */
        h->damage(attack_min + rand_int(attack_max - attack_min + 1), name_key);
        return 1;
    }
    /* 单步贪心（含占位登记，v0.1 换 A* 追击） */
    int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
    Tile& cur = level->at(x, y);
    int nx = x, ny = y;
    if (level->passable(x + sx, y + sy) && !level->at(x + sx, y + sy).actor) { nx += sx; ny += sy; }
    else if (level->passable(x + sx, y) && !level->at(x + sx, y).actor)       { nx += sx; }
    else if (level->passable(x, y + sy) && !level->at(x, y + sy).actor)       { ny += sy; }
    if (nx != x || ny != y) {
        if (cur.actor == this) cur.actor = nullptr;
        x = nx; y = ny;
        level->at(x, y).actor = this;
    }
    return 1;
}

}  /* namespace dg */
