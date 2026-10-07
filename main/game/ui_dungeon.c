/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * ui_dungeon.c —— LVGL 首屏装配（v0.4 上游对齐版）
 *
 * 与 v0.3b 的差别（用户指令：素材 / 按钮 / 交互全部对齐上游，像真正的游戏）：
 *   1. 场景流驱动：引擎 scene（标题 / 选职业 / 游戏中 / 背包 / 菜单 / 死亡 /
 *      通关）各对应一层 LVGL 容器，show_scene() 只切显隐，不再开机即 new_game。
 *   2. 真素材：所有按钮图标、背包物品、职业头像都经 dg_api_blit_icon 从上游
 *      烘焙图集（items.png / icons.png / heroes）取格，不再自绘中文圆键色块。
 *   3. 结构化 HUD：血条 / 经验条 / 能量用 lv_bar，深度 / 金币 / 层数走
 *      dg_api_get_hud，不再解析状态字符串。
 *   4. 触屏为主：点地图即走 / 砍 / 捡（A* 由引擎做），底部圆弧一排上游图标键
 *      （包 / 待 / 搜 / 装 / 下 / 菜）取代原左下十字键 —— 上游就是 tap-to-move。
 *   5. 音效接线：每帧 dg_api_pop_sfx → dg_audio_play_sfx；开屏 dg_audio_start。
 *
 * 圆屏红线（SDG_UI_SAFE_*）：任何可点控件矩形必须完整落在圆内（屏心 (180,180)
 * 半径 180）。顶部 y<50 是控制中心下滑捕获带，只放只读 HUD，不放可点元素。
 *
 * 与引擎的边界：本文件只 #include "dungeon_api.h"（C ABI）+ "dg_audio.h"，
 * 不 #include 任何 C++ 头；图集格索引在本文件镜像一份（dg_icons.h 属引擎内部）。
 */

#include "ui_dungeon.h"
#include "dg_audio.h"
#include "dungeon_api.h"          /* 引擎 C ABI 边界 */
#include "sdgoods_tap.h"          /* sdgoods_tap_synth：串口调试注入（ext_cmd） */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 平台中文子集字体（bsp/fonts，tools/gen_fonts.py 生成）。新增文案缺字自动
 * 回退 cn_font 全量集，重跑 gen_fonts.py 即补齐。 */
LV_FONT_DECLARE(si_yuan_black_icon_14);

static const char *TAG = "ui_dungeon";

/* ---- 圆屏几何常量（360 直径，屏心 180,180 半径 180）---- */
#define DG_SCREEN_W        360
#define DG_SCREEN_H        360
#define DG_VIEW_LOG        DG_VIEWPORT_W              /* 176 引擎逻辑 fb 边长 */
#define DG_VIEW_SIZE       (DG_VIEW_LOG * DG_VIEW_SCALE)  /* 352 满圆画布 */
#define DG_VP_ORIGIN       ((DG_SCREEN_W - DG_VIEW_SIZE) / 2)  /* 4 */
#define DG_ICON_PX         DG_ICON_CELL               /* 16 图集格边长 */

/* ---- 上游 interfaces/icons.png 格索引（对照真图集逐格核验，镜像 dg_icons.h §3）---- */
#define IC_GOLD        1    /* 金币堆 */
#define IC_SWORD       3    /* 蓝剑框（装备 / 新冒险） */
#define IC_SWAP        5    /* 绿↔红交换箭头（丢弃） */
#define IC_BOOK        8    /* 打开的书（读取） */
#define IC_ARROW       16   /* 灰色右箭头 */
#define IC_GAMEPAD     22   /* 手柄（等待） */
#define IC_LEVELUP     24   /* 上升条图（升级） */
#define IC_SCROLL      26   /* 卷轴（菜单） */
#define IC_INFO        33   /* 蓝 i */
#define IC_WARN        34   /* 黄 ! */
#define IC_CLOSE       37   /* 红 X */
#define IC_PLUS        38   /* 红 +（使用） */
#define IC_RESTART     39   /* 红循环箭头（继续 / 再来 / 回标题） */
#define IC_TROPHY      41   /* 奖杯 */
#define IC_CLIPBOARD   44   /* 剪贴板（保存） */
#define IC_CHEST       48   /* 宝箱 */
#define IC_STAR        49   /* 黄星 */
#define IC_SEARCH      50   /* 放大镜 */
#define IC_UPDOWN      52   /* 绿↑红↓（下楼梯） */
#define IC_BAG         53   /* 清单板（背包） */
#define IC_SKULL       80   /* 白骷髅 */
/* 图集格子号 → 源像素（16 列 16px） */
#define CELL_X(idx)    (((idx) % 16) * 16)
#define CELL_Y(idx)    (((idx) / 16) * 16)

/* 上游职业头像图集句柄 + 精灵首格（idle 帧在 (16,0)，取 16×16 当头像） */
#define HERO_AVATAR_SX 16
#define HERO_AVATAR_SY 0

/* ---- 原生 RGB565（未交换，与 gfx / blit 同域）；显示前统一 bswap ---- */
#define NAT565(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define BTN_BG          NAT565(26, 34, 48)     /* 圆弧键底 */
#define PANEL_BG        NAT565(14, 14, 18)     /* 面板底 */

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* ==================== LVGL 对象句柄 ==================== */
static lv_obj_t *s_scr = NULL;

/* 游戏中层 */
static lv_obj_t *s_g_layer    = NULL;   /* 视口容器 */
static lv_obj_t *s_viewport   = NULL;
static lv_obj_t *s_toolbar    = NULL;   /* 底部圆弧键容器 */
static lv_obj_t *s_hud        = NULL;   /* 顶部 HUD 容器 */
static lv_obj_t *s_hp_bar     = NULL;
static lv_obj_t *s_xp_bar     = NULL;
static lv_obj_t *s_en_bar     = NULL;
static lv_obj_t *s_depth_lbl  = NULL;
static lv_obj_t *s_gold_lbl   = NULL;
static lv_obj_t *s_lvl_lbl    = NULL;
static lv_obj_t *s_msg_lbl    = NULL;
static lv_obj_t *s_descend_btn = NULL;  /* 楼梯高亮用 */
/* M1 瞬时消息 toast：新消息出现时显示，TOAST_MS 后自动淡隐，不再常驻盖图。 */
#define DG_TOAST_MS 2800
static uint32_t s_msg_expire_ms = 0;
static char     s_msg_last[96]  = {0};

/* 独立页面 */
static lv_obj_t *s_title_layer = NULL;
static lv_obj_t *s_class_layer = NULL;
static lv_obj_t *s_inv_layer   = NULL;
static lv_obj_t *s_menu_layer  = NULL;
static lv_obj_t *s_over_layer  = NULL;
static lv_obj_t *s_win_layer   = NULL;

/* 背包网格（34 格固定复用） */
typedef struct {
    lv_obj_t      *img;
    lv_img_dsc_t   dsc;
    uint16_t      *buf;          /* 32×32 scale2 */
} inv_cell_t;
static inv_cell_t s_inv[DG_MAX_INVENTORY];
static lv_obj_t  *s_inv_detail   = NULL;
static int        s_inv_sel      = -1;

static bool s_running = false;
static dg_scene_t s_shown_scene = DG_SCENE_TITLE;
static uint32_t s_last_hud_ms = 0;

/* 视口显示缓冲（物理 352×352）：引擎 fb 是逻辑 176×176 native 小端，本层
 * 负责 2× 最近邻放大 + 字节序交换（LV_COLOR_16_SWAP=1）。 */
static uint16_t *s_display_fb = NULL;
static lv_img_dsc_t s_viewport_dsc = {
    .header.cf         = LV_IMG_CF_TRUE_COLOR,
    .header.always_zero = 0,
    .header.w          = DG_VIEW_SIZE,
    .header.h          = DG_VIEW_SIZE,
    .data_size         = DG_VIEW_SIZE * DG_VIEW_SIZE * sizeof(uint16_t),
    .data              = NULL,
};

/* ==================== 前向声明 ==================== */
static void show_scene(dg_scene_t scene);
static void on_viewport_click_cb(lv_event_t *e);
static void on_viewport_long_cb(lv_event_t *e);
static void on_toolbar_cb(lv_event_t *e);
static void on_title_new_cb(lv_event_t *e);
static void on_title_load_cb(lv_event_t *e);
static void on_class_cb(lv_event_t *e);
static void on_class_back_cb(lv_event_t *e);
static void on_inv_cell_cb(lv_event_t *e);
static void on_inv_action_cb(lv_event_t *e);
static void on_inv_close_cb(lv_event_t *e);
static void on_menu_cb(lv_event_t *e);
static void on_end_cb(lv_event_t *e);
static void refresh_inventory(void);

/* ==================== 素材图标工具 ==================== */

/* 把图集一格 blit 进 buf（side×side，scale 最近邻），叠在 bg 上，再整块 bswap
 * 成显示字节序。buf 须已分配 side*side*2 字节。key_skip=true 保留 bg 透明处。 */
static void fill_icon_buf(int sheet, int cell, uint16_t *buf, int scale, uint16_t bg)
{
    int side = DG_ICON_PX * scale;
    for (int i = 0; i < side * side; i++) buf[i] = bg;
    dg_api_blit_icon(sheet, CELL_X(cell), CELL_Y(cell), DG_ICON_PX, DG_ICON_PX,
                     buf, side, side, side, scale, true);
    for (int i = 0; i < side * side; i++) buf[i] = bswap16(buf[i]);
}

/* 建一个图标 lv_img（常驻 PSRAM 缓冲，静态图标用）。scale=源格放大倍率。 */
static lv_obj_t *icon_img_create(lv_obj_t *parent, int sheet, int cell,
                                 int scale, uint16_t bg)
{
    int side = DG_ICON_PX * scale;
    size_t nbytes = (size_t)side * side * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM);
    lv_img_dsc_t *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    if (!buf || !d) return NULL;
    fill_icon_buf(sheet, cell, buf, scale, bg);
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.always_zero = 0;
    d->header.w = side;
    d->header.h = side;
    d->data_size = nbytes;
    d->data = (const uint8_t *)buf;
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, d);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

/* 圆形上游图标按钮：不透色底 + 内嵌 icon（缩到能进圆），点按回调带 user_data。 */
static lv_obj_t *icon_btn_create(lv_obj_t *parent, int sheet, int cell,
                                 int btn_d, lv_event_cb_t cb, int id,
                                 uint16_t bg)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, btn_d, btn_d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1a2230), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(0x556072), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(b, 4);
    /* 底换色由 bg 参数决定：直接用 bg 作 icon 叠底，圆内不露方块角 */
    int icon_scale = 2;                       /* 32px 进 btn_d>=48 圆 */
    lv_obj_t *ic = icon_img_create(b, sheet, cell, icon_scale, bg);
    if (ic) lv_obj_center(ic);
    (void)btn_d;
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    return b;
}

/* ==================== 页面构建 ==================== */

/* 一个铺满圆的容器（透明，仅作层管理与显隐）。 */
static lv_obj_t *layer_create(void)
{
    lv_obj_t *l = lv_obj_create(s_scr);
    lv_obj_set_size(l, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(l, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(l, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(l, 0, LV_PART_MAIN);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static void hud_build(lv_obj_t *parent)
{
    s_hud = parent;
    /* 顶部只读带，居中半透胶囊。圆屏适配：胶囊不能贴在 y=12——那里圆形边框
     * 半弦只有 ~129px，150 宽的胶囊两角会被物理裁掉。下移到 y=20（该处
     * 半弦 ~165，角距圆心 ~177<180）保证完整落进圆内。 */
    lv_obj_t *cap = lv_obj_create(parent);
    lv_obj_set_size(cap, 150, 46);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_radius(cap, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cap, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cap, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(cap, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cap, 4, LV_PART_MAIN);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);

    /* 第一行：深度 + 等级 + 金币 */
    s_depth_lbl = lv_label_create(cap);
    lv_label_set_text(s_depth_lbl, "1F");
    lv_obj_set_style_text_font(s_depth_lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_depth_lbl, lv_color_hex(0xe8e0b0), LV_PART_MAIN);
    lv_obj_align(s_depth_lbl, LV_ALIGN_TOP_LEFT, 2, 0);

    s_lvl_lbl = lv_label_create(cap);
    lv_label_set_text(s_lvl_lbl, "Lv1");
    lv_obj_set_style_text_font(s_lvl_lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lvl_lbl, lv_color_hex(0x9ad07a), LV_PART_MAIN);
    lv_obj_align(s_lvl_lbl, LV_ALIGN_TOP_MID, 0, 0);

    s_gold_lbl = lv_label_create(cap);
    lv_label_set_text(s_gold_lbl, "0");
    lv_obj_set_style_text_font(s_gold_lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_gold_lbl, lv_color_hex(0xf0d060), LV_PART_MAIN);
    lv_obj_align(s_gold_lbl, LV_ALIGN_TOP_RIGHT, -2, 0);

    /* 血条 / 经验条 / 能量条（细） */
    s_hp_bar = lv_bar_create(cap);
    lv_obj_set_size(s_hp_bar, 138, 9);
    lv_obj_align(s_hp_bar, LV_ALIGN_BOTTOM_LEFT, 2, -12);
    lv_bar_set_range(s_hp_bar, 0, 100);
    lv_bar_set_value(s_hp_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_hp_bar, lv_color_hex(0x3a1414), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hp_bar, lv_color_hex(0xd0403a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hp_bar, lv_color_hex(0x14181c), LV_PART_INDICATOR | LV_PART_MAIN);
    lv_obj_clear_flag(s_hp_bar, LV_OBJ_FLAG_CLICKABLE);

    s_xp_bar = lv_bar_create(cap);
    lv_obj_set_size(s_xp_bar, 138, 5);
    lv_obj_align(s_xp_bar, LV_ALIGN_BOTTOM_LEFT, 2, -4);
    lv_bar_set_range(s_xp_bar, 0, 100);
    lv_bar_set_value(s_xp_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_xp_bar, lv_color_hex(0x1a1a28), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_xp_bar, lv_color_hex(0x5a7ad0), LV_PART_MAIN);
    lv_obj_clear_flag(s_xp_bar, LV_OBJ_FLAG_CLICKABLE);

    s_en_bar = lv_bar_create(cap);
    lv_obj_set_size(s_en_bar, 40, 5);
    lv_obj_align(s_en_bar, LV_ALIGN_BOTTOM_RIGHT, -2, -4);
    lv_bar_set_range(s_en_bar, 0, 100);
    lv_bar_set_value(s_en_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_en_bar, lv_color_hex(0x241c10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_en_bar, lv_color_hex(0xd0a040), LV_PART_MAIN);
    lv_obj_clear_flag(s_en_bar, LV_OBJ_FLAG_CLICKABLE);

    /* 消息 toast（胶囊下沿，只读，仍在圆内）：初始隐藏，有新消息时闪现并自动淡隐 */
    s_msg_lbl = lv_label_create(parent);
    lv_label_set_text(s_msg_lbl, "");
    lv_obj_set_style_text_font(s_msg_lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_msg_lbl, lv_color_hex(0xd8d0a0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_msg_lbl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg_lbl, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg_lbl, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg_lbl, 3, LV_PART_MAIN);
    lv_obj_set_width(s_msg_lbl, 210);
    lv_obj_set_style_text_align(s_msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_msg_lbl, LV_ALIGN_TOP_MID, 0, 72);
}

/* ---- M1 圆屏操控：半透明方向键（lv_line 箭头）+ 精简动作键 ----
 * 依据同硬件 DOOM 已验证规范：地图保持满圆可见，操控件做成半透明白色
 * 悬浮键、放在圆盘边缘裁切区（y≥190），中心英雄区不遮挡；方向键解决
 * 「小屏点格子不准」，tap-to-move 降为辅助（上半屏无遮挡仍可点）。 */

/* 方向键箭头：0 上 1 右 2 下 3 左。lv_line 的点是对象内坐标，需常驻。 */
static lv_point_t s_chev[4][3] = {
    { {4,15},{12,7},{20,15} },   /* 上 ^ */
    { {9,4},{17,12},{9,20} },    /* 右 > */
    { {4,9},{12,17},{20,9} },    /* 下 v */
    { {15,4},{7,12},{15,20} },   /* 左 < */
};

static void on_dpad_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    static const int8_t dxy[4][2] = { {0,-1},{1,0},{0,1},{-1,0} };
    dg_api_step(dxy[dir][0], dxy[dir][1]);
}

/* 半透明圆形幽灵键（白色描边 + 淡白底），落在圆盘边缘裁切区 */
static lv_obj_t *ghost_btn_create(lv_obj_t *parent, int cx, int cy, int d,
                                  lv_event_cb_t cb, int id)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(b, 6);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    return b;
}

static void toolbar_build(lv_obj_t *parent)
{
    s_toolbar = parent;

    /* 左下：4 向方向键（逐格移动 / 朝该方向攻击），键心距 44，中心透图 */
    const int dcx = 108, dcy = 248, arm = 44;
    static const int8_t arm_pos[4][2] = {
        { 0, -arm }, { arm, 0 }, { 0, arm }, { -arm, 0 } };   /* 上右下左 */
    for (int dir = 0; dir < 4; dir++) {
        int cx = dcx + arm_pos[dir][0], cy = dcy + arm_pos[dir][1];
        lv_obj_t *b = ghost_btn_create(parent, cx, cy, 44, on_dpad_cb, dir);
        lv_obj_t *ln = lv_line_create(b);
        lv_line_set_points(ln, s_chev[dir], 3);
        lv_obj_set_style_line_color(ln, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_line_width(ln, 3, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(ln, true, LV_PART_MAIN);
        lv_obj_set_size(ln, 24, 24);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(ln);
    }

    /* 右下：4 个动作键（走上游图标），全部落在圆内；装备收进背包，不再占键位 */
    static const struct { int sheet, cell, btn, cx, cy; } acts[] = {
        { DG_SHEET_ICONS, IC_GAMEPAD, DG_BTN_WAIT,      216, 210 },
        { DG_SHEET_ICONS, IC_SEARCH,  DG_BTN_SEARCH,    258, 246 },
        { DG_SHEET_ICONS, IC_BAG,     DG_BTN_INVENTORY, 240, 292 },
        { DG_SHEET_ICONS, IC_SCROLL,  DG_BTN_MENU,      196, 296 },
    };
    for (unsigned i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        lv_obj_t *b = icon_btn_create(parent, acts[i].sheet, acts[i].cell, 44,
                                      on_toolbar_cb, acts[i].btn, BTN_BG);
        lv_obj_set_pos(b, acts[i].cx - 22, acts[i].cy - 22);
    }

    /* 下楼键：站在出口才显示，底部正中（D-pad 下键与背包键之间） */
    s_descend_btn = icon_btn_create(parent, DG_SHEET_ICONS, IC_UPDOWN, 46,
                                    on_toolbar_cb, DG_BTN_DESCEND, BTN_BG);
    lv_obj_set_pos(s_descend_btn, 180 - 23, 300 - 23);
    lv_obj_add_flag(s_descend_btn, LV_OBJ_FLAG_HIDDEN);
}

static void game_layer_build(void)
{
    s_g_layer = layer_create();
    /* 视口满圆 */
    s_viewport = lv_img_create(s_g_layer);
    lv_img_set_src(s_viewport, &s_viewport_dsc);
    lv_obj_set_pos(s_viewport, DG_VP_ORIGIN, DG_VP_ORIGIN);
    lv_obj_add_flag(s_viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_viewport, on_viewport_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_viewport, on_viewport_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    /* HUD 与 toolbar 是 g_layer 的子层，切场景时随 g_layer 显隐但单独控 toolbar */
    lv_obj_t *hud_parent = lv_obj_create(s_g_layer);
    lv_obj_set_size(hud_parent, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_pos(hud_parent, 0, 0);
    lv_obj_set_style_bg_opa(hud_parent, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hud_parent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hud_parent, LV_OBJ_FLAG_SCROLLABLE);
    hud_build(hud_parent);

    lv_obj_t *tb_parent = lv_obj_create(s_g_layer);
    lv_obj_set_size(tb_parent, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_pos(tb_parent, 0, 0);
    lv_obj_set_style_bg_opa(tb_parent, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(tb_parent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tb_parent, LV_OBJ_FLAG_SCROLLABLE);
    toolbar_build(tb_parent);
}

static void title_layer_build(void)
{
    s_title_layer = layer_create();
    lv_obj_t *box = lv_obj_create(s_title_layer);
    lv_obj_set_size(box, 300, 300);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0c0c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x8a7a50), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, "次元地牢");
    lv_obj_set_style_text_font(t, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *sub = lv_label_create(box);
    lv_label_set_text(sub, "深入 12 层 · 取回护身符\n点地图即移动 / 攻击 / 拾取");
    lv_obj_set_style_text_font(sub, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9a9a8a), LV_PART_MAIN);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 46);

    /* 中央徽记用上游奖杯图标 */
    lv_obj_t *crest = icon_img_create(box, DG_SHEET_ICONS, IC_TROPHY, 3, PANEL_BG);
    if (crest) lv_obj_align(crest, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *bnew = icon_btn_create(box, DG_SHEET_ICONS, IC_SWORD, 52,
                                     on_title_new_cb, 0, BTN_BG);
    lv_obj_align(bnew, LV_ALIGN_BOTTOM_MID, -34, -22);
    lv_obj_t *blbl = lv_label_create(bnew);
    lv_label_set_text(blbl, "新冒险");
    lv_obj_set_style_text_font(blbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(blbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(blbl, LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_obj_t *bload = icon_btn_create(box, DG_SHEET_ICONS, IC_RESTART, 52,
                                      on_title_load_cb, 0, BTN_BG);
    lv_obj_align(bload, LV_ALIGN_BOTTOM_MID, 34, -22);
    lv_obj_t *llbl = lv_label_create(bload);
    lv_label_set_text(llbl, "继续");
    lv_obj_set_style_text_font(llbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(llbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(llbl, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void class_layer_build(void)
{
    s_class_layer = layer_create();
    lv_obj_t *t = lv_label_create(s_class_layer);
    lv_label_set_text(t, "选择职业");
    lv_obj_set_style_text_font(t, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    static const struct { int sheet; int cell; dg_class_t cls; const char *name; } cls[] = {
        { DG_SHEET_HERO_WARRIOR,  0, DG_CLASS_WARRIOR, "战士" },
        { DG_SHEET_HERO_MAGE,     0, DG_CLASS_MAGE,    "法师" },
        { DG_SHEET_HERO_ROGUE,    0, DG_CLASS_ROGUE,   "盗贼" },
        { DG_SHEET_HERO_HUNTRESS, 0, DG_CLASS_HUNTER,  "猎手" },
    };
    /* 4 个头像按钮沿水平排开（y=150，半弦内），每个 16×16 头像 scale4=64 */
    const int xs[4] = { 52, 128, 204, 280 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(s_class_layer);
        lv_obj_set_size(b, 66, 92);   /* 加高：头像 64 + 名字行不叠字 */
        lv_obj_set_pos(b, xs[i] - 33, 150 - 46);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x14161e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_hex(0x556072), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_set_ext_click_area(b, 4);

        lv_obj_t *ic = icon_img_create(b, cls[i].sheet, cls[i].cell, 3, PANEL_BG);
        if (ic) lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_t *nl = lv_label_create(b);
        lv_label_set_text(nl, cls[i].name);
        lv_obj_set_style_text_font(nl, &si_yuan_black_icon_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(nl, lv_color_hex(0xd8d0a0), LV_PART_MAIN);
        lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, 2);
        lv_obj_add_event_cb(b, on_class_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cls[i].cls);
    }

    lv_obj_t *back = icon_btn_create(s_class_layer, DG_SHEET_ICONS, IC_CLOSE, 44,
                                     on_class_back_cb, 0, BTN_BG);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void inv_layer_build(void)
{
    s_inv_layer = layer_create();
    /* 半透全屏遮罩（可点关闭外部无效区不关，避免误触；用不点透的子面板） */
    lv_obj_t *mask = lv_obj_create(s_inv_layer);
    lv_obj_set_size(mask, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(mask);
    lv_label_set_text(title, "背包");
    lv_obj_set_style_text_font(title, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    /* 网格 6×5，格心落在圆内（面板 300×170 居中，格 46×34） */
    const int COLS = 6, ROWS = 6, cell = 40;
    int gx0 = (DG_SCREEN_W - COLS * cell) / 2;
    int gy0 = 54;
    for (int i = 0; i < DG_MAX_INVENTORY; i++) {
        int r = i / COLS, c = i % COLS;
        lv_obj_t *slot = lv_btn_create(mask);
        lv_obj_set_size(slot, 36, 36);
        lv_obj_set_pos(slot, gx0 + c * cell + 2, gy0 + r * cell + 2);
        lv_obj_set_style_radius(slot, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slot, lv_color_hex(0x1c1c26), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(slot, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(slot, lv_color_hex(0x3a3a48), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(slot, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(slot, 0, LV_PART_MAIN);

        /* 预分配 32×32 图标缓冲，refresh 时复用 */
        s_inv[i].buf = heap_caps_malloc(32 * 32 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        s_inv[i].dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        s_inv[i].dsc.header.always_zero = 0;
        s_inv[i].dsc.header.w = 32;
        s_inv[i].dsc.header.h = 32;
        s_inv[i].dsc.data_size = 32 * 32 * sizeof(uint16_t);
        s_inv[i].dsc.data = (const uint8_t *)s_inv[i].buf;
        s_inv[i].img = lv_img_create(slot);
        lv_img_set_src(s_inv[i].img, &s_inv[i].dsc);
        lv_obj_center(s_inv[i].img);
        lv_obj_add_flag(s_inv[i].img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(slot, on_inv_cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(slot, LV_OBJ_FLAG_HIDDEN);
    }
    (void)ROWS;

    /* 详情行 + 三动作键 + 关闭 */
    s_inv_detail = lv_label_create(mask);
    lv_label_set_text(s_inv_detail, "点选物品");
    lv_obj_set_style_text_font(s_inv_detail, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_inv_detail, lv_color_hex(0xd8d0a0), LV_PART_MAIN);
    lv_obj_set_width(s_inv_detail, 240);
    lv_obj_set_style_text_align(s_inv_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_inv_detail, LV_ALIGN_CENTER, 0, 42);

    static const struct { const char *name; int cell; int act; } acts[] = {
        { "使用", IC_PLUS,  0 },
        { "装备", IC_SWORD, 1 },
        { "丢弃", IC_SWAP,  2 },
    };
    for (int i = 0; i < 3; i++) {
        int xs = 108 + i * 72;
        lv_obj_t *b = icon_btn_create(mask, DG_SHEET_ICONS, acts[i].cell, 44,
                                      on_inv_action_cb, acts[i].act, BTN_BG);
        lv_obj_set_pos(b, xs - 22, 250);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, acts[i].name);
        lv_obj_set_style_text_font(l, &si_yuan_black_icon_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    lv_obj_t *close = icon_btn_create(mask, DG_SHEET_ICONS, IC_CLOSE, 40,
                                      on_inv_close_cb, 0, BTN_BG);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -20, 22);
}

static void menu_layer_build(void)
{
    s_menu_layer = layer_create();
    lv_obj_t *box = lv_obj_create(s_menu_layer);
    lv_obj_set_size(box, 240, 210);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0c0c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x8a7a50), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, "菜单");
    lv_obj_set_style_text_font(t, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    /* menu_action：0 保存 1 读取 2 回标题（槽 0） */
    static const struct { const char *name; int cell; int act; } m[] = {
        { "保存", IC_CLIPBOARD, 0 },
        { "读取", IC_BOOK,      1 },
        { "回标题", IC_RESTART,  2 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = icon_btn_create(box, DG_SHEET_ICONS, m[i].cell, 56,
                                      on_menu_cb, m[i].act, BTN_BG);
        lv_obj_set_pos(b, 24 + i * 68, 120);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, m[i].name);
        lv_obj_set_style_text_font(l, &si_yuan_black_icon_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 1);
    }
}

static void end_layer_build(lv_obj_t **out, bool win)
{
    lv_obj_t *layer = layer_create();
    *out = layer;
    lv_obj_t *box = lv_obj_create(layer);
    lv_obj_set_size(box, 280, 240);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0c0c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(win ? 0xd0a040 : 0x8a3030), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = icon_img_create(box, DG_SHEET_ICONS, win ? IC_TROPHY : IC_SKULL,
                                   4, PANEL_BG);
    if (ic) lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, win ? "通关！" : "你死了");
    lv_obj_set_style_text_font(t, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(win ? 0xf0d060 : 0xe06060), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *sub = lv_label_create(box);
    lv_label_set_text(sub, win ? "护身符到手，地牢臣服" : "地牢记住了你的名字");
    lv_obj_set_style_text_font(sub, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9a9a8a), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *b1 = icon_btn_create(box, DG_SHEET_ICONS, IC_RESTART, 52,
                                   on_end_cb, win ? 1 : 1, BTN_BG);
    lv_obj_align(b1, LV_ALIGN_BOTTOM_MID, -32, -18);
    lv_obj_t *l1 = lv_label_create(b1);
    lv_label_set_text(l1, "再来");
    lv_obj_set_style_text_font(l1, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l1, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(l1, LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_obj_t *b2 = icon_btn_create(box, DG_SHEET_ICONS, IC_CLOSE, 52,
                                   on_end_cb, 0, BTN_BG);
    lv_obj_align(b2, LV_ALIGN_BOTTOM_MID, 32, -18);
    lv_obj_t *l2 = lv_label_create(b2);
    lv_label_set_text(l2, "回标题");
    lv_obj_set_style_text_font(l2, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l2, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(l2, LV_ALIGN_BOTTOM_MID, 0, -2);
}

/* ==================== 启动 ==================== */

void ui_dungeon_start(void)
{
    ESP_LOGI(TAG, "ui_dungeon_start (v0.4 upstream, tile x%d)", DG_VIEW_SCALE);

    s_display_fb = heap_caps_malloc(DG_VIEW_SIZE * DG_VIEW_SIZE * sizeof(uint16_t),
                                    MALLOC_CAP_SPIRAM);
    if (!s_display_fb)
        s_display_fb = calloc(DG_VIEW_SIZE * DG_VIEW_SIZE, sizeof(uint16_t));
    s_viewport_dsc.data = (const uint8_t *)s_display_fb;
    /* 首帧先涂黑，避免加载前白点 */
    for (int i = 0; i < DG_VIEW_SIZE * DG_VIEW_SIZE; i++) s_display_fb[i] = 0;

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0a0a0a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 引擎 init 已把 scene 置 TITLE；建全部页面 */
    game_layer_build();
    title_layer_build();
    class_layer_build();
    inv_layer_build();
    menu_layer_build();
    end_layer_build(&s_over_layer, false);
    end_layer_build(&s_win_layer, true);

    /* 音频：BSP 已在 main.c 完成 sdgoods_audio_init，这里开混音任务 */
    dg_audio_start();

    lv_scr_load(s_scr);
    s_running = true;
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

bool ui_dungeon_is_running(void) { return s_running; }

/* ==================== 场景切换 ==================== */

static void set_hidden(lv_obj_t *o, bool hide)
{
    if (!o) return;
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void show_scene(dg_scene_t scene)
{
    set_hidden(s_title_layer, scene != DG_SCENE_TITLE);
    set_hidden(s_class_layer, scene != DG_SCENE_CLASS_SELECT);
    set_hidden(s_inv_layer,   scene != DG_SCENE_INVENTORY);
    set_hidden(s_menu_layer,  scene != DG_SCENE_MENU);
    set_hidden(s_over_layer,  scene != DG_SCENE_GAME_OVER);
    set_hidden(s_win_layer,   scene != DG_SCENE_WIN);

    bool in_dungeon = (scene == DG_SCENE_IN_GAME || scene == DG_SCENE_INVENTORY ||
                       scene == DG_SCENE_MENU || scene == DG_SCENE_GAME_OVER ||
                       scene == DG_SCENE_WIN);
    set_hidden(s_g_layer, !in_dungeon);
    /* HUD / toolbar：仅纯游戏态显示；overlay 页把 toolbar 藏起，HUD 保留在死亡页也行 */
    if (s_toolbar) set_hidden(s_toolbar, scene != DG_SCENE_IN_GAME);
    /* HUD 只在纯游戏态显示：背包页盖住它（否则标题与深度文字叠字），
     * 菜单/结算页是居中弹窗不挡图，留 HUD 反而有“在看状态栏”的游戏感 */
    if (s_hud)     set_hidden(s_hud, !(scene == DG_SCENE_IN_GAME || scene == DG_SCENE_MENU));

    if (scene == DG_SCENE_INVENTORY) refresh_inventory();
    if (scene == DG_SCENE_TITLE) {
        /* 标题：无存档则「继续」置灰 */
        /* （视觉置灰可后续补，这里保留按钮常亮） */
    }
}

/* ==================== 逐帧 poll ==================== */

void ui_dungeon_poll(void)
{
    if (!s_running) return;

    dg_api_tick_if_needed();

    /* 音效队列 → 音频层（一帧可有多条，全部取走） */
    int sfx_id;
    int guard = 0;
    while (dg_api_pop_sfx(&sfx_id) && guard++ < 8) {
        dg_audio_play_sfx(sfx_id);
    }

    dg_scene_t sc = dg_api_current_scene();
    if (sc != s_shown_scene) {
        s_shown_scene = sc;
        show_scene(sc);
    }

    uint32_t now = lv_tick_get();
    bool g_visible = (sc == DG_SCENE_IN_GAME || sc == DG_SCENE_INVENTORY ||
                      sc == DG_SCENE_MENU || sc == DG_SCENE_GAME_OVER || sc == DG_SCENE_WIN);

    /* 视口刷新（g_layer 可见时） */
    if (g_visible) {
        int fb_w = 0, fb_h = 0;
        const uint16_t *fb = dg_api_get_tilemap_fb(&fb_w, &fb_h);
        if (fb && fb_w == DG_VIEW_LOG && fb_h == DG_VIEW_LOG) {
            for (int y = 0; y < DG_VIEW_LOG; y++) {
                uint16_t *row0 = s_display_fb + (y * 2) * DG_VIEW_SIZE;
                uint16_t *row1 = row0 + DG_VIEW_SIZE;
                const uint16_t *srow = fb + y * DG_VIEW_LOG;
                for (int x = 0; x < DG_VIEW_LOG; x++) {
                    uint16_t b = bswap16(srow[x]);
                    row0[x * 2] = b; row0[x * 2 + 1] = b;
                    row1[x * 2] = b; row1[x * 2 + 1] = b;
                }
            }
            lv_obj_invalidate(s_viewport);
        }
    }

    /* HUD + 消息（150ms 刷新，比 v0.3 更跟手） */
    if (sc == DG_SCENE_IN_GAME || sc == DG_SCENE_INVENTORY) {
        if (now - s_last_hud_ms >= 150) {
            s_last_hud_ms = now;
            dg_hud_t hud;
            if (dg_api_get_hud(&hud)) {
                if (hud.hp_max <= 0) hud.hp_max = 1;
                lv_bar_set_value(s_hp_bar, hud.hp * 100 / hud.hp_max, LV_ANIM_ON);
                if (hud.exp_max <= 0) hud.exp_max = 1;
                lv_bar_set_value(s_xp_bar, hud.exp * 100 / hud.exp_max, LV_ANIM_OFF);
                if (hud.energy_max <= 0) hud.energy_max = 1;
                lv_bar_set_value(s_en_bar, hud.energy * 100 / hud.energy_max, LV_ANIM_OFF);
                char b[24];
                /* 圆屏 HUD 胶囊只有 150px（受圆形边框半弦约束），三格 CJK
                 * 标签塞不下「层+章节+等级+金币」，会和居中的 Lv 叠字。章节由
                 * tileset 视觉自证，这里只留「层号」，把中心位让给等级。 */
                snprintf(b, sizeof(b), "%dF", hud.depth);
                lv_label_set_text(s_depth_lbl, b);
                snprintf(b, sizeof(b), "Lv%d", hud.lvl);
                lv_label_set_text(s_lvl_lbl, b);
                snprintf(b, sizeof(b), "%d金", hud.gold);
                lv_label_set_text(s_gold_lbl, b);
                /* 下楼键：站在出口才显示（否则藏起，不占地图） */
                if (s_descend_btn) {
                    set_hidden(s_descend_btn, !hud.on_stairs);
                    lv_obj_set_style_border_color(s_descend_btn,
                        lv_color_hex(hud.on_stairs ? 0xd0a040 : 0x556072), LV_PART_MAIN);
                }
            }
            char msg[96];
            if (dg_api_get_message(msg, sizeof(msg), 0) > 0) {
                /* 只在“换了新消息”时重置 toast，同一句不反复刷新计时 */
                if (strcmp(msg, s_msg_last) != 0) {
                    snprintf(s_msg_last, sizeof(s_msg_last), "%s", msg);
                    lv_label_set_text(s_msg_lbl, msg);
                    lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_opa(s_msg_lbl, LV_OPA_COVER, LV_PART_MAIN);
                    s_msg_expire_ms = now + DG_TOAST_MS;
                }
            }
        }
    }

    /* toast 自动淡隐（每帧跑，不受 150ms 门控）：最后 500ms 渐隐，到期藏起。
     * 非游戏态（背包/菜单/结算）不干预，交由场景切换自然隐藏。 */
    if (s_msg_lbl && s_msg_expire_ms &&
        (sc == DG_SCENE_IN_GAME || sc == DG_SCENE_INVENTORY)) {
        if (now >= s_msg_expire_ms) {
            lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
            s_msg_expire_ms = 0;
            /* 注意：不清 s_msg_last。引擎 get_message 会持续返回同一句（常驻），
             * 若清空则下一帧又判定为“新消息”重新弹出 → toast 永远淡不掉。
             * 保留末次文本，只有真正不同的新消息才重新弹。 */
        } else {
            uint32_t left = s_msg_expire_ms - now;
            if (left < 500)
                lv_obj_set_style_opa(s_msg_lbl, (lv_opa_t)(255 * left / 500), LV_PART_MAIN);
        }
    }

    /* 背包打开时，物品变化（使用/装备/丢弃后）刷新由动作回调触发；
     * 这里兜底：每帧若选中格变无效则清空选择 */
    if (sc == DG_SCENE_INVENTORY && now - s_last_hud_ms >= 150) {
        /* no-op：refresh 已在动作里做 */
    }
}

/* ==================== 背包刷新 ==================== */

static void refresh_inventory(void)
{
    int n = dg_api_inv_count();
    for (int i = 0; i < DG_MAX_INVENTORY; i++) {
        lv_obj_t *slot_parent = s_inv[i].img ? lv_obj_get_parent(s_inv[i].img) : NULL;
        if (!slot_parent) continue;
        if (i < n && s_inv[i].buf) {
            dg_item_info_t it;
            if (dg_api_inv_get(i, &it)) {
                fill_icon_buf(DG_SHEET_ITEMS, it.icon, s_inv[i].buf, 2, PANEL_BG);
                lv_obj_clear_flag(s_inv[i].img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_invalidate(s_inv[i].img);
            }
            lv_obj_clear_flag(slot_parent, LV_OBJ_FLAG_HIDDEN);
            /* 已装备高亮边框 */
            lv_obj_set_style_border_color(slot_parent,
                lv_color_hex(it.equipped ? 0xd0a040 : 0x3a3a48), LV_PART_MAIN);
        } else {
            lv_obj_add_flag(slot_parent, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_inv_sel >= n) { s_inv_sel = -1; lv_label_set_text(s_inv_detail, "点选物品"); }
}

/* ==================== 输入回调 ==================== */

static void on_viewport_click_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_viewport) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t r;
    lv_obj_get_coords(s_viewport, &r);
    int px = (pt.x - r.x1) / DG_VIEW_SCALE;
    int py = (pt.y - r.y1) / DG_VIEW_SCALE;
    dg_api_on_viewport_tap(px, py);
}

static void on_viewport_long_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_viewport) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t r;
    lv_obj_get_coords(s_viewport, &r);
    int px = (pt.x - r.x1) / DG_VIEW_SCALE;
    int py = (pt.y - r.y1) / DG_VIEW_SCALE;
    int cx = 0, cy = 0;
    dg_api_get_camera(&cx, &cy);
    dg_api_on_long_press(cx + px / DG_TILE_PX, cy + py / DG_TILE_PX);
}

static void on_toolbar_cb(lv_event_t *e)
{
    dg_btn_id_t btn = (dg_btn_id_t)(intptr_t)lv_event_get_user_data(e);
    dg_api_on_button(btn);
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void on_title_new_cb(lv_event_t *e)
{
    (void)e;
    dg_api_goto_class_select();
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void on_title_load_cb(lv_event_t *e)
{
    (void)e;
    if (dg_api_has_save(0)) {
        if (dg_api_load_game(0)) {
            s_shown_scene = dg_api_current_scene();
            show_scene(s_shown_scene);
        }
    } else {
        dg_api_goto_class_select();
        s_shown_scene = dg_api_current_scene();
        show_scene(s_shown_scene);
    }
}

static void on_class_cb(lv_event_t *e)
{
    int cls = (int)(intptr_t)lv_event_get_user_data(e);
    dg_api_pick_class(cls);
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void on_class_back_cb(lv_event_t *e)
{
    (void)e;
    dg_api_goto_title();
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void on_inv_cell_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    dg_item_info_t it;
    if (!dg_api_inv_get(slot, &it)) return;
    s_inv_sel = slot;
    char b[96];
    int used = snprintf(b, sizeof(b), "%s", it.name ? it.name : "?");
    if (it.qty > 1) used += snprintf(b + used, sizeof(b) - used, " ×%d", it.qty);
    if (it.tier > 0) used += snprintf(b + used, sizeof(b) - used, " T%d", it.tier);
    if (it.str_req > 0) snprintf(b + used, sizeof(b) - used, " 需力%d", it.str_req);
    lv_label_set_text(s_inv_detail, b);
    /* 选中高亮：选中格描金边，其余恢复默认 */
    for (int i = 0; i < DG_MAX_INVENTORY; i++) {
        lv_obj_t *p = s_inv[i].img ? lv_obj_get_parent(s_inv[i].img) : NULL;
        if (!p) continue;
        lv_obj_set_style_border_color(p,
            lv_color_hex(i == slot ? 0xf0e0a0 : 0x3a3a48), LV_PART_MAIN);
    }
}

static void on_inv_action_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_inv_sel < 0) { dg_audio_play_sfx(DG_SFX_ERROR); return; }
    bool ok = false;
    switch (act) {
    case 0: ok = dg_api_inv_use(s_inv_sel);   break;
    case 1: ok = dg_api_inv_equip(s_inv_sel); break;
    case 2: ok = dg_api_inv_drop(s_inv_sel);  break;
    }
    if (ok) {
        if (act == 2) s_inv_sel = -1;
        refresh_inventory();
    }
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void on_inv_close_cb(lv_event_t *e)
{
    (void)e;
    dg_api_on_button(DG_BTN_INVENTORY);
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void on_menu_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    dg_api_menu_action(act, 0);
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void on_end_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    if (act == 0) dg_api_goto_title();
    else          dg_api_goto_class_select();
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

/* ==================== 串口调试注入（覆盖 bsp 弱符号） ====================
 *  v0.4 场景流（配合 tools/play_test.py 取证，全部同一 COM6 会话）：
 *   h=回标题  g=进选职业  1/2/3/4=选战/法/贼/猎并开局  s=截屏(bsp)
 *   w/a/x/d   相邻 tile 走一步（走/砍/捡由引擎判）
 *   q=等待 e=搜索 j=背包开关 r=下楼 f=快速装备 m=菜单开关
 *   t + "x,y;" 像素 tap（命中视口即等价手指点地图）
 *   n + "x,y;" tile 直推引擎（A*）
 *   v         debug_dump 一行
 */
void sdgoods_console_ext_cmd(char c)
{
    static char s_coord[24];
    static char s_coord_mode = 0;
    if (s_coord_mode) {
        if ((c >= '0' && c <= '9') || c == ',') {
            if (strlen(s_coord) < sizeof(s_coord) - 1) strncat(s_coord, &c, 1);
            return;
        }
        char *comma = strchr(s_coord, ',');
        if (comma && (c == ';' || c == '\n')) {
            *comma = '\0';
            int x = atoi(s_coord), y = atoi(comma + 1);
            if (s_coord_mode == 't') sdgoods_tap_synth(x, y, 0, 0, 1);
            else dg_api_on_tap(x, y);
        }
        s_coord_mode = 0;
        return;
    }
    if (c == 't' || c == 'n') { s_coord_mode = c; s_coord[0] = '\0'; return; }
    if (c == 'v') {
        char dump[192];
        if (dg_api_debug_dump(dump, sizeof(dump)) > 0) ESP_LOGI(TAG, "%s", dump);
        char bf[64];
        if (dg_api_hero_buffs(bf, sizeof(bf)) > 0) ESP_LOGI(TAG, "B %s", bf);
        return;
    }
    if (c == 'b') { dg_api_debug_buff(2, 8); ESP_LOGI(TAG, "ext_cmd: b (haste+8)"); return; }  /* 取证：施加 HASTE */
    if (c == 'k') { ESP_LOGI(TAG, "ext_cmd: k (M3 selftest)"); dg_api_debug_m3(); return; }      /* 取证：M3 机制自检 */
    if (c == 'p') { ESP_LOGI(TAG, "ext_cmd: p (M4 selftest)"); dg_api_debug_m4(); return; }      /* 取证：M4 物品全谱自检 */
    if (c == 'y') { ESP_LOGI(TAG, "ext_cmd: y (M5 selftest)"); dg_api_debug_m5(); return; }      /* 取证：M5 关卡生成自检 */

    /* 场景流直推（不经像素命中，取证确定性强） */
    switch (c) {
    case 'h': dg_api_goto_title();        return;
    case 'g': dg_api_goto_class_select(); return;
    case '1': dg_api_pick_class(DG_CLASS_WARRIOR); return;
    case '2': dg_api_pick_class(DG_CLASS_MAGE);    return;
    case '3': dg_api_pick_class(DG_CLASS_ROGUE);   return;
    case '4': dg_api_pick_class(DG_CLASS_HUNTER);  return;
    default: break;
    }

    static const int8_t dl[4][2] = { {0,-1}, {0,1}, {-1,0}, {1,0} };
    int hx = 0, hy = 0;
    switch (c) {
    case 'w': case 'a': case 'x': case 'd': {
        int idx = (c == 'w') ? 0 : (c == 'x') ? 1 : (c == 'a') ? 2 : 3;
        dg_api_get_hero_pos(&hx, &hy);
        dg_api_on_tap(hx + dl[idx][0], hy + dl[idx][1]);
        break;
    }
    case 'q': dg_api_on_button(DG_BTN_WAIT);      break;
    case 'e': dg_api_on_button(DG_BTN_SEARCH);    break;
    case 'j': dg_api_on_button(DG_BTN_INVENTORY); break;
    case 'r': dg_api_on_button(DG_BTN_DESCEND);   break;
    case 'f': dg_api_on_button(DG_BTN_EQUIP);     break;
    case 'm': dg_api_on_button(DG_BTN_MENU);      break;
    default:
        ESP_LOGI(TAG, "ext_cmd: %c (h g 1-4 w a x d q e j r f m t n v)", c);
        break;
    }
}
