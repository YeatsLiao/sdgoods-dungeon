/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * item_def.h —— 物品定义表（引擎内部头，不出 src/）
 *
 * v0.4 把「物品」从类层级改成三坐标查表：kind（大类）× sub（变体）× tier（档位）。
 * 名字 / 图标 / 数值全部集中在这里，UI 通过 dg_item_info_t 直接消费，
 * 引擎在 item.cpp 的一个 switch 里结算效果。图标格子号见 dg_icons.h 的
 * DGITEM_*（照图逐格核对过，索引口径写进 DESIGN.md 素材一节）。
 */
#ifndef DG_ITEM_DEF_H
#define DG_ITEM_DEF_H

#include <stdint.h>

namespace dg {

class Item;
class JavaRandom;

/* 药水变体（Item::sub）—— 颜色顺序对齐上游 items.png 352 起的彩液行 */
enum PotionSub : int16_t {
    POT_HEAL = 0,     /* 红斑：治疗 */
    POT_HASTE,        /* 黄斑：急速 */
    POT_INVIS,        /* 透明：隐身 */
    POT_LEVITATE,     /* 白斑：悬浮 */
    POT_STRENGTH,     /* 金斑：力量 */
    POT_PURIFY,       /* 绿斑：净化 */
    POT_COUNT
};

/* 卷轴变体 */
enum ScrollSub : int16_t {
    SC_MAP = 0,       /* 魔法地图：揭全图 */
    SC_UPGRADE,       /* 升级：给已装备的武器/护甲 +1 */
    SC_REMOVE_CURSE,  /*  Remove Curse：解掉身上诅咒 */
    SC_TELEPORT,      /* 传送：随机落到本层空地 */
    SC_COUNT
};

/* 戒指变体 */
enum RingSub : int16_t {
    RG_MIGHT = 0,     /* 力量 +2 */
    RG_ACCURACY,      /* 命中 +8 */
    RG_EVASION,       /* 闪避 +8 */
    RG_COUNT
};

/* 法杖变体 */
enum WandSub : int16_t {
    WD_BOLT = 0,      /* 魔法飞弹：固定伤害远程 */
    WD_SLOW,          /* 缓速：目标 SLOW */
    WD_COUNT
};

/* 食物变体 */
enum FoodSub : int16_t {
    FD_RATION = 0,    /* 口粮：普通回复 */
    FD_PASTY,         /* 馅饼：大额回复 */
    FD_COUNT
};

/* 装备槽（Item::equipped 字段值） */
enum EquipSlot : uint8_t { EQ_NONE = 0, EQ_WEAPON = 1, EQ_ARMOR = 2, EQ_RING = 3 };

/* ---- 数值表 ---- */
int weapon_dmg_min(int tier);
int weapon_dmg_max(int tier);
int weapon_str_req(int tier);
int weapon_icon(int tier);
const char* weapon_name(int tier);

int armor_dr_min(int tier);
int armor_dr_max(int tier);
int armor_str_req(int tier);
int armor_icon(int tier);
const char* armor_name(int tier);

/* ---- 名字 / 图标 ---- */
const char* item_name(int kind, int sub, int tier);
int         item_icon(int kind, int sub, int tier);

/* ---- 构造与掉落 ---- */
/* 就地填好一件物品（不动 pool 位图、不落格子）。 */
void fill_item(Item* it, int kind, int sub, int tier);
/* 按 quality(0 垃圾 / 1 普通 / 2 精良 / 3 首领) 掉一件；池满返回 nullptr。
 * tier_cap 由当前层推算，越深允许越高档位。 */
Item* roll_drop(int quality, int depth, JavaRandom* r);
/* 装备档位上限（1..5） */
int tier_cap_for_depth(int depth);

/* ---- 效果结算（实现见 item.cpp）----
 * item_use 只结算效果，不碰背包数组 —— 是否从背包移除由调用方
 * （Hero::use）决定，避免两个文件抢同一份生命周期的所有权。
 * 返回 1 = 已消耗（一次性用品）；0 = 失败/未消耗。 */
int  item_use(class Hero* h, Item* it);

}  /* namespace dg */

#endif
