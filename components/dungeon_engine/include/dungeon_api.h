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
 * v0.4 对齐上游的三类新增边界：
 *   A. 场景流：标题 → 选职业 → 12 层 3 章节 → 取护身符通关（dg_api_pick_class 等）
 *   B. 素材透出：UI 直接取烘焙图集的任意格子（dg_api_blit_icon），按钮 / 背包
 *      图标 / 职业头像全部走上游真素材，不再自绘色块与中文字圆键
 *   C. 结构化 HUD + 音效队列：UI 不必解析字符串；引擎把「该播什么声」排进
 *      队列，音频层每帧 pop（DG_SFX_*）
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
#define DG_MAX_DEPTH         12     /* 总局层数（12F 放护身符 = 通关） */
#define DG_CHAPTER_DEPTH     4      /* 每章层数：1-4 下水道 / 5-8 监狱 / 9-12 洞穴 */
/* 引擎侧只渲染「逻辑像素」：1 tile = DG_TILE_PX(16) 逻辑 px，视口 11×11 tile
 * = 176×176。UI 层按 DG_VIEW_SCALE 整数倍最近邻放大到 352×352 满圆画布。
 * 为什么要在 UI 侧放大：1.85 寸圆屏上 16px 格子小到看不清（用户反馈「画面好
 * 小」），2× 后 tile 32px 才 readable；而缩放是显示层的事，引擎的 tile 几何、
 * 素材索引、点击换算全部保持 16px 基准不变。像素风 1:1 复制，不插值。 */
#define DG_TILE_PX           16     /* 逻辑单 tile 像素（引擎渲染基准） */
#define DG_VIEW_SCALE        2      /* UI 显示倍率：176 逻辑 → 352 满圆 */
#define DG_VIEW_TILE_W       11     /* 视口 tile 数（≈ Shattered 手机版视距）*/
#define DG_VIEW_TILE_H       11
#define DG_VIEWPORT_W        (DG_VIEW_TILE_W * DG_TILE_PX)   /* 176 逻辑 px */
#define DG_VIEWPORT_H        (DG_VIEW_TILE_H * DG_TILE_PX)
#define DG_MAX_INVENTORY     34     /* 背包容量（Shattered 原版 34） */
#define DG_SAVE_SLOTS        3      /* 存档槽位数 */
#define DG_ICON_CELL         16     /* 图集格边长（上游 items/icons 均 16×16） */

/* ===== 枚举 ===== */

/* UI 悬浮控件。v0.4 语义重排（值变了，UI 与引擎必须同步编译）：
 *   DESCEND 取代旧 CAST —— 站在楼梯上时高亮，是「继续推进」的主路径按钮；
 *   MENU    在 v0.4 是真菜单（存/读/回标题），不再是死键。 */
typedef enum {
    DG_BTN_INVENTORY = 0,
    DG_BTN_DESCEND   = 1,   /* 下楼 / 进入下一层 */
    DG_BTN_EQUIP     = 2,   /* 快速装备背包第一件可用装备 */
    DG_BTN_SEARCH    = 3,
    DG_BTN_WAIT      = 4,
    DG_BTN_MENU      = 5,
    DG_BTN_COUNT     = 6
} dg_btn_id_t;

/* 游戏场景状态机（v0.4 全流程接通，UI 按 scene 切页面） */
typedef enum {
    DG_SCENE_TITLE = 0,       /* 标题：继续游戏 / 新冒险 */
    DG_SCENE_CLASS_SELECT,     /* 选职业（4 张上游英雄帧 + 简介） */
    DG_SCENE_IN_GAME,          /* 主游戏 */
    DG_SCENE_INVENTORY,        /* 全屏背包 */
    DG_SCENE_MENU,             /* 系统菜单（存 / 读 / 回标题） */
    DG_SCENE_GAME_OVER,        /* 死亡结算 */
    DG_SCENE_WIN,              /* 通关（取得护身符） */
    DG_SCENE_COUNT
} dg_scene_t;

/* 地形类型（对应 Shattered 的 Level.terrain 子集）。
 * v0.4 加了 LOCKED_DOOR / HIGH_GRASS / PEDESTAL —— 门要钥匙、高草能藏、
 * 基座托护身符，都是从上游手感里必须补齐的三件。 */
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
    DG_TERR_TRAP,             /* 陷阱（已揭露；未揭露时是 FLOOR） */
    DG_TERR_CHEST,            /* 宝箱 */
    DG_TERR_SECRET,           /* 秘密门（未揭露） */
    DG_TERR_STATUE,           /* 装饰柱 */
    DG_TERR_LOCKED_DOOR,      /* 上锁的门（需钥匙） */
    DG_TERR_HIGH_GRASS,       /* 茂草（比 GRASS 更挡视线） */
    DG_TERR_PEDESTAL,         /* 基座（放护身符） */
    DG_TERR_COUNT
} dg_terrain_t;

/* 职业（v0.4 四职业全部可选，初始装备与数值各自不同） */
typedef enum {
    DG_CLASS_WARRIOR = 0,
    DG_CLASS_MAGE    = 1,
    DG_CLASS_ROGUE   = 2,
    DG_CLASS_HUNTER  = 3,
    DG_CLASS_COUNT   = 4
} dg_class_t;

/* 物品大类（顺序必须与引擎 Item::Kind 一致，UI 用来分格渲染与配色） */
typedef enum {
    DG_IT_WEAPON = 0,
    DG_IT_ARMOR,
    DG_IT_POTION,
    DG_IT_SCROLL,
    DG_IT_RING,
    DG_IT_WAND,
    DG_IT_FOOD,
    DG_IT_KEY,
    DG_IT_GOLD,
    DG_IT_AMULET,
    DG_IT_COUNT
} dg_item_kind_t;

/* 音效 ID：引擎侧只排「发生了什么」，波形由 main/game/dg_audio.c 合成。
 * 与上游 mp3 资产解耦（设备无解码器 / 分区放不下 18MB），是程序化拟音。 */
typedef enum {
    DG_SFX_NONE = 0,
    DG_SFX_STEP,        /* 走路 */
    DG_SFX_HIT,         /* 我砍中 */
    DG_SFX_MISS,        /* 我砍空 */
    DG_SFX_HURT,        /* 我挨打 */
    DG_SFX_KILL,        /* 击杀 */
    DG_SFX_DIE,         /* 我死 */
    DG_SFX_PICKUP,      /* 拾取装备 */
    DG_SFX_GOLD,        /* 拾金 */
    DG_SFX_LEVELUP,     /* 升级 */
    DG_SFX_DRINK,       /* 喝药水 */
    DG_SFX_SCROLL,      /* 读卷轴 */
    DG_SFX_DOOR,        /* 开门 */
    DG_SFX_LOCKED,      /* 门锁着 */
    DG_SFX_STAIRS,      /* 下楼 */
    DG_SFX_TRAP,        /* 踩陷阱 */
    DG_SFX_CHEST,       /* 开箱 */
    DG_SFX_SELECT,      /* 菜单选择 */
    DG_SFX_ERROR,       /* 无效操作 */
    DG_SFX_WIN,         /* 通关 */
    DG_SFX_ZAP,         /* 法杖 / 远程 */
    DG_SFX_COUNT
} dg_sfx_id_t;

/* 图集句柄：UI 取素材只走这里（与引擎内 gfx::Sheet 数值解耦，避免 C++ 枚举
 * 泄漏到 C 边界）。v0.4 UI 需要：物品图标、UI 图标、4 个职业头像。 */
typedef enum {
    DG_SHEET_ITEMS = 0,      /* sprites/items.png，16 列 */
    DG_SHEET_ICONS = 1,      /* interfaces/icons.png，16 列 */
    DG_SHEET_HERO_WARRIOR = 2,
    DG_SHEET_HERO_MAGE    = 3,
    DG_SHEET_HERO_ROGUE   = 4,
    DG_SHEET_HERO_HUNTRESS = 5,
    DG_SHEET_COUNT
} dg_sheet_t;

/* ===== 结构化数据（UI 不再解析字符串） ===== */

/* HUD：圆屏顶栏的血条 / 经验条 / 深度 / 金币 / 饥饿全部由此驱动 */
typedef struct {
    int16_t hp, hp_max;
    int16_t exp, exp_max;
    int16_t gold;
    int16_t str;
    int16_t energy, energy_max;   /* 饥饿（0 = 饿到掉血） */
    int16_t depth;                /* 1 基 */
    int16_t chapter;              /* 0 基：0 下水道 1 监狱 2 洞穴 */
    int16_t lvl;
    int16_t cls;
    uint8_t has_amulet;
    uint8_t on_stairs;            /* 站在 EXIT 上 → DESCEND 键高亮 */
    uint8_t keys;                 /* 钥匙数 */
    uint8_t hunger_state;         /* 0 饱 1 微饿 2 饿 3 快饿死 */
} dg_hud_t;

/* 背包格：UI 画图标网格 + 详情行 */
typedef struct {
    int16_t kind;                 /* dg_item_kind_t */
    int16_t sub;                  /* 同大类内的变体号（药水类型等） */
    int16_t qty;
    int16_t icon;                 /* items.png 格子号 */
    int16_t tier;                 /* 装备档位 1..5，非装备为 0 */
    int16_t str_req;              /* 力量需求，0 = 无 */
    uint8_t equipped;             /* 0 不在身上 1 武器 2 护甲 3 戒指 */
    uint8_t cursed;
    const char *name;             /* UTF-8 中文名（引擎静态字面量，UI 只读） */
} dg_item_info_t;

/* 存档槽位摘要（标题页列表用） */
typedef struct {
    uint8_t valid;
    int16_t hero_class;
    int16_t depth;
    int32_t gold;
    uint32_t game_time;
} dg_save_info_t;

/* ===== 生命周期 ===== */

/* app_main 里调一次：初始化 RNG、扫 assets 分区 offset 表、构造 Game 单例。 */
void dg_api_init(void);

/* 开新局：seed 是 32 位无符号；相同 seed 必须生成相同地图（Java Random 决定）。
 * hero_class 见 dg_class_t。 */
void dg_api_new_game(int hero_class, uint32_t seed);

/* 每帧 poll 从 UI 层调；若有 pending 输入或 A* 寻路进度，会推进游戏状态机。
 * 同时驱动动画时钟（精灵 idle、平滑移动、飘字、闪白）。 */
void dg_api_tick_if_needed(void);

/* ===== 输入回调（UI 层调用） ===== */

/* 用户点击 tile 坐标（gx, gy），范围 [0, DG_MAP_W) × [0, DG_MAP_H)。
 * 若点击在玩家相邻 8 格 → 走/砍；若点击远处 → A* 寻路；若点击物品格 → 拾取。 */
void dg_api_on_tap(int gx, int gy);

/* 用户点击主视窗像素坐标（**逻辑像素**，范围 [0,DG_VIEWPORT_W)×[0,DG_VIEWPORT_H)）。
 * 相机左上角 tile 是引擎内部状态（会随英雄移动），故视口像素→tile 的换算
 * 必须由引擎做，UI 层不感知相机。
 * 注意 UI 传入前要把屏幕物理 px 除以 DG_VIEW_SCALE 换回逻辑 px。 */
void dg_api_on_viewport_tap(int px, int py);

/* 长按 tile：弹检视卡（不消耗回合）。 */
void dg_api_on_long_press(int gx, int gy);

/* 功能键（见 dg_btn_id_t）。 */
void dg_api_on_button(dg_btn_id_t btn);

/* 方向键逐格移动（M1 圆屏操控）：dx,dy ∈ {-1,0,1}，一次一步（回合制）。
 * 目标格有敌人则攻击、是门/箱/陷阱/物品则按 hero_try_step 的既有语义处理；
 * 走不动（墙/锁门无钥匙）返回 false 且不消耗回合。 */
bool dg_api_step(int dx, int dy);

/* ===== 场景流（v0.4 标题 / 选职业 / 菜单） ===== */

/* 回标题页（会丢弃当前局；UI 侧应先确认）。 */
void dg_api_goto_title(void);

/* 进选职业页。 */
void dg_api_goto_class_select(void);

/* 选定职业并立刻开新局（引擎内部取一个真随机 seed）。 */
void dg_api_pick_class(int hero_class);

/* 系统菜单动作：0=保存当前槽 1=读取 2=回标题 3=新游戏（重开一局同职业）。
 * slot 仅对 SAVE/LOAD 有效。 */
void dg_api_menu_action(int action, int slot);

/* 当前选中职业（选职业页高亮用）。 */
int dg_api_selected_class(void);

/* ===== 渲染输出（UI 层每帧拉取） ===== */

/* 状态栏文本（写入 buf，返回写入长度；无变更返回 0）。
 * v0.4 起 HUD 主要走 dg_api_get_hud，本函数保留给消息行/调试。 */
int dg_api_get_status_text(char *buf, int cap);

/* 主视窗 tile framebuffer（RGB565，DG_VIEWPORT_W × DG_VIEWPORT_H）。
 * 返回 NULL 表示本帧未脏，UI 可跳过刷新。out_w / out_h 可传 NULL。 */
const uint16_t *dg_api_get_tilemap_fb(int *out_w, int *out_h);

/* 消息 log 逐行拉取（index 从 0 起，最多返回 3 行；无消息返回 0）。 */
int dg_api_get_message(char *buf, int cap, int index);

/* 背包/状态 overlay 多行文本（含 \n，LVGL label 直接消费；buf 建议 ≥256）。 */
int dg_api_get_stats_text(char *buf, int cap);

/* 英雄 tile 坐标与相机左上 tile（圆屏调试注入用：
 * 屏幕像素 = 视口屏幕原点 + (hero - cam) * DG_TILE_PX，屏几何归 UI 层）。 */
void dg_api_get_hero_pos(int *x, int *y);
void dg_api_get_camera(int *x, int *y);

/* 调试导出（cap 建议 ≥192）：一行给出英雄/相机/出口/物品/怪 tile 坐标 + 英雄数值，
 * 供 PC 脚本定向点击，并做文字层取证（血量 / 金币 / 深度）。 */
int dg_api_debug_dump(char *buf, int cap);

/* 调试：把英雄当前生效的 buff 写成 "type:剩余回合 " 序列（供取证 'v' 旁路行）。
 * 例 "haste:12 invis:8 "；无 buff 返回 0。 */
int dg_api_hero_buffs(char *buf, int cap);

/* 调试（仅取证用）：给英雄施加一个 buff。type 取引擎 Buff::Type 数值
 * （1 slow 2 haste 3 invis 6 poison 7 burning…），duration 回合。 */
void dg_api_debug_buff(int type, int duration);

/* 调试（仅取证用）：M3 怪物专属 AI + 首领机制自检——直接跑 Goo 愈合 / 首领召唤 /
 * 野兽人狂暴 / 萨满瞬移 / 蜘蛛结网 关键分支，逐项 ESP_LOGI 打 "M3 ... PASS/FAIL"。 */
void dg_api_debug_m3(void);

/* 调试（仅取证用）：M4 物品全谱自检——鉴定/经验/狂暴/恐惧/沉睡/法杖/戒指/附魔 关键分支，
 * 逐项 ESP_LOGI 打 "M4 ... PASS/FAIL"。 */
void dg_api_debug_m4(void);

/* ===== 素材透出（v0.4：UI 按钮 / 图标全走上游图集） ===== */

/* 素材是否可用（assets 分区烧了烘焙图）。false 时 UI 应回退纯色 + 文字。 */
bool dg_api_assets_ready(void);

/* 把图集一个矩形区域按 scale 倍最近邻拷进 dst（native RGB565）。
 *   sheet    : dg_sheet_t
 *   sx, sy   : 源像素坐标（不是格子号，UI 自己算 DG_CELL_X/Y，见 dungeon_api 注释）
 *   w, h     : 源尺寸（逻辑 px，通常 16）
 *   dst      : 目标缓冲，行宽 dst_pitch（像素），尺寸 dst_w × dst_h
 *   scale    : 1 / 2 / 3 …（目标占 w*scale × h*scale）
 *   key_skip : true = 遇透明色键不写（精灵/图标叠底）；false = 整块覆盖
 * 越界像素丢弃，dst 越界部分不动。返回 false = 图集缺失。 */
bool dg_api_blit_icon(int sheet, int sx, int sy, int w, int h,
                      uint16_t *dst, int dst_pitch, int dst_w, int dst_h,
                      int scale, bool key_skip);

/* ===== HUD / 背包 ===== */

bool dg_api_get_hud(dg_hud_t *out);

int  dg_api_inv_count(void);
bool dg_api_inv_get(int slot, dg_item_info_t *out);
/* 使用 / 装备 / 丢弃背包第 slot 件（返回是否真的执行了，UI 据此播选择音）。 */
bool dg_api_inv_use(int slot);
bool dg_api_inv_equip(int slot);
bool dg_api_inv_drop(int slot);
/* 装备位摘要：1=有武器 2=有护甲 4=有戒指（UI 在背包页画「已装备」角标）。 */
int  dg_api_equip_mask(void);

/* ===== 音效队列 ===== */

/* 引擎排声 → UI/音频层每帧取走。返回 true 表示取出一个 id。
 * 队列满时引擎会丢弃最旧的一条，绝不阻塞。 */
bool dg_api_pop_sfx(int *id);

/* ===== 存档槽位 ===== */

/* 检查指定槽位（0..2）是否有存档。 */
bool dg_api_has_save(int slot);

/* 加载 / 保存；slot 越界返回 false。 */
bool dg_api_load_game(int slot);
bool dg_api_save_game(int slot);
bool dg_api_delete_save(int slot);

/* 槽位摘要（标题页显示 "1F 战士 30 回合"）。无存档返回 false。 */
bool dg_api_save_info(int slot, dg_save_info_t *out);

/* ===== 场景查询（供 UI 决定显示哪个 overlay） ===== */

dg_scene_t dg_api_current_scene(void);

/* ===== 版本与调试 ===== */

/* 返回 "0.4.0-upstream"，UI 可在关于页显示。 */
const char *dg_api_version(void);

/* 引擎自测：跑一遍 Java Random 单元测试 + 关卡生成 sanity check，
 * 结果通过 ESP_LOG 输出；返回通过的测试数（异常则返回 -1）。 */
int  dg_api_run_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* DG_API_H */
