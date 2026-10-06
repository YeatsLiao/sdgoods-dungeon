/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hero.cpp —— 玩家：战斗公式、护甲减伤、饥饿、Buff 计时、背包与装备
 *
 * 数值口径（对齐上游 Shattered 的判定形状，常量做了手持平衡）：
 *   命中  chance = atkSkill / (atkSkill + defSkill)
 *   伤害  武器档位区间 + 力量盈余加成；赤手 = 1 + str/4
 *   减伤  护甲档位 [dr_min, dr_max] 随机；力量不足时上限砍半
 *   经验  maxExp = 5 + lvl*5；升级 +5 生命上限 / +1 力量 / +2 命中 / +1 防御
 *   饥饿  energy 上限 300，每回合 -1；不足 1/3 停止自然回血，归零开始掉血
 */
#include "dg_types.h"
#include "item/item_def.h"
#include "actor/mob_spec.h"
#include "rng/java_random.h"
#include "esp_log.h"
#include <cstdio>

static const char *TAG = "dg.hero";

/* 飘字配色（native RGB565，UI 侧统一 bswap） */
static const uint16_t C_HURT  = 0xF800;   /* 我受伤：红 */
static const uint16_t C_DEALT = 0xFFE0;   /* 打出去：黄 */
static const uint16_t C_MISS  = 0xAD55;   /* 落空：灰 */
static const uint16_t C_HEAL  = 0x07E0;   /* 治疗：绿 */
static const uint16_t C_LEVEL = 0x07FF;   /* 升级：青 */

namespace dg {

/* ===== Actor 基类公共实现 ===== */

void Actor::damage(int dmg, const char* src)
{
    (void)src;
    if (dmg < 0) dmg = 0;
    Game& g = Game::instance();
    hp -= dmg;
    if (hp < 0) hp = 0;
    if (dmg > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "-%d", dmg);
        bool is_hero = (this == g.hero);
        g.add_float(x, y, buf, is_hero ? C_HURT : C_DEALT);
        g.flash_actor(this);
        g.sfx(is_hero ? DG_SFX_HURT : DG_SFX_HIT);
        g.anim_running = true;
    }
    if (hp <= 0) die();
}

void Actor::die()
{
    if (level) {
        if (x >= 0 && x < DG_MAP_W && y >= 0 && y < DG_MAP_H) {
            Tile& t = level->at(x, y);
            if (t.actor == this) t.actor = nullptr;
        }
        level->del_actor(this);
    }
    /* 身上挂的 buff 全部退回池子 */
    while (first_buff) {
        Buff* b = first_buff;
        first_buff = b->next;
        Game::instance().free_buff(b);
    }
    ESP_LOGI(TAG, "%s died", name_key ? name_key : "(unnamed)");
}

Buff* Actor::get_buff(Buff::Type t) const
{
    for (Buff* b = first_buff; b; b = b->next) if (b->type == t) return b;
    return nullptr;
}

void Actor::add_buff(Buff::Type t, int duration, int value)
{
    Buff* old = get_buff(t);
    if (old) {                                  /* 同类叠加取更长剩余，不做多层计数 */
        if (duration > old->duration) old->duration = duration;
        if (value > old->value) old->value = value;
        return;
    }
    Buff* b = Game::instance().alloc_buff();
    if (!b) return;
    b->type = t;
    b->duration = duration;
    b->value = value;
    b->owner = this;
    b->next = first_buff;
    first_buff = b;
}

void Actor::remove_buff(Buff::Type t)
{
    Buff** pp = &first_buff;
    while (*pp) {
        if ((*pp)->type == t) {
            Buff* gone = *pp;
            *pp = gone->next;
            Game::instance().free_buff(gone);
            return;
        }
        pp = &(*pp)->next;
    }
}

int Actor::buff_turns(Buff::Type t)
{
    Buff* b = get_buff(t);
    return b ? b->duration : 0;
}

/* 回合制计时 + 周期伤害。返回 false 表示本回合被状态吃掉（麻痹/睡）。 */
void Actor::act_buffs()
{
    Game& g = Game::instance();
    Buff* b = first_buff;
    while (b) {
        Buff* nx = b->next;
        bool dead = false;
        switch (b->type) {
        case Buff::POISON:
        case Buff::BURNING:
            hp -= 1;
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "-1");
                g.add_float(x, y, buf, b->type == Buff::POISON ? 0x8B1C : 0xF900);
                g.anim_running = true;
            }
            dead = (hp <= 0);
            break;
        default:
            break;
        }
        if (dead) die();
        if (b->duration > 0) {
            b->duration--;
            if (b->duration == 0) { remove_buff(b->type); }
        }
        b = nx;
    }
}

/* ===== Hero ===== */

int Hero::act()
{
    return -1;                     /* 玩家由输入驱动 */
}

int Hero::armorDrMax() const
{
    return equipped_armor ? armor_dr_max(equipped_armor->tier) : 0;
}

int Hero::attackSkill() const
{
    int ak = attack_skill;
    if (equipped_ring && equipped_ring->sub == RG_ACCURACY) ak += 8;
    return ak;
}

int Hero::defenseSkill() const
{
    int df = defense_skill;
    if (equipped_ring && equipped_ring->sub == RG_EVASION) df += 8;
    if (has_buff(Buff::HASTE)) df += 4;
    /* 上游 Armor.evasionFactor：穿不动护甲时 evasion /= 1.5^(STRReq-STR)，
     * 末尾再 Math.max(1,…)。这才是「穿不动甲」真正拖后腿的地方。 */
    if (equipped_armor && str < equipped_armor->str_req) {
        int enc = equipped_armor->str_req - str;
        for (int i = 0; i < enc && df > 1; i++) df = (df * 2 + 1) / 3;   /* ≈ /1.5 取整 */
    }
    if (df < 1) df = 1;
    return df;
}

/* 命中判定：严格照搬上游 Char.hit() 的形状 ——
 *   acuRoll = rand[0, attackSkill)  vs  defRoll = rand[0, defenseSkill)
 *   acuRoll >= defRoll 即命中；隐身（偷袭）必中。
 * 不再是 atk/(atk+def) 比值式，那个会让高命中永远打不空、低命中打老鼠像刮痧。
 * 伤害照搬 MeleeWeapon.damageRoll：NormalIntRange(min,max) 三角分布 +
 * 力量盈余 exStr = range(0, STR-STRReq)（穿得动就越狠，穿不动加成为 0）。 */
int Hero::attack(Actor* enemy)
{
    if (!enemy) return 0;
    Game& g = Game::instance();
    Mob* m = (Mob*)enemy;

    int ak = attackSkill();
    /* 上游 Weapon.accuracyFactor：力量不足持 weapon 时 ACC /= 1.5^encumbrance（
     * 不是平减，而是指数惩罚——穿不动武器会越来越打不中）。 */
    if (equipped_weapon && str < equipped_weapon->str_req) {
        int enc = equipped_weapon->str_req - str;
        for (int i = 0; i < enc && ak > 1; i++) ak = (ak * 2 + 1) / 3;   /* ≈ /1.5 取整 */
    }
    if (ak < 1) ak = 1;

    int df = m->defenseSkill(this);

    /* 上游：隐身且能偷袭 = INFINITE_ACCURACY，必中 */
    bool sneak = has_buff(Buff::INVISIBILITY);
    if (!sneak && !rollHit(g.ui_rng, ak, df)) {
        g.add_float(enemy->x, enemy->y, "MISS", C_MISS);
        g.sfx(DG_SFX_MISS);
        g.anim_running = true;
        return 1;
    }

    int lo, hi;
    if (equipped_weapon) {
        lo = weapon_dmg_min(equipped_weapon->tier);
        hi = weapon_dmg_max(equipped_weapon->tier);
        if (equipped_weapon->cursed) hi = lo + (hi - lo) / 2;      /* 诅咒件打折 */
    } else {
        lo = 1;
        hi = 1 + str / 4;
    }
    int dmg = rndNormalRange(g.ui_rng, lo, hi);
    int req = equipped_weapon ? equipped_weapon->str_req : 0;
    if (str > req) dmg += rndIntRange(g.ui_rng, 0, str - req);     /* exStr 力量盈余 */
    if (equipped_ring && equipped_ring->sub == RG_MIGHT) dmg += 2;

    enemy->damage(dmg, "hero");

    if (!m->is_alive()) {
        int xp = m->xp_in_kill;
        exp += xp;
        g.sfx(DG_SFX_KILL);
        while (exp >= maxExp()) {
            exp -= maxExp();
            lvl++;
            hp_max += 5; hp += 5;
            str += 1; attack_skill += 2; defense_skill += 1;
            char buf[16];
            snprintf(buf, sizeof(buf), "LV%d", lvl);
            g.add_float(x, y, buf, C_LEVEL);
            g.sfx(DG_SFX_LEVELUP);
            char msg[64];
            snprintf(msg, sizeof(msg), "你升到了 %d 级！力量 %d，生命 %d。", lvl, str, hp_max);
            g.log(msg);
        }
    }
    return 1;
}

/* 护甲减伤掷骰（在真正掉血之前吞掉一层） */
int Hero::defenseRoll(Mob* enemy)
{
    (void)enemy;
    if (!equipped_armor) return 0;
    int lo = armor_dr_min(equipped_armor->tier);
    int hi = armor_dr_max(equipped_armor->tier);
    /* 上游 Armor.drRoll() = Random.NormalIntRange(DRMin, DRMax)；力量不足只影响
     * 闪避（见 defenseSkill），不减 DR 上限——之前把 hi 砍四分之一是臆造。 */
    return rndNormalRange(Game::instance().ui_rng, lo, hi);
}

void Hero::damage(int dmg, const char* src)
{
    if (dmg <= 0) { Actor::damage(0, src); return; }
    int dr = defenseRoll(nullptr);
    int left = dmg - dr;
    if (left < 0) left = 0;
    if (dr > 0 && left == 0) {
        Game::instance().add_float(x, y, "-", C_MISS);             /* 完全格挡 */
        Game::instance().anim_running = true;
    }
    Actor::damage(left, src);
}

void Hero::die()
{
    Game& g = Game::instance();
    g.scene = DG_SCENE_GAME_OVER;
    g.sfx(DG_SFX_DIE);
    g.log("你死了……地牢记住了你的名字。");
    g.fb_dirty = true;
    ESP_LOGW(TAG, "hero died at depth %d", g.depth);
}

bool Hero::pickup(Item* it)
{
    if (!it) return false;
    Game& g = Game::instance();
    if (it->kind == Item::K_AMULET) {
        has_amulet = true;
        g.free_item(it);
        g.sfx(DG_SFX_WIN);
        g.log("你拿到了 Yogs-Dzewa 的护身符 —— 你赢了！");
        g.scene = DG_SCENE_WIN;
        g.fb_dirty = true;
        return true;
    }
    if (it->kind == Item::K_KEY) {
        /* 钥匙不占背包格：它是个计数，开门时消费 */
        keys++;
        g.free_item(it);
        g.sfx(DG_SFX_PICKUP);
        g.log("你摸到了一把钥匙。");
        return true;
    }
    if (inv_count >= DG_MAX_INVENTORY) {
        g.log("背包满了。");
        g.sfx(DG_SFX_ERROR);
        return false;
    }
    inventory[inv_count++] = it;
    it->x = it->y = -1;
    g.sfx(it->kind == Item::K_GOLD ? DG_SFX_GOLD : DG_SFX_PICKUP);
    /* 空槽自动填装（非诅咒才填 —— 诅咒装上去就摘不下来，得让玩家自己决定） */
    if (!it->cursed) {
        if (it->kind == Item::K_WEAPON && !equipped_weapon) equip(it);
        else if (it->kind == Item::K_ARMOR && !equipped_armor) equip(it);
        else if (it->kind == Item::K_RING && !equipped_ring) equip(it);
    }
    return true;
}

bool Hero::equip(Item* it)
{
    if (!it) return false;
    Game& g = Game::instance();
    Item** slot = nullptr;
    uint8_t which = EQ_NONE;
    if (it->kind == Item::K_WEAPON)      { slot = &equipped_weapon; which = EQ_WEAPON; }
    else if (it->kind == Item::K_ARMOR)  { slot = &equipped_armor;  which = EQ_ARMOR; }
    else if (it->kind == Item::K_RING)   { slot = &equipped_ring;   which = EQ_RING; }
    else { g.sfx(DG_SFX_ERROR); return false; }

    if (*slot) {
        if ((*slot)->equipped == which) { g.sfx(DG_SFX_ERROR); return false; }
        (*slot)->equipped = EQ_NONE;
    }
    *slot = it;
    it->equipped = which;
    if (it->cursed) {
        if (it->kind == Item::K_RING) {
            /* 诅咒戒指摘不下来：直接把旧的那件也焊死在身上 */
            g.log("戒指扣住了你的手指 —— 它摘不下来了。");
        } else {
            g.log("这东西一戴上就取不掉了。");
        }
    } else {
        g.log(it->name);
    }
    g.sfx(DG_SFX_PICKUP);
    g.fb_dirty = true;
    return true;
}

bool Hero::unequip(Item* it)
{
    if (!it || it->cursed) { Game::instance().sfx(DG_SFX_ERROR); return false; }
    if (it->kind == Item::K_WEAPON && equipped_weapon == it) equipped_weapon = nullptr;
    else if (it->kind == Item::K_ARMOR && equipped_armor == it) equipped_armor = nullptr;
    else if (it->kind == Item::K_RING && equipped_ring == it) equipped_ring = nullptr;
    else return false;
    it->equipped = EQ_NONE;
    Game::instance().sfx(DG_SFX_SELECT);
    return true;
}

bool Hero::use(int slot)
{
    if (slot < 0 || slot >= inv_count) return false;
    Item* it = inventory[slot];
    if (!it) return false;
    if (it->is_equipment()) return equip(it);
    int consumed = item_use(this, it);
    if (!consumed) return false;
    /* 从背包里摘掉并归还池子 */
    for (int i = slot; i + 1 < inv_count; i++) inventory[i] = inventory[i + 1];
    inventory[--inv_count] = nullptr;
    Game::instance().free_item(it);
    Game::instance().fb_dirty = true;
    return true;
}

void Hero::drop(int slot)
{
    if (slot < 0 || slot >= inv_count) return;
    Item* it = inventory[slot];
    if (!it) return;
    Game& g = Game::instance();
    if (it->equipped != EQ_NONE) {
        if (it->cursed) { g.log("它粘在你身上，扔不掉。"); g.sfx(DG_SFX_ERROR); return; }
        unequip(it);
    }
    for (int i = slot; i + 1 < inv_count; i++) inventory[i] = inventory[i + 1];
    inventory[--inv_count] = nullptr;
    it->x = x; it->y = y;
    it->on_drop(level);
    g.log("你把它扔在了地上。");
    g.sfx(DG_SFX_STEP);
    g.fb_dirty = true;
}

}  /* namespace dg */
