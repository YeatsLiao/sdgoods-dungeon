/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dg_icons.h —— 上游图集格子索引常量表（引擎内部头，不出 src/）
 *
 * 这些数字不是随手挑的：全部对齐 Shattered Pixel Dungeon 上游素材与
 * DungeonTileSheet / ItemSpriteSheet / Icons 的索引口径，改一个数就要去
 * 上游对一次源码。图集几何：
 *   tiles_{sewers,prison,caves}.png  256×256，16 列 × 16px
 *   sprites/items.png                256×512，16 列 × 16px
 *   interfaces/icons.png             256×128，16 列 × 16px
 * 格子号 idx → 像素 (idx % 16 * 16, idx / 16 * 16)。
 */
#ifndef DG_ICONS_H
#define DG_ICONS_H

/* 图集列数（三张图集都是 16 列 16px） */
#define DG_SHEET_COLS      16
#define DG_SHEET_CELL      16
#define DG_CELL_X(idx)     (((idx) % DG_SHEET_COLS) * DG_SHEET_CELL)
#define DG_CELL_Y(idx)     (((idx) / DG_SHEET_COLS) * DG_SHEET_CELL)
/* 上游 xy(列,行)，1 基 */
#define DG_XY(x, y)        (((x) - 1) + ((y) - 1) * DG_SHEET_COLS)

/* ===========================================================================
 * 1. 地形图集（对齐上游 tiles/DungeonTileSheet.java）
 * =========================================================================== */

/* --- 地面块（第 1 行，24 格）上游 DungeonTileSheet L52-77 --- */
#define DTS_GROUND            DG_XY(1, 1)     /* 0  */
#define DTS_FLOOR             (DTS_GROUND + 0)
#define DTS_FLOOR_DECO        (DTS_GROUND + 1)
#define DTS_GRASS             (DTS_GROUND + 2)
#define DTS_EMBERS            (DTS_GROUND + 3)
#define DTS_FLOOR_SP          (DTS_GROUND + 4)   /* 特殊地板（入口/楼梯周边） */
#define DTS_FLOOR_ALT_1       (DTS_GROUND + 6)
#define DTS_FLOOR_DECO_ALT    (DTS_GROUND + 7)
#define DTS_GRASS_ALT         (DTS_GROUND + 8)
#define DTS_EMBERS_ALT        (DTS_GROUND + 9)
#define DTS_FLOOR_SP_ALT      (DTS_GROUND + 10)
#define DTS_FLOOR_ALT_2       (DTS_GROUND + 12)

/* --- 入口 / 出口 / 井 / 基座（第 2 行前段）--- */
#define DTS_ENTRANCE          (DTS_GROUND + 16)
#define DTS_EXIT              (DTS_GROUND + 17)
#define DTS_WELL              (DTS_GROUND + 18)
#define DTS_EMPTY_WELL        (DTS_GROUND + 19)
#define DTS_PEDESTAL          (DTS_GROUND + 20)
#define DTS_ENTRANCE_SP       (DTS_GROUND + 22)

/* --- 深渊（第 2 行后段）--- */
#define DTS_CHASM             (DTS_GROUND + 24)

/* --- 水（第 3 行，16 格 = 4 位缝合掩码）--- */
#define DTS_WATER             DG_XY(1, 3)     /* 32 */
#define DTS_WATER_TOP         1
#define DTS_WATER_RIGHT       2
#define DTS_WATER_BOTTOM      4
#define DTS_WATER_LEFT        8

/* --- 平墙（第 4 行）--- */
#define DTS_FLAT_WALLS        DG_XY(1, 4)     /* 48 */
#define DTS_FLAT_WALL         (DTS_FLAT_WALLS + 0)
#define DTS_FLAT_WALL_DECO    (DTS_FLAT_WALLS + 1)
#define DTS_FLAT_WALL_BOOK    (DTS_FLAT_WALLS + 2)
#define DTS_FLAT_WALL_ALT     (DTS_FLAT_WALLS + 4)
#define DTS_FLAT_WALL_DECO_ALT (DTS_FLAT_WALLS + 5)
#define DTS_FLAT_WALL_BOOK_ALT (DTS_FLAT_WALLS + 6)
#define DTS_FLAT_DOOR         (DTS_FLAT_WALLS + 8)
#define DTS_FLAT_DOOR_OPEN    (DTS_FLAT_WALLS + 9)
#define DTS_FLAT_DOOR_LOCKED  (DTS_FLAT_WALLS + 10)
#define DTS_FLAT_DOOR_CRYSTAL (DTS_FLAT_WALLS + 11)
#define DTS_UNLOCKED_EXIT     (DTS_FLAT_WALLS + 12)
#define DTS_LOCKED_EXIT       (DTS_FLAT_WALLS + 13)

/* --- 平地形装饰（第 5 行）--- */
#define DTS_FLAT_OTHER        DG_XY(1, 5)     /* 64 */
#define DTS_ALCHEMY           (DTS_FLAT_OTHER + 0)
#define DTS_BARRICADE         (DTS_FLAT_OTHER + 1)
#define DTS_HIGH_GRASS        (DTS_FLAT_OTHER + 2)
#define DTS_FURROWED_GRASS    (DTS_FLAT_OTHER + 3)   /* 耕过的草（占 +3） */
#define DTS_HIGH_GRASS_ALT    (DTS_FLAT_OTHER + 5)   /* 上游是 +5，不是 +3！*/
#define DTS_FURROWED_ALT      (DTS_FLAT_OTHER + 6)
#define DTS_FLAT_STATUE       (DTS_FLAT_OTHER + 8)
#define DTS_FLAT_STATUE_SP    (DTS_FLAT_OTHER + 9)

/* --- 凸墙：能看到墙面（第 6~7 行，32 格）---
 * 上游掩码：+1 右侧开放、+2 左侧开放（getRaisedWallTile） */
#define DTS_RAISED_WALLS      DG_XY(1, 6)     /* 80 */
#define DTS_RAISED_WALL_OPEN_R    (DTS_RAISED_WALLS + 0)  /* 基础=右左都墙 */
#define DTS_RAISED_WALL         (DTS_RAISED_WALLS + 0)
#define DTS_RAISED_WALL_RIGHT   (DTS_RAISED_WALLS + 1)   /* 右侧开放 */
#define DTS_RAISED_WALL_LEFT    (DTS_RAISED_WALLS + 2)   /* 左侧开放 */
#define DTS_RAISED_WALL_PE      (DTS_RAISED_WALLS + 3)   /* 左右都开放 */
#define DTS_RAISED_WALL_DECO    (DTS_RAISED_WALLS + 4)
#define DTS_RAISED_WALL_DOOR    (DTS_RAISED_WALLS + 8)   /* 门前墙 */
#define DTS_RAISED_WALL_BOOK    (DTS_RAISED_WALLS + 12)  /* 书架墙 */
#define DTS_RAISED_WALL_ALT     (DTS_RAISED_WALLS + 16)

/* --- 凸门（第 8 行前段）--- */
#define DTS_RAISED_DOORS      DG_XY(1, 8)     /* 112 */
#define DTS_RAISED_DOOR           (DTS_RAISED_DOORS + 0)
#define DTS_RAISED_DOOR_OPEN      (DTS_RAISED_DOORS + 1)
#define DTS_RAISED_DOOR_LOCKED    (DTS_RAISED_DOORS + 2)
#define DTS_RAISED_DOOR_CRYSTAL   (DTS_RAISED_DOORS + 3)
#define DTS_RAISED_DOOR_SIDEWAYS  (DTS_RAISED_DOORS + 4)   /* 上下开门里的地板 */

/* --- 凸地形装饰（第 8 行后段起，上游 xy(9,8)）--- */
#define DTS_RAISED_OTHER      DG_XY(9, 8)     /* 120 */
#define DTS_RAISED_STATUE     (DTS_RAISED_OTHER + 8)   /* = 128 */

/* --- 内墙：四面被墙包住（第 10~11 行，掩码 16 格）---
 * 上游 stitchInternalWallTile：+1 右开放、+2 右下开放、+4 左下开放、+8 左开放 */
#define DTS_WALLS_INTERNAL    DG_XY(1, 10)    /* 144 */
#define DTS_INT_RIGHT         1
#define DTS_INT_RIGHT_BELOW   2
#define DTS_INT_LEFT_BELOW    4
#define DTS_INT_LEFT          8
#define DTS_WALLS_INTERNAL_DOOR DG_XY(1, 11)  /* 160 */

/* --- 悬挑墙：墙在上、地在下（第 13~14 行）--- */
#define DTS_WALLS_OVERHANG    DG_XY(1, 13)    /* 192 */
#define DTS_OH_RIGHT_BELOW    1
#define DTS_OH_LEFT_BELOW     2
#define DTS_OVERHANG_DECO     (DTS_WALLS_OVERHANG + 4)
#define DTS_OVERHANG_WOODEN   (DTS_WALLS_OVERHANG + 8)
#define DTS_DOOR_SIDEWAYS_OVERHANG        (DTS_WALLS_OVERHANG + 16)
#define DTS_DOOR_SIDEWAYS_OVERHANG_CLOSED (DTS_WALLS_OVERHANG + 20)
#define DTS_DOOR_SIDEWAYS_OVERHANG_LOCKED (DTS_WALLS_OVERHANG + 24)

#define DTS_DOOR_OVERHANG     DG_XY(1, 15)    /* 224 */
#define DTS_DOOR_OVERHANG_OPEN (DTS_DOOR_OVERHANG + 1)
#define DTS_DOOR_SIDEWAYS     (DTS_DOOR_OVERHANG + 3)  /* 门上方墙格的门楣 */
#define DTS_EXIT_UNDERHANG    (DTS_DOOR_OVERHANG + 6)
#define DTS_OTHER_OVERHANG    DG_XY(1, 16)    /* 232 */
#define DTS_OVERHANG_STATUE   (DTS_OTHER_OVERHANG + 8)

/* ===========================================================================
 * 2. 物品图集 sprites/items.png（16 列 × 32 行）
 *    格子号是照图逐格核对出来的（tools/ 取证流程见 DESIGN.md 素材一节）。
 * =========================================================================== */

#define DGITEM_POTION_BLANK   8
#define DGITEM_SCROLL         16
#define DGITEM_SCROLL_ALT     17
#define DGITEM_GOLD           18
#define DGITEM_GEM            19
#define DGITEM_SKULL_MARK     32
#define DGITEM_CHEST          36     /* +0 木 / +1 铜 / +2 银 / +3 金（按章节） */
#define DGITEM_KEY            56
#define DGITEM_KEY_SILVER     57
#define DGITEM_KEY_GOLD       58

/* 近战武器：80~111 是刀刃区，按「越靠后越大件」取 5 档 */
#define DGITEM_WEP_T1         80
#define DGITEM_WEP_T2         84
#define DGITEM_WEP_T3         90
#define DGITEM_WEP_T4         96
#define DGITEM_WEP_T5         103
#define DGITEM_WEP_DAGGER     85

/* 护甲：176 起是胸甲区，5 档正好对应 5 章节 */
#define DGITEM_ARMOR_T1       176
#define DGITEM_ARMOR_T2       177
#define DGITEM_ARMOR_T3       178
#define DGITEM_ARMOR_T4       179
#define DGITEM_ARMOR_T5       180

/* 戒指 336 起；药水（带色液）352 起 12 色；法杖 425 起；食物 432 起 */
#define DGITEM_RING           336
#define DGITEM_POTION         352
#define DGITEM_WAND           425
#define DGITEM_FOOD           432
#define DGITEM_FOOD_PASTY     438
#define DGITEM_AMULET         450

/* ===========================================================================
 * 3. UI 图集 interfaces/icons.png（256×128，16 列 × 8 行）
 *    索引已对照真图集逐格核验（见 shots/icons_grid.png 切格图）。
 * =========================================================================== */

#define DGICON_GOLD           1   /* 金币堆 */
#define DGICON_SWORD          3   /* 蓝剑框（装备） */
#define DGICON_SWAP           5   /* 绿↔红交换箭头 */
#define DGICON_BOOK           8   /* 打开的书 */
#define DGICON_ARROW_RIGHT    16  /* 灰色右箭头 */
#define DGICON_GAMEPAD        22  /* 手柄（等待） */
#define DGICON_LEVELUP        24  /* 上升条图 */
#define DGICON_SCROLL         26  /* 卷轴（菜单） */
#define DGICON_INFO           33  /* 蓝 i */
#define DGICON_WARN           34  /* 黄 !（重要消息） */
#define DGICON_CLOSE          37  /* 红 X */
#define DGICON_PLUS           38  /* 红 + */
#define DGICON_RESTART        39  /* 红循环箭头 */
#define DGICON_TROPHY         41  /* 奖杯 */
#define DGICON_CLIPBOARD      44  /* 剪贴板（保存） */
#define DGICON_CHEST          48  /* 宝箱 */
#define DGICON_STAR           49  /* 黄星 */
#define DGICON_SEARCH         50  /* 放大镜 */
#define DGICON_UPDOWN         52  /* 绿↑红↓（楼层） */
#define DGICON_BAG            53  /* 清单板（背包） */
#define DGICON_SKULL          80  /* 白骷髅 */

#endif /* DG_ICONS_H */
