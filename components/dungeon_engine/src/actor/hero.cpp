/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hero.cpp —— 玩家角色（骨架）
 */
#include "dg_types.h"
#include "rng/java_random.h"      /* Game::ui_rng->nextInt 需完整类型 */
#include "esp_log.h"
#include <cstdio>

static const char *TAG = "dg.hero";

namespace dg {

int Hero::act() {
    /* 玩家 act 由输入驱动，返回 -1 表示等待 */
    return -1;
}

int Hero::attack(Actor* enemy) {
    if (!enemy) return 0;
    JavaRandom& r = *Game::instance().ui_rng;
    /* Shattered 手感简化版（v0.3）：命中 = roll(attack_skill) vs 对方防御技；
     * 伤害 = 1..str/2（力量驱动，升级 +str 即可感变强） */
    if (r.nextInt(attack_skill + 10) < static_cast<Mob*>(enemy)->defenseSkill(this)) {
        Game::instance().log("你砍空了。");
        return 1;
    }
    int dmg = 1 + r.nextInt(str / 2 + 1);
    enemy->damage(dmg, "hit");
    if (!enemy->is_alive()) {
        int xp = static_cast<Mob*>(enemy)->xp_in_kill;
        exp += xp;
        Game::instance().log("你解决了它。获得经验。");
        /* 升级门槛：5 + lvl*5（原版 maxExp 简化），升级回 5 血 + 成长 */
        while (exp >= 5 + lvl * 5) {
            exp -= 5 + lvl * 5;
            lvl++;
            hp_max += 5; hp += 5;
            str += 1; attack_skill += 2; defense_skill += 1;
            static char s_lvl_msgs[8][80];
            snprintf(s_lvl_msgs[lvl % 8], sizeof(s_lvl_msgs[0]),
                     "你升到了 %d 级！力量 %d，生命 %d。", lvl, str, hp_max);
            Game::instance().log(s_lvl_msgs[lvl % 8]);
        }
    } else {
        Game::instance().log("你砍中了敌人。");
    }
    return 1;
}

int Hero::defense(Mob* enemy) {
    (void)enemy;
    return defense_skill;
}

bool Hero::pickup(Item* it) {
    if (!it || inv_count >= DG_MAX_INVENTORY) return false;
    inventory[inv_count++] = it;
    Game::instance().log("picked_up");
    return true;
}

bool Hero::equip(Item* it) {
    if (!it) return false;
    if (it->kind == Item::K_WEAPON) equipped_weapon = it;
    else if (it->kind == Item::K_ARMOR) equipped_armor = it;
    else return false;
    it->on_equip(this);
    return true;
}

bool Hero::use(int slot) {
    if (slot < 0 || slot >= inv_count) return false;
    inventory[slot]->on_use(this);
    return true;
}

void Actor::damage(int dmg, const char* src) {
    (void)src;
    hp -= dmg;
    if (hp <= 0) die();
}

void Actor::die() {
    if (level) {
        /* 抹掉格子占位（存活体的 tile->actor 不变式） */
        if (x >= 0 && x < DG_MAP_W && y >= 0 && y < DG_MAP_H) {
            Tile& t = level->at(x, y);
            if (t.actor == this) t.actor = nullptr;
        }
        for (int i = 0; i < level->actor_count; i++) {
            if (level->actors[i] == this) {
                level->actors[i] = level->actors[level->actor_count - 1];
                level->actors[--level->actor_count] = nullptr;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "%s died", name_key ? name_key : "(unnamed)");
}

void Actor::add_buff(Buff* b) {
    b->next = first_buff;
    b->owner = this;
    first_buff = b;
    b->attach();
}

void Actor::remove_buff(Buff::Type t) {
    Buff** pp = &first_buff;
    while (*pp) {
        if ((*pp)->type == t) {
            Buff* gone = *pp;
            *pp = gone->next;
            gone->detach();
            delete gone;
            return;
        }
        pp = &(*pp)->next;
    }
}

Buff* Actor::get_buff(Buff::Type t) {
    for (Buff* b = first_buff; b; b = b->next) if (b->type == t) return b;
    return nullptr;
}

}  /* namespace dg */
