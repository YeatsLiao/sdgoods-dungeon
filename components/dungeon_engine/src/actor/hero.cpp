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

static const char *TAG = "dg.hero";

namespace dg {

int Hero::act() {
    /* 玩家 act 由输入驱动，返回 -1 表示等待 */
    return -1;
}

int Hero::attack(Actor* enemy) {
    if (!enemy) return 0;
    /* Shattered 手感公式骨架版：命中 = 简单 roll，正式 v0.1 会照搬
       Hero.attackSkill(enemy) vs enemy.defenseSkill(this) + 命中判定 */
    int dmg = Game::instance().ui_rng->nextInt(4) + 2;
    enemy->damage(dmg, "hit");
    Game::instance().log("you_hit");
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
