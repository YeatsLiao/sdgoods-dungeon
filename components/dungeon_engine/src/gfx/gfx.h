/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * gfx.h —— 烘焙图集（RGB5 容器）注册表 + blit 原语 + 迷你像素字
 *          （引擎内部头，不出 src/）
 *
 * 架构决策（v0.2 定，v0.4 扩）：ESP32 侧零 PNG 解码 —— tools/pack_assets.py
 * 把上游图集预烘焙成 "RGB5" 容器（magic 4B + u16 w + u16 h + RGB565 LE），
 * 本模块一次性全部读进 PSRAM 常驻，渲染器只做 memcpy / 逐像素比色键。
 *
 * v0.4 相对 v0.2 的三点变化：
 *   1. 从「3 张硬编码图」变成「26 张命名图集注册表」（SH_* 枚举）；
 *   2. 透明约定从「纯黑=透明」改为「色键 0x001F=透明」—— 上游精灵普遍带
 *      黑色描边，旧约定会在角色身上画出洞（见 pack_assets.py 文件头）；
 *   3. 三张章节 tileset 全常驻 + 各自暗化副本，按 depth 直接切，不做运行期
 *      分区读（一次 128KB 的 flash 读会卡渲染帧，代价远大于 768KB PSRAM）。
 */
#ifndef DG_GFX_H
#define DG_GFX_H

#include <stdint.h>

namespace dg {
namespace gfx {

/* 透明色键（RGB565 纯蓝 0,0,31）。与 pack_assets.py::KEY_RGB565 严格对偶。 */
static constexpr uint16_t KEY = 0x001F;

/* ---- 图集句柄（顺序即注册表顺序，名字见 gfx.cpp::k_names）---- */
enum Sheet : uint8_t {
    /* 章节 tileset */
    SH_TILES_SEWERS = 0,
    SH_TILES_PRISON,
    SH_TILES_CAVES,
    /* 怪物帧图：16×16 一格，第 0 行横向连帧即 idle。名录顺序对齐上游
     * MobSpawner.standardMobRotation：下水道 → 监狱 → 洞穴 → 首领。 */
    SH_MOB_RAT,
    SH_MOB_SNAKE,
    SH_MOB_GNOLL,
    SH_MOB_SWARM,
    SH_MOB_CRAB,
    SH_MOB_SLIME,
    /* 监狱 */
    SH_MOB_SKELETON,
    SH_MOB_THIEF,
    SH_MOB_DM100,
    SH_MOB_GUARD,
    SH_MOB_NECROMANCER,
    /* 洞穴 */
    SH_MOB_BAT,
    SH_MOB_BRUTE,
    SH_MOB_SHAMAN,
    SH_MOB_SPINNER,
    SH_MOB_DM200,
    /* 特殊 */
    SH_MOB_MIMIC,
    /* 章节首领：下水道 Goo / 监狱 Tengu / 洞穴 DwarfKing */
    SH_MOB_GOO,
    SH_MOB_TENGU,
    SH_MOB_KING,
    /* 英雄：256×128，idle 帧在 (1,0) 12×15 */
    SH_HERO_WARRIOR,
    SH_HERO_MAGE,
    SH_HERO_ROGUE,
    SH_HERO_HUNTRESS,
    /* 物品 / UI */
    SH_ITEMS,
    SH_ICONS,
    SH_COUNT
};

/* 一次性懒加载全部图集；缺图不致命（该 Sheet 返回 nullptr，渲染器回退占位）。
 * 返回「至少拿到 tileset」= true；false 表示 assets 分区没烧素材。 */
bool load();
bool ready();

/* 原始像素 / 尺寸（未加载返回 nullptr / 0） */
const uint16_t* px(Sheet s);
int sheet_w(Sheet s);
int sheet_h(Sheet s);

/* 章节 tileset（chapter 0=下水道 1=监狱 2=洞穴，越界自动 clamp）与其暗化副本 */
const uint16_t* tileset(int chapter);
const uint16_t* tileset_dim(int chapter);

/* 第 0 行「有墨」的连续帧数（idle 动画长度）。图集缺失返回 1。
 * 上游帧图第 0 行后半段常是空白，直接按 w/16 取帧会让怪忽隐忽现。 */
int anim_frames(Sheet s);

/* ---- blit 原语（目标 fb 行主序 native RGB565，越界像素自动裁剪）---- */

/* 不透明拷贝（地形层用：源图已按黑底合成，无比色键必要） */
void blit(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
          const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h);

/* 跳过色键像素（精灵 / 图标层用） */
void blit_key(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
              const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h);

/* 同上，但落笔前把颜色压到 ~28% 亮度（记忆态里的掉落物 / 怪物残影） */
void blit_key_dim(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
                  const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h);

/* 受击闪白：把区域内非色键像素提亮（1 帧即撤，见 dungeon.cpp 的 flash 计时） */
void blit_flash(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
                const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h);

/* 纯色小方块 / 暗化整块 */
void fill_rect(uint16_t* fb, int fb_w, int fb_h, int dx, int dy, int w, int h,
               uint16_t color);

/* ---- 迷你像素字（伤害飘字 / 楼层标记）----
 * 3×5 点阵，只含 0-9 + - + ! x ? . 与 L V M S I 少量字母，够用即止。
 * 走引擎内渲染（逻辑 px），放大交给 UI，避免为几个数字扩中文字体子集。 */
void     text_px(uint16_t* fb, int fb_w, int fb_h, int x, int y,
                 const char* s, uint16_t color);
int      text_px_w(const char* s);   /* 含字间距的像素宽 */
uint16_t rgb565(uint8_t r8, uint8_t g8, uint8_t b8);

}  /* namespace gfx */
}  /* namespace dg */

#endif
