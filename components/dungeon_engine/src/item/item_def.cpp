/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * item_def.cpp —— 物品数值 / 名字 / 图标表 + 掉落掷骰
 */
#include "item/item_def.h"
#include "dg_types.h"
#include "dg_icons.h"
#include "rng/java_random.h"

namespace dg {

/* 表下标一律走这两个钳位：调用方传错 tier/sub 只会拿到边界档位，
 * 不会越界读到表外的字符串（真机上那是直接崩在 label 渲染里）。 */
static inline int clamp_tier(int t)
{
    if (t < 1) return 1;
    if (t > 5) return 5;
    return t;
}
static inline int clamp_sub(int s, int n)
{
    if (s < 0) return 0;
    if (s >= n) return n - 1;
    return s;
}

/* tier 下标 1..5（0 号位是占位，避免调用方 off-by-one 悄悄拿到 1 档数值） */
static const char* const k_wep_name[6] = {
    "", "锈蚀短刀", "短剑", "长剑", "战斧", "巨剑"
};
/* 对齐 items.png 刀刃区 80~111 里挑出的 5 档（见 dg_icons.h 注释） */
static const int k_wep_icon[6]  = { 0, DGITEM_WEP_T1, DGITEM_WEP_T2, DGITEM_WEP_T3,
                                    DGITEM_WEP_T4, DGITEM_WEP_T5 };
/* 上游 MeleeWeapon.java（lvl=0 基础，本移植不做强化）：
 *   min(tier) = tier            →  1,2,3,4,5
 *   max(tier) = 5*(tier+1)      →  10,15,20,25,30
 *   STRReq(tier) = 8 + tier*2   →  10,12,14,16,18（武器/护甲同式） */
static const int k_wep_dmin[6]  = { 0, 1, 2, 3, 4, 5 };
static const int k_wep_dmax[6]  = { 0, 10, 15, 20, 25, 30 };
static const int k_wep_str[6]   = { 0, 10, 12, 14, 16, 18 };

static const char* const k_arm_name[6] = {
    "", "布甲", "皮甲", "锁子甲", "板甲", "符文甲"
};
static const int k_arm_icon[6]  = { 0, DGITEM_ARMOR_T1, DGITEM_ARMOR_T2, DGITEM_ARMOR_T3,
                                    DGITEM_ARMOR_T4, DGITEM_ARMOR_T5 };
/* 上游 Armor.java（lvl=0）：DRMin=0；DRMax=tier*(2+0)=tier*2 → 2,4,6,8,10 */
static const int k_arm_drmin[6] = { 0, 0, 0, 0, 0, 0 };
static const int k_arm_drmax[6] = { 0, 2, 4, 6, 8, 10 };
static const int k_arm_str[6]   = { 0, 10, 12, 14, 16, 18 };

static const char* const k_pot_name[POT_COUNT] = {
    "治疗药水", "急速药水", "隐身药水", "悬浮药水", "力量药水", "净化药水", "经验药水"
};
static const char* const k_scr_name[SC_COUNT] = {
    "魔法地图卷轴", "升级卷轴", "解除诅咒卷轴", "传送卷轴",
    "鉴定卷轴", "恐惧卷轴", "沉睡卷轴", "狂暴卷轴"
};
static const char* const k_rng_name[RG_COUNT] = {
    "力量之戒", "精准之戒", "疾影之戒", "急速之戒", "再生之戒", "蒺藜之戒"
};
static const char* const k_wnd_name[WD_COUNT] = {
    "飞弹法杖", "缓速法杖", "烈焰法杖", "寒冰法杖"
};
static const char* const k_food_name[FD_COUNT] = {
    "旅行口粮", "猎人馅饼"
};

int weapon_dmg_min(int t) { return k_wep_dmin[clamp_tier(t)]; }
int weapon_dmg_max(int t) { return k_wep_dmax[clamp_tier(t)]; }
int weapon_str_req(int t) { return k_wep_str[clamp_tier(t)]; }
int weapon_icon(int t)    { return k_wep_icon[clamp_tier(t)]; }
const char* weapon_name(int t) { return k_wep_name[clamp_tier(t)]; }

int armor_dr_min(int t) { return k_arm_drmin[clamp_tier(t)]; }
int armor_dr_max(int t) { return k_arm_drmax[clamp_tier(t)]; }
int armor_str_req(int t) { return k_arm_str[clamp_tier(t)]; }
int armor_icon(int t)    { return k_arm_icon[clamp_tier(t)]; }
const char* armor_name(int t) { return k_arm_name[clamp_tier(t)]; }

const char* item_name(int kind, int sub, int tier)
{
    switch (kind) {
    case Item::K_WEAPON: return k_wep_name[clamp_tier(tier)];
    case Item::K_ARMOR:  return k_arm_name[clamp_tier(tier)];
    case Item::K_POTION: return k_pot_name[clamp_sub(sub, POT_COUNT)];
    case Item::K_SCROLL: return k_scr_name[clamp_sub(sub, SC_COUNT)];
    case Item::K_RING:   return k_rng_name[clamp_sub(sub, RG_COUNT)];
    case Item::K_WAND:   return k_wnd_name[clamp_sub(sub, WD_COUNT)];
    case Item::K_FOOD:   return k_food_name[clamp_sub(sub, FD_COUNT)];
    case Item::K_KEY:    return "钥匙";
    case Item::K_GOLD:   return "金币";
    case Item::K_AMULET: return "Yogs-Dzewa 的护身符";
    default:             return "未知之物";
    }
}

const char* item_display(int kind, int sub, int tier, bool identified)
{
    /* 只有药水/卷轴/戒指/法杖这四类需要鉴定；未鉴定给按大类的泛称 */
    if (!identified) {
        switch (kind) {
        case Item::K_POTION: return "未鉴定的药水";
        case Item::K_SCROLL: return "未鉴定的卷轴";
        case Item::K_RING:   return "未鉴定的戒指";
        case Item::K_WAND:   return "未鉴定的法杖";
        default: break;
        }
    }
    return item_name(kind, sub, tier);
}

int item_icon(int kind, int sub, int tier)
{
    switch (kind) {
    case Item::K_WEAPON: return k_wep_icon[clamp_tier(tier)];
    case Item::K_ARMOR:  return k_arm_icon[clamp_tier(tier)];
    /* 彩液区 352 起是连续的 12 格，按变体号顺排即可对上颜色 */
    case Item::K_POTION: return DGITEM_POTION + clamp_sub(sub, POT_COUNT);
    case Item::K_SCROLL: return DGITEM_SCROLL + (clamp_sub(sub, SC_COUNT) & 1);
    case Item::K_RING:   return DGITEM_RING + clamp_sub(sub, RG_COUNT);
    case Item::K_WAND:   return DGITEM_WAND + clamp_sub(sub, WD_COUNT);
    case Item::K_FOOD:   return clamp_sub(sub, FD_COUNT) == FD_PASTY
                                  ? DGITEM_FOOD_PASTY : DGITEM_FOOD;
    case Item::K_KEY:    return DGITEM_KEY;
    case Item::K_GOLD:   return DGITEM_GOLD;
    case Item::K_AMULET: return DGITEM_AMULET;
    default:             return DGITEM_GEM;
    }
}

void fill_item(Item* it, int kind, int sub, int tier)
{
    it->kind    = (Item::Kind)kind;
    it->sub     = (int16_t)sub;
    it->tier    = (int16_t)clamp_tier(tier);
    it->qty     = 1;
    it->cursed  = 0;
    it->equipped = EQ_NONE;
    it->enchant  = 0;
    it->x = it->y = 0;
    switch (kind) {
    case Item::K_WEAPON:
        it->str_req = (int16_t)weapon_str_req(tier);
        break;
    case Item::K_ARMOR:
        it->str_req = (int16_t)armor_str_req(tier);
        break;
    default:
        it->str_req = 0;
        break;
    }
    it->name = item_name(kind, sub, tier);
    it->icon = (int16_t)item_icon(kind, sub, tier);
}

int tier_cap_for_depth(int depth)
{
    /* 0 基层号：1-4F 只掉 1~2 档，5-8F 到 3 档，9-12F 到 4~5 档 */
    int d = depth + 1;
    if (d <= 2) return 1;
    if (d <= 4) return 2;
    if (d <= 7) return 3;
    if (d <= 10) return 4;
    return 5;
}

/* ---- 掉落掷骰 ----
 * quality：0 杂物（地上随机） 1 普通怪 2 精英/宝箱 3 首领
 * 大类的权重按「越深越该掉装备」递增，与上游掉落表的方向一致；
 * 具体数值是本移植的手持平衡，目标是 12 层能穿到 3~4 档。 */
Item* roll_drop(int quality, int depth, JavaRandom* r)
{
    int cap = tier_cap_for_depth(depth);
    int tier = 1 + r->nextInt(cap);
    if (quality >= 2 && tier < cap) tier++;
    if (quality >= 3) tier = cap;

    int roll = r->nextInt(100);
    int kind, sub = 0;

    if (quality >= 3) {
        /* 首领：必掉一件满档装备 + 一笔金币（金币由调用方另投） */
        kind = r->nextBoolean() ? Item::K_WEAPON : Item::K_ARMOR;
    } else if (quality == 0) {
        /* 地表杂物：吃喝为主，偶尔一张卷轴 */
        if (roll < 45)      { kind = Item::K_FOOD;   sub = r->nextInt(FD_COUNT); }
        else if (roll < 75) { kind = Item::K_POTION; sub = r->nextInt(POT_COUNT); }
        else if (roll < 90) { kind = Item::K_SCROLL; sub = r->nextInt(SC_COUNT); }
        else                { kind = Item::K_KEY; }
    } else if (quality == 1) {
        if (roll < 22)      { kind = Item::K_WEAPON; }
        else if (roll < 44) { kind = Item::K_ARMOR; }
        else if (roll < 64) { kind = Item::K_POTION; sub = r->nextInt(POT_COUNT); }
        else if (roll < 80) { kind = Item::K_SCROLL; sub = r->nextInt(SC_COUNT); }
        else if (roll < 92) { kind = Item::K_FOOD;   sub = r->nextInt(FD_COUNT); }
        else                { kind = Item::K_WAND;   sub = r->nextInt(WD_COUNT); }
    } else {
        /* 宝箱 / 精英：装备与稀有消耗品各半 */
        if (roll < 32)      { kind = Item::K_WEAPON; }
        else if (roll < 60) { kind = Item::K_ARMOR; }
        else if (roll < 74) { kind = Item::K_RING;   sub = r->nextInt(RG_COUNT); }
        else if (roll < 86) { kind = Item::K_POTION; sub = r->nextInt(POT_COUNT); }
        else                { kind = Item::K_SCROLL; sub = r->nextInt(SC_COUNT); }
    }

    Game& g = Game::instance();
    Item* it = g.alloc_item();
    if (!it) return nullptr;
    fill_item(it, kind, sub, tier);
    /* 20% 概率带诅咒（只贴装备，消耗品不诅咒）—— 逼出解除诅咒卷轴的价值 */
    if (it->is_equipment() && r->nextInt(100) < 20) it->cursed = 1;
    /* 高档武器（tier≥4）随机带近战附魔，品阶越高越可能（M4）*/
    if (kind == Item::K_WEAPON && it->tier >= 4 && r->nextInt(100) < 45)
        it->enchant = (int8_t)(1 + r->nextInt(EN_COUNT - 1));
    return it;
}

}  /* namespace dg */
