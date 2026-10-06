/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dungeon_api.h —— 引擎对 UI 层的唯一公开边界（纯 C ABI）
 *
 * 设计原则（架构决策，不可轻改）：
 *   1. 本文件必须是纯 C，允许 .c / .cpp 都能 #include，不含 C++ 类
 *   2. UI 层通过 pull model 从引擎读状态，不感知引擎内部对象图
 *   3. 所有函数在 LVGL 线程（即 sdgoods_lvgl_loop 所在线程）调用，无需加锁
 *   4. 输入路径：UI 事件 → dg_api_on_* → 引擎置 pending → dg_api_tick_if_needed
 *      在下一次 poll 时消费，保证回合制行为确定性
 *   5. 渲染路径：引擎维护 tilemap framebuffer 于 PSRAM，UI 每帧拷到 LVGL img
 *      对象；返回 NULL 表示本帧无变化，UI 可跳过 invalidate 省 CPU
 *
 * 变更本文件视为架构级改动，需同步更新 DESIGN.md §3 §6 §10 与所有调用点。
 */
#ifndef DG_API_H
#define DG_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 常量（与 UI 布局共享，改动需同步） ===== */
#define DG_MAP_W             32     /* 单层地图宽（tile） */
#define DG_MAP_H             32     /* 单层地图高（tile） */
/* 主视窗 352×352：**满圆**设计 —— 360 直径圆屏上能铺满赤道弦的最大 16 整除
 * 正方形（半侧 176 < 半径 180，所以 y=180 一行几乎无黑边），四角超出圆的
 * 部分被物理边框吃掉（不计入可见面积）。v0.3 首版用内接 256×256，实测左右
 * 各留 52px 黑牙，观感“画面小”；改满圆 + HUD 悬浮后可见地图面积 +48%。
 * 22×22 tile 视距靠 FOV 半径限亮，圆内基本全亮。 */
#define DG_VIEWPORT_W        352    /* 主视窗像素尺寸 */
#define DG_VIEWPORT_H        352
#define DG_TILE_PX           16     /* 屏上单 tile 像素 */
#define DG_VIEW_TILE_W       (DG_VIEWPORT_W / DG_TILE_PX)   /* 22 */
#define DG_VIEW_TILE_H       (DG_VIEWPORT_H / DG_TILE_PX)   /* 22 */
#define DG_MAX_INVENTORY     34     /* 背包容量（Shattered 原版 34） */
#define DG_SAVE_SLOTS        3      /* 存档槽位数 */

/* ===== 枚举 ===== */

/* UI 底部悬浮键（圆屏 v0.3 重设：点击移动为主，只留 3 颗真有用键） */
typedef enum {
    DG_BTN_INVENTORY = 0,
    DG_BTN_CAST      = 1,   /* v0.3 暂未接线（法师技能），枚举位保留 */
    DG_BTN_EQUIP     = 2,   /* v0.3 暂未接线（装备栏），枚举位保留 */
    DG_BTN_SEARCH    = 3,
    DG_BTN_WAIT      = 4,
    DG_BTN_MENU      = 5,
    DG_BTN_COUNT     = 6
} dg_btn_id_t;

/* 游戏场景状态机 */
typedef enum {
    DG_SCENE_TITLE = 0,       /* 主菜单（New Game / Continue / Settings） */
    DG_SCENE_CLASS_SELECT,     /* 选职业 */
    DG_SCENE_IN_GAME,          /* 主游戏 */
    DG_SCENE_INVENTORY,        /* 全屏背包 */
    DG_SCENE_GAME_OVER,        /* 死亡结算 */
    DG_SCENE_WIN,              /* 通关 */
} dg_scene_t;

/* 地形类型（对应 Shattered 的 Level.terrain 子集，MVP 版） */
typedef enum {
    DG_TERR_EMPTY = 0,        /* 空气（未生成的墙外） */
    DG_TERR_WALL,             /* 实体墙 */
    DG_TERR_FLOOR,            /* 地板 */
    DG_TERR_DOOR,             /* 门（关） */
    DG_TERR_OPEN_DOOR,        /* 门（开） */
    DG_TERR_EXIT,             /* 下层楼梯 */
    DG_TERR_ENTRY,            /* 上层入口 */
    DG_TERR_WATER,            /* 浅水 */
    DG_TERR_GRASS,            /* 高草（遮蔽） */
    DG_TERR_TRAP,             /* 陷阱（未揭露） */
    DG_TERR_CHEST,            /* 宝箱 */
    DG_TERR_SECRET,           /* 秘密门（未揭露） */
    DG_TERR_STATUE,           /* 装饰柱 */
} dg_terrain_t;

/* 职业（v0.1 只支持战士；其他职业保留枚举位） */
typedef enum {
    DG_CLASS_WARRIOR = 0,
    DG_CLASS_MAGE    = 1,
    DG_CLASS_ROGUE   = 2,
    DG_CLASS_HUNTER  = 3,
    DG_CLASS_COUNT   = 4
} dg_class_t;

/* ===== 生命周期 ===== */

/* app_main 里调一次：初始化 RNG、扫 assets 分区 offset 表、构造 Game 单例。 */
void dg_api_init(void);

/* 开新局：seed 是 32 位无符号；相同 seed 必须生成相同地图（Java Random 决定）。
 * hero_class 见 dg_class_t。 */
void dg_api_new_game(int hero_class, uint32_t seed);

/* 每帧 poll 从 UI 层调；若有 pending 输入或 A* 寻路进度，会推进游戏状态机。 */
void dg_api_tick_if_needed(void);

/* ===== 输入回调（UI 层调用） ===== */

/* 用户点击 tile 坐标（gx, gy），范围 [0, DG_MAP_W) × [0, DG_MAP_H)。
 * 若点击在玩家相邻 8 格 → 走/砍；若点击远处 → A* 寻路；若点击物品格 → 拾取。 */
void dg_api_on_tap(int gx, int gy);

/* 用户点击主视窗像素坐标（px, py，范围 [0,DG_VIEWPORT_W)×[0,DG_VIEWPORT_H)）。
 * 相机左上角 tile 是引擎内部状态（会随英雄移动），故视口像素→tile 的换算
 * 必须由引擎做，UI 层不感知相机 —— v0.2 新增，坐标系见 DESIGN.md §6。
 * 推荐 UI 用本接口，dg_api_on_tap 保留给调试/脚本直推 tile 用。 */
void dg_api_on_viewport_tap(int px, int py);

/* 长按 tile：弹检视卡（不消耗回合）。 */
void dg_api_on_long_press(int gx, int gy);

/* 底部 6 圆键。 */
void dg_api_on_button(dg_btn_id_t btn);

/* ===== 渲染输出（UI 层每帧拉取） ===== */

/* 状态栏文本（写入 buf，返回写入长度；无变更返回 0）。 */
int dg_api_get_status_text(char *buf, int cap);

/* 主视窗 tile framebuffer（RGB565，DG_VIEWPORT_W × DG_VIEWPORT_H）。
 * 返回 NULL 表示本帧未脏，UI 可跳过刷新。out_w / out_h 可传 NULL。 */
const uint16_t *dg_api_get_tilemap_fb(int *out_w, int *out_h);

/* 消息 log 逐行拉取（index 从 0 起，最多返回 3 行；无消息返回 0）。 */
int dg_api_get_message(char *buf, int cap, int index);

/* 背包/状态 overlay 多行文本（v0.3，含 \n，LVGL label 直接消费；
 * buf 建议 ≥192：多行中文模板最坏情况约 150B）。 */
int dg_api_get_stats_text(char *buf, int cap);

/* 英雄 tile 坐标与相机左上 tile（v0.3 圆屏调试注入用：
 * 屏幕像素 = 视口屏幕原点 + (hero - cam) * DG_TILE_PX，屏几何归 UI 层）。 */
void dg_api_get_hero_pos(int *x, int *y);
void dg_api_get_camera(int *x, int *y);

/* 调试导出（cap 建议 ≥160）：一行给出英雄/相机/出口/物品/怪 tile 坐标 + 英雄数值，
 * 形如 "D h=8,8 c=0,0 s=20/20 g=37 d=1F e=12,5 i=7,9|10,14 m=5,6"，
 * 供 PC 脚本定向点击，并做文字层取证（血量 / 金币 / 深度）。 */
int dg_api_debug_dump(char *buf, int cap);

/* ===== 存档槽位 ===== */

/* 检查指定槽位（0..2）是否有存档。 */
bool dg_api_has_save(int slot);

/* 加载 / 保存；slot 越界返回 false。 */
bool dg_api_load_game(int slot);
bool dg_api_save_game(int slot);

/* ===== 场景查询（供 UI 决定显示哪个 overlay） ===== */

dg_scene_t dg_api_current_scene(void);

/* ===== 版本与调试 ===== */

/* 返回 "0.0.1-<MMDDHH>"，UI 可在关于页显示。 */
const char *dg_api_version(void);

/* 引擎自测：跑一遍 Java Random 单元测试 + 关卡生成 sanity check，
 * 结果通过 ESP_LOG 输出；返回通过的测试数（异常则返回 -1）。 */
int  dg_api_run_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* DG_API_H */
