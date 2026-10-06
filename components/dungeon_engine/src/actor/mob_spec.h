/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mob_spec.h —— 怪物物种表（引擎内部头，不出 src/）
 *
 * 上游 Shattered 用「一怪一类」的继承树（Mob → Rat → Snake …），本移植把
 * 差异压进这张常量表 + Mob::act() 里的 flags 分支：少 20 张 vtable，换一次
 * switch，flash 省、调试直白。数值口径来自上游 Mob 子类的 hp/defense/
 * damageRoll/XP 常量（首领做了手持平衡）。
 */
#ifndef DG_MOB_SPEC_H
#define DG_MOB_SPEC_H

#include <stdint.h>

namespace dg {

class JavaRandom;

/* 物种行为位 */
enum MobFlag : uint16_t {
    MF_NONE      = 0,
    MF_RANGED    = 1 << 0,   /* 远程：投矛 / 法术光束 */
    MF_BOSS      = 1 << 1,   /* 首领：不随层缩放、专属房间 */
    MF_UNDEAD    = 1 << 2,   /* 亡灵：免疫毒 / 燃烧 */
    MF_STEALTHY  = 1 << 3,   /* 潜行：唤醒距离减半 */
    MF_SUMMONER  = 1 << 4,   /* 召唤：周期性叫随从 */
    MF_SPLITTER  = 1 << 5,   /* 分裂：受击概率一分为二 */
    MF_THIEF     = 1 << 6,   /* 偷窃：命中后抢金币 */
    MF_AGGRO     = 1 << 7,   /* 主动攻击：不需要视线就醒 */
};

struct MobSpec {
    const char* name;       /* 中文名（飘字 / 消息） */
    uint8_t     sheet;      /* gfx::Sheet（SH_MOB_*），渲染取图 */
    uint16_t    flags;
    int16_t     hp;
    int16_t     def;        /* 防御技能基础值 */
    int16_t     acc;        /* 命中技能基础值 */
    int16_t     atk_min;
    int16_t     atk_max;
    int16_t     xp;         /* 击杀经验 */
    int16_t     loot;       /* 掉落倾向：0 无 1 普通 2 较好 3 首领 */
    uint8_t     dmin;       /* 出现层区间（1 基，含） */
    uint8_t     dmax;       /* 0 = 不限 */
    uint8_t     see;        /* 唤醒视距 */
    uint8_t     speed;      /* 行动倍率（16 = 1.0×，上游口径） */
};

/* 物种编号（顺序即 MOB_SPECS 下标；对齐上游 MobSpawner 名录） */
enum MobId : uint8_t {
    /* 下水道 */
    MOB_RAT = 0,
    MOB_SNAKE,
    MOB_GNOLL,
    MOB_SWARM,
    MOB_CRAB,
    MOB_SLIME,
    /* 监狱 */
    MOB_SKELETON,
    MOB_THIEF,
    MOB_DM100,
    MOB_GUARD,
    MOB_NECROMANCER,
    /* 洞穴 */
    MOB_BAT,
    MOB_BRUTE,
    MOB_SHAMAN,
    MOB_SPINNER,
    MOB_DM200,
    /* 特殊 + 首领 */
    MOB_MIMIC,
    MOB_GOO,
    MOB_TENGU,
    MOB_DWARFKING,
    MOB_SPEC_COUNT
};

extern const MobSpec MOB_SPECS[MOB_SPEC_COUNT];

/* 按当前层从常规怪里挑一只（首领不进这张表，由 spawn 逻辑定点投放）。
 * 找不到返回 nullptr。 */
const MobSpec* mob_pick_for_depth(JavaRandom* r, int depth);

/* 首领查表（depth 1 基：4F 腐蚀之胶 Goo / 8F 天狗 Tengu / 12F 矮人之王 DwarfKing）。非常领层返回 nullptr。 */
const MobSpec* mob_boss_for_depth(int depth);

}  /* namespace dg */

#endif
