/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mob_spec.cpp —— 物种表常量 + 按层挑选
 */
#include "actor/mob_spec.h"
#include "gfx/gfx.h"
#include "rng/java_random.h"

namespace dg {

/* sheet 字段写成 gfx::Sheet 的显式转换：图集缺失时渲染器会回退纯色块，
 * 但枚举值拼错就是静默错位（把 gnoll 画成 thief），所以这里只允许编译期常量。 */
#define SH(x) ((uint8_t)dg::gfx::x)

const MobSpec MOB_SPECS[MOB_SPEC_COUNT] = {
    /* 字段序：name sheet flags hp def acc atk_min atk_max xp loot dmin dmax see speed
     * （数值抄上游各 Mob.java 的 HT / defenseSkill / attackSkill / damageRoll / EXP；
     *  damageRoll 的 NormalIntRange(min,max) 拆成 atk_min/atk_max；
     *  speed 16=1.0×，32=2.0×（Crab/Bat 上游 baseSpeed=2）；表内层号 1 基） */

    /* ---- 下水道（1-4F）---- */
    { "老鼠",     SH(SH_MOB_RAT),      MF_NONE,                             8,  2,  8,  1,  4,  1, 0, 1,  4,  8, 16 },
    { "蛇",       SH(SH_MOB_SNAKE),    MF_NONE,                             4, 25, 10,  1,  4,  2, 0, 1,  4,  8, 16 },
    { "鬣狗人",   SH(SH_MOB_GNOLL),    MF_RANGED,                          12,  4, 10,  1,  6,  2, 1, 1,  4,  8, 16 },
    { "蜂群",     SH(SH_MOB_SWARM),    MF_SPLITTER,                        50,  5, 10,  1,  4,  3, 1, 1,  7,  8, 16 },
    { "螃蟹",     SH(SH_MOB_CRAB),     MF_NONE,                            15,  5, 12,  1,  7,  4, 1, 1,  4,  8, 32 },
    { "史莱姆",   SH(SH_MOB_SLIME),    MF_SPLITTER,                        20,  5, 12,  2,  5,  4, 1, 2,  4,  8, 16 },

    /* ---- 监狱（5-8F）---- */
    { "骷髅",     SH(SH_MOB_SKELETON), MF_UNDEAD,                          25,  9, 12,  2, 10,  5, 1, 5,  8,  8, 16 },
    { "窃贼",     SH(SH_MOB_THIEF),    MF_THIEF | MF_STEALTHY,             20, 12, 12,  1, 10,  5, 2, 5,  8,  8, 16 },
    { "DM-100",   SH(SH_MOB_DM100),    MF_RANGED,                          20,  8, 11,  2,  8,  6, 1, 5,  8,  8, 16 },
    { "狱警",     SH(SH_MOB_GUARD),    MF_RANGED,                          40, 10, 12,  4, 12,  7, 1, 6,  8,  8, 16 },
    { "亡灵法师", SH(SH_MOB_NECROMANCER), MF_RANGED | MF_SUMMONER | MF_UNDEAD, 40, 14, 16, 5, 15, 8, 2, 7, 8, 8, 16 },

    /* ---- 洞穴（9-12F）---- */
    { "蝙蝠",     SH(SH_MOB_BAT),      MF_AGGRO,                           30, 15, 16,  5, 18,  7, 0, 9, 12,  8, 32 },
    { "野兽人",   SH(SH_MOB_BRUTE),    MF_RANGED,                          40, 15, 20,  8, 20,  8, 1, 9, 12,  8, 16 },
    { "萨满",     SH(SH_MOB_SHAMAN),   MF_RANGED,                          35, 15, 18,  5, 10,  8, 1, 9, 12,  8, 16 },
    { "蜘蛛",     SH(SH_MOB_SPINNER),  MF_RANGED,                          50, 17, 22, 10, 20,  9, 1,10, 12,  8, 16 },
    { "DM-200",   SH(SH_MOB_DM200),    MF_RANGED,                          80, 12, 20, 10, 25,  9, 2,11, 12,  8, 16 },

    /* ---- 特殊：宝箱怪（不进轮转表，由开箱触发）---- */
    { "宝箱怪",   SH(SH_MOB_MIMIC),    MF_NONE,                            30, 12, 20,  6, 16,  8, 3, 2, 12,  8, 16 },

    /* ---- 章节首领：4F Goo / 8F Tengu / 12F DwarfKing（数值抄上游，未做挑战难度加成）---- */
    { "腐蚀之胶", SH(SH_MOB_GOO),      MF_BOSS,                           100,  8, 12,  1,  8, 10, 3, 4,  4, 12, 16 },
    { "天狗",     SH(SH_MOB_TENGU),    MF_BOSS | MF_RANGED,               200, 15, 20,  5, 20, 20, 3, 8,  8, 12, 16 },
    { "矮人之王", SH(SH_MOB_KING),     MF_BOSS | MF_RANGED | MF_SUMMONER, 300, 22, 24,  8, 30, 40, 3,12, 12, 12, 16 },
};

/* 上游 MobSpawner.standardMobRotation 的 12 层映射（首领层同时铺常规怪）。
 * 每层是一个「物种多重集」，均匀抽一个 —— 与上游 shuffle 后随机取等价。 */
static const uint8_t ROT_F1[]  = { MOB_RAT, MOB_RAT, MOB_RAT, MOB_SNAKE };
static const uint8_t ROT_F2[]  = { MOB_RAT, MOB_RAT, MOB_SNAKE, MOB_GNOLL, MOB_GNOLL };
static const uint8_t ROT_F3[]  = { MOB_RAT, MOB_SNAKE, MOB_GNOLL, MOB_GNOLL, MOB_GNOLL, MOB_SWARM, MOB_CRAB };
static const uint8_t ROT_F4[]  = { MOB_GNOLL, MOB_SWARM, MOB_CRAB, MOB_CRAB, MOB_SLIME, MOB_SLIME };
static const uint8_t ROT_F5[]  = { MOB_SKELETON, MOB_SKELETON, MOB_SKELETON, MOB_THIEF, MOB_SWARM };
static const uint8_t ROT_F6[]  = { MOB_SKELETON, MOB_SKELETON, MOB_SKELETON, MOB_THIEF, MOB_DM100, MOB_GUARD };
static const uint8_t ROT_F7[]  = { MOB_SKELETON, MOB_SKELETON, MOB_THIEF, MOB_DM100, MOB_DM100, MOB_GUARD, MOB_GUARD, MOB_NECROMANCER };
static const uint8_t ROT_F8[]  = { MOB_SKELETON, MOB_THIEF, MOB_DM100, MOB_DM100, MOB_GUARD, MOB_GUARD, MOB_NECROMANCER, MOB_NECROMANCER };
static const uint8_t ROT_F9[]  = { MOB_BAT, MOB_BAT, MOB_BAT, MOB_BRUTE, MOB_SHAMAN };
static const uint8_t ROT_F10[] = { MOB_BAT, MOB_BAT, MOB_BRUTE, MOB_BRUTE, MOB_SHAMAN, MOB_SPINNER };
static const uint8_t ROT_F11[] = { MOB_BAT, MOB_BRUTE, MOB_BRUTE, MOB_SHAMAN, MOB_SHAMAN, MOB_SPINNER, MOB_SPINNER, MOB_DM200 };
static const uint8_t ROT_F12[] = { MOB_BAT, MOB_BRUTE, MOB_SHAMAN, MOB_SHAMAN, MOB_SPINNER, MOB_SPINNER, MOB_DM200, MOB_DM200 };

const MobSpec* mob_pick_for_depth(JavaRandom* r, int depth)
{
    const uint8_t* tbl = nullptr;
    int n = 0;
#define ROT(d) case d: tbl = ROT_F##d; n = (int)(sizeof(ROT_F##d) / sizeof(ROT_F##d[0])); break;
    switch (depth + 1) {   /* 入参 0 基，轮转表 1 基 */
        ROT(1) ROT(2) ROT(3) ROT(4) ROT(5) ROT(6)
        ROT(7) ROT(8) ROT(9) ROT(10) ROT(11) ROT(12)
        /* 越界（理论上不会到，最大 12 层）：回退到 1 层 */
        default: tbl = ROT_F1; n = (int)(sizeof(ROT_F1) / sizeof(ROT_F1[0])); break;
    }
#undef ROT
    if (!n) return nullptr;
    return &MOB_SPECS[tbl[r->nextInt(n)]];
}

const MobSpec* mob_boss_for_depth(int depth)
{
    switch (depth + 1) {              /* 入参 0 基 */
    case 4:  return &MOB_SPECS[MOB_GOO];
    case 8:  return &MOB_SPECS[MOB_TENGU];
    case 12: return &MOB_SPECS[MOB_DWARFKING];
    default: return nullptr;
    }
}

}  /* namespace dg */
