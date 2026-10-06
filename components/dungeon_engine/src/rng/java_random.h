/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * java_random.h —— 复刻 java.util.Random 的 48-bit LCG 伪随机数发生器
 *
 * 架构决策：Shattered PD 的关卡生成 / 掉落 / 命中判定全部依赖 Gdx.rand
 * （= java.util.Random），必须精确复刻才能保证「同 seed 出同地图」与手感
 * 数值一致。用 C 标准 rand() 会漂手感、毁掉 Daily Run 语义。
 *
 * 参数（与 OpenJDK 严格一致）：
 *   multiplier : 0x5DEECE66D
 *   addend     : 0xB
 *   mask       : (1L << 48) - 1
 *
 * do not use try/catch, no dynamic_cast —— 本工程 -fno-exceptions -fno-rtti。
 */
#ifndef DG_JAVA_RANDOM_H
#define DG_JAVA_RANDOM_H

#include <stdint.h>

namespace dg {

class JavaRandom {
public:
    /* 与 Java new Random(seed) 语义一致：初次 next() 前会 XOR 一次 mask */
    explicit JavaRandom(uint64_t seed = 0);

    /* 重播种 */
    void setSeed(uint64_t seed);

    /* Java Random.nextInt() —— 32 位全范围 */
    int nextInt();

    /* Java Random.nextInt(bound) —— 均匀 [0, bound)，bound 必须是正整数 */
    int nextInt(int bound);

    /* Java Random.nextLong() —— 64 位全范围 */
    int64_t nextLong();

    /* Java Random.nextFloat() —— 均匀 [0.0, 1.0) */
    float nextFloat();

    /* Java Random.nextDouble() —— 均匀 [0.0, 1.0) */
    double nextDouble();

    /* Java Random.nextBoolean() */
    bool nextBoolean();

    /* Java Random.nextGaussian() —— Box-Muller 双元法，与 OpenJDK 保持
     * 相同的 spare/hasNext 缓存语义（同一 seed 序列下第 k 次调用产出一致）*/
    double nextGaussian();

    /* 底层 next(nbits)，1..32 位 */
    int next(int nbits);

private:
    uint64_t m_seed;
    /* nextGaussian 的双元缓存（对齐 OpenJDK 行为） */
    bool     m_has_next_gauss = false;
    double   m_next_gauss     = 0.0;
};

/* ===== 单元测试：dg_test_java_random_selftest =====
 * 用 OpenJDK 已知答案向量（KAT）核对，任何不一致返回 -1。
 * dg_api_run_selftest 内部调用，可在开机日志看到结果。
 */
int dg_test_java_random_selftest();

/* ===== 上游战斗 / 伤害掷骰助手（对齐 com.watabou.utils.Random）=====
 * 这些是 hit()/damageRoll() 反复要用的均匀与三角分布，集中一处，
 * 免得 hero.cpp / mob.cpp / item_def.cpp 三份各写各的口径漂。 */

/* Random.IntRange(min,max) —— 均匀闭区间 [min,max] */
inline int rndIntRange(JavaRandom* r, int min, int max)
{
    if (max <= min) return min;
    return min + r->nextInt(max - min + 1);
}

/* Random.NormalIntRange(min,max) —— 上游伤害用的三角分布（两次均匀取平均） */
inline int rndNormalRange(JavaRandom* r, int min, int max)
{
    if (max <= min) return min;
    int a = min + r->nextInt(max - min + 1);
    int b = min + r->nextInt(max - min + 1);
    return (a + b) >> 1;
}

/* Char.hit() 核心：acuRoll=rand[0,acu) vs defRoll=rand[0,def)，acuRoll>=defRoll 命中。
 *   · 上游 defStat<=0 直接必中；acuStat<=0 视为落空。
 *   · 隐身 / 偷袭必中的分支由调用方短路（不进这里）。 */
inline bool rollHit(JavaRandom* r, int acuStat, int defStat)
{
    if (defStat <= 0) return true;
    if (acuStat <= 0) return false;
    return r->nextInt(acuStat) >= r->nextInt(defStat);
}

}  /* namespace dg */

#endif
