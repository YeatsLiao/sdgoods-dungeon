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
 *   4. 操控为主 + 触屏为辅：底部对称手柄布局 —— 左下方向十字（逐格移动/攻击）
 *      + 右下动作十字（待/搜/包/菜，上游图标），两十字同深色圆键风格、中间留空列
 *      给英雄/下楼键；点地图即走/砍/捡（A* 由引擎做）作为辅助。控件全部落在圆内、
 *      不压满圆中心（圆屏适配）。
 *   5. 音效接线：每帧 dg_api_pop_sfx → dg_audio_play_sfx；开屏 dg_audio_start。
 *   6. v0.5 表现层（游戏感）：受击抖屏 / 低血闪烁 / 出口呼吸路点 / 换层横幅 /
 *      首次引导浮层 / 结算氛围雾 / 场景转场（逐对象 opa + y 位移，不走
 *      OPA_LAYERED 离屏合成——360 满圆 layer 在 PSRAM 上开销过大）。
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
#include "nvs.h"                  /* 首次引导标记（一次性持久化） */
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
#define IC_CHECK       7    /* 绿对勾（使用） */
#define IC_BOOK        8    /* 打开的书（读取） */
#define IC_ARROW       16   /* 灰色右箭头 */
#define IC_STATS       24   /* 上升条图（属性面板：stats 语义） */
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

/* 上游职业头像：HeroSprite.FRAME_WIDTH=12/HEIGHT=15，帧是 12px 步长紧排；
 * avatar() 用 uvRect(1,0,12,15) —— 帧1（x=12..23）。之前按 16px 网格取 x=16
 * 正好横跨帧1尾+帧2头，职业页头像被“切两半”。 */
#define HERO_AVATAR_SX 12
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
/* 换层横幅显示期间压制新 toast（横幅与 toast 同处地图上部会重叠）；
 * 被压制的消息在窗口结束后照常弹一次 */
static uint32_t s_msg_suppress_until = 0;

/* 独立页面 */
static lv_obj_t *s_title_layer = NULL;
static lv_obj_t *s_class_layer = NULL;
static lv_obj_t *s_inv_layer   = NULL;
static lv_obj_t *s_hero_layer  = NULL;   /* 英雄属性面板（DG_SCENE_HERO） */
static lv_obj_t *s_hero_title  = NULL;
static lv_obj_t *s_hero_txt    = NULL;
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

/* ---- v0.5 表现层（游戏感）---- */
static lv_obj_t *s_waypoint    = NULL;   /* 出口呼吸路点（圆环） */
static lv_obj_t *s_banner      = NULL;   /* 「第 N 层 · 章节」横幅 */
static lv_obj_t *s_guide_layer = NULL;   /* 首次引导浮层 */
static lv_obj_t *s_end_box[2]  = {0};    /* 死亡/通关圆盘（索引 0=死 1=胜） */
static int16_t   s_prev_hp     = -1;     /* 上一轮 HUD 血量（受击抖屏检测） */
static int16_t   s_last_depth  = 0;      /* 上一轮层数（换层横幅检测） */
static bool      s_hp_low      = false;  /* 低血告警闪烁进行中 */
static bool      s_guide_done  = false;  /* 本次会话引导已检查过 NVS */
static bool      s_guide_active= false;  /* 引导未关闭（回游戏要重新弹出） */
static lv_obj_t *s_class_desc  = NULL;   /* 职业介绍（两段式选择） */
static lv_obj_t *s_class_btns[4] = {0};  /* 职业卡（选中高亮描边） */
static int       s_class_sel   = -1;     /* 当前展示介绍的职业，-1=未选 */

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
static void on_menu_cb(lv_event_t *e);
static void on_end_cb(lv_event_t *e);
static void refresh_inventory(void);
static void hero_layer_build(void);
static void refresh_hero(void);
static void anim_opa_cb(void *var, int32_t v);
static void anim_translate_y_cb(void *var, int32_t v);
static void guide_layer_build(void);
static void viewport_shake(void);
static void waypoint_update(const dg_hud_t *hud);
static void banner_show(int depth, int chapter);
static void fade_tree(lv_obj_t *o, uint32_t time);

/* ==================== 素材图标工具 ==================== */

/* 把图集一格 blit 进 buf（side×side，scale 最近邻），叠在 bg 上，再整块 bswap
 * 成显示字节序。buf 须已分配 side*side*2 字节。key_skip=true 保留 bg 透明处。 */
static void fill_icon_buf(int sheet, int cell, uint16_t *buf, int scale, uint16_t bg)
{
    int side = DG_ICON_PX * scale;
    uint16_t tmp[DG_ICON_PX * DG_ICON_PX];
    for (int i = 0; i < DG_ICON_PX * DG_ICON_PX; i++) tmp[i] = bg;
    dg_api_blit_icon(sheet, CELL_X(cell), CELL_Y(cell), DG_ICON_PX, DG_ICON_PX,
                     tmp, DG_ICON_PX, DG_ICON_PX, DG_ICON_PX, 1, true);
    /* icons 图集各格墨迹普遍不居中（红叉中心 5.0≠8、且每格偏移都不同），
     * 硬编码补偿没完没了：先算墨迹包围盒，按墨迹居中放进 buf */
    int x0 = DG_ICON_PX, y0 = DG_ICON_PX, x1 = -1, y1 = -1;
    for (int y = 0; y < DG_ICON_PX; y++)
        for (int x = 0; x < DG_ICON_PX; x++)
            if (tmp[y * DG_ICON_PX + x] != bg) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) { x0 = 0; y0 = 0; x1 = DG_ICON_PX - 1; y1 = DG_ICON_PX - 1; }
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    int ox = (side - bw * scale) / 2 - x0 * scale;
    int oy = (side - bh * scale) / 2 - y0 * scale;
    for (int i = 0; i < side * side; i++) buf[i] = bg;
    for (int y = 0; y < DG_ICON_PX; y++)
        for (int x = 0; x < DG_ICON_PX; x++) {
            uint16_t c = tmp[y * DG_ICON_PX + x];
            if (c == bg) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int dx = ox + x * scale + sx, dy = oy + y * scale + sy;
                    if (dx >= 0 && dx < side && dy >= 0 && dy < side)
                        buf[dy * side + dx] = c;
                }
        }
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

/* 英雄立绘：上游英雄图集是 12px 步长紧排帧（HeroSprite.FRAME_WIDTH=12），
 * 按 16px 网格取帧必带上邻帧碎片（职业页“头像被切成两半”的根因）。
 * 取上游 avatar() 的帧：uvRect(1,0,12,15)。scale=放大倍率。 */
static lv_obj_t *hero_img_create(lv_obj_t *parent, int sheet, int scale, uint16_t bg)
{
    const int w = 12, h = 15;
    int bw = w * scale, bh = h * scale;
    size_t nbytes = (size_t)bw * bh * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM);
    lv_img_dsc_t *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    if (!buf || !d) return NULL;
    for (int i = 0; i < bw * bh; i++) buf[i] = bg;
    dg_api_blit_icon(sheet, HERO_AVATAR_SX, HERO_AVATAR_SY, w, h,
                     buf, bw, bw, bh, scale, true);
    for (int i = 0; i < bw * bh; i++) buf[i] = bswap16(buf[i]);
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.always_zero = 0;
    d->header.w = bw;
    d->header.h = bh;
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

    /* 血条 / 经验条 / 能量条（细）。LVGL bar 底色在 LV_PART_MAIN、前景
     * 在 LV_PART_INDICATOR——之前 INDICATOR 误写成 LV_PART_MAIN（且连写
     * 两行互相覆盖），三根条全成一片深色，血量根本看不出来 */
    s_hp_bar = lv_bar_create(cap);
    lv_obj_set_size(s_hp_bar, 138, 9);
    lv_obj_align(s_hp_bar, LV_ALIGN_BOTTOM_LEFT, 2, -12);
    lv_bar_set_range(s_hp_bar, 0, 100);
    lv_bar_set_value(s_hp_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_hp_bar, lv_color_hex(0x3a1414), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_hp_bar, lv_color_hex(0xd0403a), LV_PART_INDICATOR);
    lv_obj_clear_flag(s_hp_bar, LV_OBJ_FLAG_CLICKABLE);

    s_xp_bar = lv_bar_create(cap);
    lv_obj_set_size(s_xp_bar, 138, 5);
    lv_obj_align(s_xp_bar, LV_ALIGN_BOTTOM_LEFT, 2, -4);
    lv_bar_set_range(s_xp_bar, 0, 100);
    lv_bar_set_value(s_xp_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_xp_bar, lv_color_hex(0x1a1a28), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_xp_bar, lv_color_hex(0x5a7ad0), LV_PART_INDICATOR);
    lv_obj_clear_flag(s_xp_bar, LV_OBJ_FLAG_CLICKABLE);

    s_en_bar = lv_bar_create(cap);
    lv_obj_set_size(s_en_bar, 40, 5);
    lv_obj_align(s_en_bar, LV_ALIGN_BOTTOM_RIGHT, -2, -4);
    lv_bar_set_range(s_en_bar, 0, 100);
    lv_bar_set_value(s_en_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_en_bar, lv_color_hex(0x241c10), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_en_bar, lv_color_hex(0xd0a040), LV_PART_INDICATOR);
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
    lv_obj_set_width(s_msg_lbl, 250);
    lv_obj_set_style_text_align(s_msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_msg_lbl, LV_ALIGN_TOP_MID, 0, 70);
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

/* 统一控制圆键：深色不透底 + 白描边，与动作键(icon_btn_create)同风格，
 * 消除“方向键半透白 / 动作键实心深色”两套视觉。方向键内嵌白色箭头。 */
static lv_obj_t *ghost_btn_create(lv_obj_t *parent, int cx, int cy, int d,
                                  lv_event_cb_t cb, int id)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1a2230), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(0x8894a8), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(b, 6);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    return b;
}

static void toolbar_build(lv_obj_t *parent)
{
    s_toolbar = parent;

    /* 对称手柄布局（蓝图已确认）：左下方向十字 + 右下动作十字，两十字中间
     * 留空列给英雄/下楼键，控件沉到下半区不压满圆中心。cluster 距屏心 ±76。 */
    const int kd = 44, arm = 36;
    static const int8_t arm_pos[4][2] = {
        { 0, -arm }, { arm, 0 }, { 0, arm }, { -arm, 0 } };   /* 上右下左 */

    /* 左下：4 向方向键（逐格移动 / 朝该方向攻击），cluster(104,252) */
    const int dcx = 104, dcy = 252;
    for (int dir = 0; dir < 4; dir++) {
        int cx = dcx + arm_pos[dir][0], cy = dcy + arm_pos[dir][1];
        lv_obj_t *b = ghost_btn_create(parent, cx, cy, kd, on_dpad_cb, dir);
        lv_obj_t *ln = lv_line_create(b);
        lv_line_set_points(ln, s_chev[dir], 3);
        lv_obj_set_style_line_color(ln, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_line_width(ln, 3, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(ln, true, LV_PART_MAIN);
        lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(ln);
    }

    /* 右下：4 个动作键（上游图标），cluster(256,252)，与方向键同尺寸同风格。
     * 上键此前是手柄图标挂「等待」：图标与功能对不上、玩家不知道它干什么
     * （用户反馈）。改为条图图标开英雄属性面板；等待移到点英雄自身格
     * （上游 onTAP 语义，引擎已支持）。 */
    const int acx = 256, acy = 252;
    static const struct { int cell, btn; } acts[] = {
        { IC_STATS,   DG_BTN_HERO },      /* 上：属性 */
        { IC_SEARCH,  DG_BTN_SEARCH },    /* 右 */
        { IC_BAG,     DG_BTN_INVENTORY }, /* 下 */
        { IC_SCROLL,  DG_BTN_MENU },      /* 左 */
    };
    for (unsigned i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        int cx = acx + arm_pos[i][0], cy = acy + arm_pos[i][1];
        lv_obj_t *b = icon_btn_create(parent, DG_SHEET_ICONS, acts[i].cell, kd,
                                      on_toolbar_cb, acts[i].btn, BTN_BG);
        lv_obj_set_pos(b, cx - kd / 2, cy - kd / 2);
    }

    /* 下楼键：站在出口才显示，底部正中（两十字之间空列） */
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

    /* v0.5 出口呼吸路点：叠在地图上、HUD/按键之下（建序在它们之前）。
     * 单对象无子件，style opa 呼吸不触发离屏 layer。 */
    s_waypoint = lv_obj_create(s_g_layer);
    lv_obj_set_size(s_waypoint, 30, 30);
    lv_obj_set_style_radius(s_waypoint, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_waypoint, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_waypoint, lv_color_hex(0x8fd8ff), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_waypoint, 3, LV_PART_MAIN);
    lv_obj_clear_flag(s_waypoint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_waypoint, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t wp;
    lv_anim_init(&wp);
    lv_anim_set_var(&wp, s_waypoint);
    lv_anim_set_exec_cb(&wp, anim_opa_cb);
    lv_anim_set_values(&wp, LV_OPA_30, LV_OPA_90);
    lv_anim_set_time(&wp, 650);
    lv_anim_set_playback_time(&wp, 650);
    lv_anim_set_repeat_count(&wp, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&wp);

    /* v0.5 换层横幅：顶部 HUD 胶囊（底 y=66）与 toast（y=70..）下方，不重叠 */
    s_banner = lv_label_create(s_g_layer);
    lv_obj_set_style_text_font(s_banner, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_banner, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_banner, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_radius(s_banner, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_banner, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_banner, 6, LV_PART_MAIN);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, 112);

    /* HUD 与 toolbar 是 g_layer 的子层，切场景时随 g_layer 显隐但单独控 toolbar。
     * 注意：裸 lv_obj_create 会带默认主题 padding（~12px），而子键坐标是相对
     * content 区的——不清零的话整个 HUD/工具栏会整体偏移（工具栏“不居中”根因） */
    lv_obj_t *hud_parent = lv_obj_create(s_g_layer);
    lv_obj_set_size(hud_parent, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_pos(hud_parent, 0, 0);
    lv_obj_set_style_pad_all(hud_parent, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hud_parent, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hud_parent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hud_parent, LV_OBJ_FLAG_SCROLLABLE);
    hud_build(hud_parent);

    lv_obj_t *tb_parent = lv_obj_create(s_g_layer);
    lv_obj_set_size(tb_parent, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_pos(tb_parent, 0, 0);
    lv_obj_set_style_pad_all(tb_parent, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tb_parent, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(tb_parent, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tb_parent, LV_OBJ_FLAG_SCROLLABLE);
    toolbar_build(tb_parent);
}

static void title_layer_build(void)
{
    s_title_layer = layer_create();
    /* 圆屏：开始面板用正圆盘（直径 320，半径 160 < 屏半径 180），而非圆角方块——
       方角会超出圆形可视区被裁，看着像被切角的正方形。内容沿中轴排布均在盘内。 */
    lv_obj_t *box = lv_obj_create(s_title_layer);
    lv_obj_set_size(box, 320, 320);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0c0c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x8a7a50), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);   /* 面板子件按盒坐标精确定位，不要主题 padding */
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

    /* 中央徽记用上游奖杯图标；加轻微上下浮动（translate_y 不破坏 CENTER 对齐） */
    lv_obj_t *crest = icon_img_create(box, DG_SHEET_ICONS, IC_TROPHY, 3, PANEL_BG);
    if (crest) {
        lv_obj_align(crest, LV_ALIGN_CENTER, 0, 10);
        lv_anim_t bob;
        lv_anim_init(&bob);
        lv_anim_set_var(&bob, crest);
        lv_anim_set_exec_cb(&bob, anim_translate_y_cb);
        lv_anim_set_values(&bob, 0, -5);
        lv_anim_set_time(&bob, 900);
        lv_anim_set_playback_time(&bob, 900);
        lv_anim_set_repeat_count(&bob, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&bob);
    }

    /* 两个主按钮：图标圆键在上、说明文字在下方（文字为 box 子对象，不叠图标） */
    lv_obj_t *bnew = icon_btn_create(box, DG_SHEET_ICONS, IC_SWORD, 52,
                                     on_title_new_cb, 0, BTN_BG);
    lv_obj_align(bnew, LV_ALIGN_BOTTOM_MID, -34, -34);
    lv_obj_t *blbl = lv_label_create(box);
    lv_label_set_text(blbl, "新冒险");
    lv_obj_set_style_text_font(blbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(blbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(blbl, LV_ALIGN_BOTTOM_MID, -34, -8);

    lv_obj_t *bload = icon_btn_create(box, DG_SHEET_ICONS, IC_RESTART, 52,
                                      on_title_load_cb, 0, BTN_BG);
    lv_obj_align(bload, LV_ALIGN_BOTTOM_MID, 34, -34);
    lv_obj_t *llbl = lv_label_create(box);
    lv_label_set_text(llbl, "继续");
    lv_obj_set_style_text_font(llbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(llbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(llbl, LV_ALIGN_BOTTOM_MID, 34, -8);
}

/* 职业表：文案对齐上游 WndClass 的英雄介绍口径（精简到两行内） */
static const struct { int sheet; dg_class_t cls; const char *name; const char *desc; } s_classes[4] = {
    { DG_SHEET_HERO_WARRIOR,  DG_CLASS_WARRIOR, "战士", "身经百战的近战勇士，起始装备更精良，越战越勇。" },
    { DG_SHEET_HERO_MAGE,     DG_CLASS_MAGE,    "法师", "驾驭奥术之焰的施法者，魔杖随使用积攒法术。" },
    { DG_SHEET_HERO_ROGUE,    DG_CLASS_ROGUE,   "盗贼", "身手敏捷的暗影行者，搜刮与探索更有效率。" },
    { DG_SHEET_HERO_HUNTRESS, DG_CLASS_HUNTER,  "猎手", "与灵鹰为伴的神射手，远程飞镖百步穿杨。" },
};

/* 进职业页时复位两段式选择态（介绍回默认、描边全灭） */
static void class_sel_reset(void)
{
    s_class_sel = -1;
    if (s_class_desc)
        lv_label_set_text(s_class_desc, "点选职业查看介绍 · 再点一次出发");
    for (int k = 0; k < 4; k++)
        if (s_class_btns[k])
            lv_obj_set_style_border_color(s_class_btns[k],
                                          lv_color_hex(0x556072), LV_PART_MAIN);
}

static void class_layer_build(void)
{
    s_class_layer = layer_create();
    lv_obj_t *t = lv_label_create(s_class_layer);
    lv_label_set_text(t, "选择职业");
    lv_obj_set_style_text_font(t, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 20);

    /* 职业介绍行（标题与卡之间）：两段式选择的第一段就是看这里 */
    s_class_desc = lv_label_create(s_class_layer);
    lv_label_set_text(s_class_desc, "点选职业查看介绍 · 再点一次出发");
    lv_obj_set_style_text_font(s_class_desc, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_class_desc, lv_color_hex(0x9a9a8a), LV_PART_MAIN);
    lv_obj_set_width(s_class_desc, 264);
    lv_obj_set_style_text_align(s_class_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_class_desc, LV_ALIGN_TOP_MID, 0, 46);

    /* 4 个头像卡沿水平排开：中心落在 x=180（此前 xs 首尾中点只有 166，
     * 整排左偏 14px，视觉上“所有东西都不在圆心”） */
    const int xs[4] = { 66, 142, 218, 294 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(s_class_layer);
        lv_obj_set_size(b, 66, 92);   /* 加高：立绘 60 + 名字行不叠字 */
        lv_obj_set_pos(b, xs[i] - 33, 150 - 46);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x14161e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_hex(0x556072), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_set_ext_click_area(b, 4);
        s_class_btns[i] = b;

        lv_obj_t *ic = hero_img_create(b, s_classes[i].sheet, 4, PANEL_BG);
        if (ic) lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_t *nl = lv_label_create(b);
        lv_label_set_text(nl, s_classes[i].name);
        lv_obj_set_style_text_font(nl, &si_yuan_black_icon_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(nl, lv_color_hex(0xd8d0a0), LV_PART_MAIN);
        lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, 2);
        lv_obj_add_event_cb(b, on_class_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
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
    /* 遮罩 80% 时背后地图透得太清楚，物品格和墙纹糊在一起显脏（用户反馈
     * 「图标混乱」观感来源之一），压到 95% */
    lv_obj_set_style_bg_opa(mask, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
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
        { "使用", IC_CHECK, 0 },
        { "装备", IC_SWORD, 1 },
        { "丢弃", IC_SWAP,  2 },
        { "关闭", IC_CLOSE, 3 },
    };
    /* 底部动作弧 4 键（关闭从顶部手势带移到这里），圆心沿底边微弧排布，
     * 说明文字移到各圆正下方（不叠图标）。坐标均落在半径 180 圆内。 */
    static const int ax[4] = { 84, 148, 212, 276 };
    static const int ay[4] = { 286, 302, 302, 286 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = icon_btn_create(mask, DG_SHEET_ICONS, acts[i].cell, 44,
                                      on_inv_action_cb, acts[i].act, BTN_BG);
        lv_obj_set_pos(b, ax[i] - 22, ay[i] - 22);
        lv_obj_t *l = lv_label_create(mask);
        lv_label_set_text(l, acts[i].name);
        lv_obj_set_style_text_font(l, &si_yuan_black_icon_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_width(l, 48);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, ax[i] - 24, ay[i] + 24);
    }
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
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
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
        lv_obj_set_pos(b, 24 + i * 68, 96);
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, m[i].name);
        lv_obj_set_style_text_font(l, &si_yuan_black_icon_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_width(l, 56);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 24 + i * 68, 96 + 56 + 2);
    }
}

/* ---- 英雄属性面板（DG_SCENE_HERO）：纯查看 overlay，数据一次拉全 ---- */

static void on_hero_close_cb(lv_event_t *e)
{
    (void)e;
    dg_api_on_button(DG_BTN_HERO);
    s_shown_scene = dg_api_current_scene();
    show_scene(s_shown_scene);
}

static void hero_layer_build(void)
{
    s_hero_layer = layer_create();
    lv_obj_t *mask = lv_obj_create(s_hero_layer);
    lv_obj_set_size(mask, DG_SCREEN_W, DG_SCREEN_H);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    /* 圆盘面板（同结算页几何：280 直径落在半径 180 屏内） */
    lv_obj_t *box = lv_obj_create(mask);
    lv_obj_set_size(box, 280, 280);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0c0c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x8a7a50), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    s_hero_title = lv_label_create(box);
    lv_label_set_text(s_hero_title, "英雄");
    lv_obj_set_style_text_font(s_hero_title, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hero_title, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_align(s_hero_title, LV_ALIGN_TOP_MID, 0, 26);

    s_hero_txt = lv_label_create(box);
    lv_label_set_text(s_hero_txt, "");
    lv_obj_set_style_text_font(s_hero_txt, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hero_txt, lv_color_hex(0xc8c8b8), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_hero_txt, 8, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hero_txt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_hero_txt, LV_ALIGN_CENTER, 0, -6);

    lv_obj_t *b = icon_btn_create(box, DG_SHEET_ICONS, IC_CLOSE, 44,
                                  on_hero_close_cb, 0, BTN_BG);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -18);
}

static void refresh_hero(void)
{
    dg_stats_t st;
    dg_api_get_stats(&st);
    static const char *k_cls[4] = { "战士", "法师", "盗贼", "猎手" };
    static const char *k_hunger[4] = { "饱食", "微饿", "饥饿", "快饿死" };
    lv_label_set_text(s_hero_title,
        (st.cls >= 0 && st.cls < 4) ? k_cls[st.cls] : "英雄");
    char b[320];
    snprintf(b, sizeof(b),
        "Lv.%d  HP %d/%d  XP %d/%d\n"
        "力量 %d   命中 %d   闪避 %d\n"
        "伤害 %d~%d   护甲 %d\n"
        "金币 %d   钥匙 %d   %s\n"
        "武器：%s\n护甲：%s\n戒指：%s",
        st.lvl, st.hp, st.hp_max, st.exp, st.exp_max,
        st.str, st.atk_skill, st.def_skill,
        st.dmg_lo, st.dmg_hi, st.armor_dr,
        st.gold, st.keys, k_hunger[st.hunger_state & 3],
        st.wep_name ? st.wep_name : "—",
        st.arm_name ? st.arm_name : "—",
        st.rng_name ? st.rng_name : "—");
    lv_label_set_text(s_hero_txt, b);
}

static void end_layer_build(lv_obj_t **out, bool win)
{
    lv_obj_t *layer = layer_create();
    *out = layer;
    /* 圆屏：结算面板同标题，用正圆盘（280 直径，半径 140 < 屏半径 180），不再是圆角方块 */
    lv_obj_t *box = lv_obj_create(layer);
    lv_obj_set_size(box, 280, 280);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    s_end_box[win ? 1 : 0] = box;      /* v0.5 入场演出用 */
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0c0c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(win ? 0xd0a040 : 0x8a3030), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
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

/* ---- v0.5 首次引导浮层：NVS 一次性标记，点「开玩」后不再弹 ---- */

static void on_guide_start_cb(lv_event_t *e)
{
    (void)e;
    dg_audio_play_sfx(DG_SFX_SELECT);
    nvs_handle_t h;
    if (nvs_open("dgdungeon", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "guide_seen", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    s_guide_active = false;
    if (s_guide_layer) lv_obj_add_flag(s_guide_layer, LV_OBJ_FLAG_HIDDEN);
}

static void guide_layer_build(void)
{
    s_guide_layer = layer_create();
    /* 圆盘面板：同标题/结算的圆形语言，盖住中部，边缘操控键仍可点（透传） */
    lv_obj_t *box = lv_obj_create(s_guide_layer);
    lv_obj_set_size(box, 320, 320);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0c0c12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x556072), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, "怎么玩");
    lv_obj_set_style_text_font(t, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, lv_color_hex(0xf0e0a0), LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *txt = lv_label_create(box);
    lv_label_set_text(txt,
        "方向键  移动 · 撞向敌人即攻击\n"
        "点地图  自动走过去 / 拾取\n"
        "点角色  原地待机回血\n"
        "右下键  属性 · 搜索 · 背包 · 菜单\n"
        "沿楼梯下到 12 层取回护身符");
    lv_obj_set_style_text_font(txt, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(txt, lv_color_hex(0xc8c8b8), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(txt, 10, LV_PART_MAIN);
    lv_obj_align(txt, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *b = lv_btn_create(box);
    lv_obj_set_size(b, 120, 44);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_radius(b, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1a2230), LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(0x8894a8), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(b, on_guide_start_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, "开玩");
    lv_obj_set_style_text_font(bl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(bl);
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
    hero_layer_build();
    menu_layer_build();
    end_layer_build(&s_over_layer, false);
    end_layer_build(&s_win_layer, true);
    guide_layer_build();          /* 最后建：浮在最上层 */

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

/* ==================== v0.5 表现层：动画助手 ==================== */

static void anim_opa_cb(void *var, int32_t v)
{ lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN); }
static void anim_bg_opa_cb(void *var, int32_t v)
{ lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_MAIN); }
static void anim_y_cb(void *var, int32_t v)
{ lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v); }
static void anim_x_cb(void *var, int32_t v)
{ lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v); }
static void anim_translate_y_cb(void *var, int32_t v)
{ lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, LV_PART_MAIN); }
static void anim_hp_opa_cb(void *var, int32_t v)
{ lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, LV_PART_INDICATOR); }
static void anim_hide_cb(lv_anim_t *a)
{ lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }

/* v8 的 style opa 不级联子对象，整层淡入需逐对象建 opa 动画（fade_tree 递归）。
 * 刻意不用 OPA_LAYERED：360 满圆离屏 layer 合成在 PSRAM 上开销过大。 */
static void fade_tree(lv_obj_t *o, uint32_t time)
{
    if (!o) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, time);
    lv_anim_start(&a);
    uint32_t i, n = lv_obj_get_child_cnt(o);
    for (i = 0; i < n; i++) fade_tree(lv_obj_get_child(o, i), time);
}

/* overlay 页（背包/菜单）入场：整树淡入 + 从底部 36px 滑上（y 位移走普通绘制路径） */
static void layer_slide_up(lv_obj_t *ov)
{
    if (!ov) return;
    fade_tree(ov, 220);
    lv_obj_set_y(ov, 36);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ov);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, 36, 0);
    lv_anim_set_time(&a, 260);
    lv_anim_start(&a);
}

/* 受击抖屏：视口短促左右抖动，结束回到原位（playback 语义保证）。
 * 只抖地图不动 HUD/按键，避免全屏眩晕。 */
static void viewport_shake(void)
{
    if (!s_viewport) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_viewport);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, DG_VP_ORIGIN, DG_VP_ORIGIN - 3);
    lv_anim_set_time(&a, 40);
    lv_anim_set_playback_time(&a, 40);
    lv_anim_set_repeat_count(&a, 4);
    lv_anim_start(&a);
}

/* 换层横幅：淡入 → 停留 → 淡出并隐藏。lv_anim_start 会删除同 var+exec
 * 的旧动画，两段必须用 ready_cb 链式衔接，不能同时挂。 */
static void banner_fade_out_cb(lv_anim_t *a)
{
    (void)a;
    if (!s_banner) return;
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, s_banner);
    lv_anim_set_exec_cb(&b, anim_opa_cb);
    lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&b, 320);
    lv_anim_set_delay(&b, 1150);       /* 淡入后的停留时长 */
    lv_anim_set_ready_cb(&b, anim_hide_cb);
    lv_anim_start(&b);
}

static void banner_show(int depth, int chapter)
{
    if (!s_banner) return;
    /* 横幅与消息 toast 同处地图上部：换层瞬间让在显 toast 立即退场，
     * 并在横幅生命周期内压制新 toast（同帧内 msg 段会把它重新弹出来，
     * 仅隐藏不够）。s_msg_last 不清：被压的消息窗口后照常弹一次。 */
    if (s_msg_lbl) {
        lv_obj_add_flag(s_msg_lbl, LV_OBJ_FLAG_HIDDEN);
        s_msg_expire_ms = 0;
    }
    s_msg_suppress_until = lv_tick_get() + 2300;
    static const char *ch_names[3] = { "下水道", "监狱", "洞穴" };
    const char *ch = (chapter >= 0 && chapter < 3) ? ch_names[chapter] : "";
    char txt[40];
    snprintf(txt, sizeof(txt), "第 %d 层 · %s", depth, ch);
    lv_label_set_text(s_banner, txt);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    lv_anim_del(s_banner, anim_opa_cb);
    lv_obj_set_style_opa(s_banner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 260);
    lv_anim_set_delay(&a, 120);
    lv_anim_set_ready_cb(&a, banner_fade_out_cb);
    lv_anim_start(&a);
}

/* 出口路点：出口已被揭雾（记忆）且没站在上面时，在出口 tile 中心呼吸。
 * 视口外/被控件区遮挡时隐藏。 */
static void waypoint_update(const dg_hud_t *hud)
{
    if (!s_waypoint) return;
    bool show = hud->exit_seen && !hud->on_stairs &&
                s_shown_scene == DG_SCENE_IN_GAME;
    if (!show) { set_hidden(s_waypoint, true); return; }
    int cam_x, cam_y;
    dg_api_get_camera(&cam_x, &cam_y);
    int tile_d = DG_TILE_PX * DG_VIEW_SCALE;          /* 32 显示像素/tile */
    int sx = DG_VP_ORIGIN + (hud->exit_x - cam_x) * tile_d + tile_d / 2;
    int sy = DG_VP_ORIGIN + (hud->exit_y - cam_y) * tile_d + tile_d / 2;
    if (sx < 24 || sx > DG_SCREEN_W - 24 || sy < 24 || sy > DG_SCREEN_H - 24) {
        set_hidden(s_waypoint, true);
        return;
    }
    lv_obj_set_pos(s_waypoint, sx - 15, sy - 15);
    set_hidden(s_waypoint, false);
}

static void show_scene(dg_scene_t scene)
{
    set_hidden(s_title_layer, scene != DG_SCENE_TITLE);
    set_hidden(s_class_layer, scene != DG_SCENE_CLASS_SELECT);
    set_hidden(s_inv_layer,   scene != DG_SCENE_INVENTORY);
    set_hidden(s_hero_layer,  scene != DG_SCENE_HERO);
    set_hidden(s_menu_layer,  scene != DG_SCENE_MENU);
    set_hidden(s_over_layer,  scene != DG_SCENE_GAME_OVER);
    set_hidden(s_win_layer,   scene != DG_SCENE_WIN);

    bool in_dungeon = (scene == DG_SCENE_IN_GAME || scene == DG_SCENE_INVENTORY ||
                       scene == DG_SCENE_HERO ||
                       scene == DG_SCENE_MENU || scene == DG_SCENE_GAME_OVER ||
                       scene == DG_SCENE_WIN);
    set_hidden(s_g_layer, !in_dungeon);
    /* HUD / toolbar：仅纯游戏态显示；overlay 页把 toolbar 藏起，HUD 保留在死亡页也行 */
    if (s_toolbar) set_hidden(s_toolbar, scene != DG_SCENE_IN_GAME);
    /* HUD 只在纯游戏态显示：背包页盖住它（否则标题与深度文字叠字），
     * 菜单/结算页是居中弹窗不挡图，留 HUD 反而有“在看状态栏”的游戏感 */
    if (s_hud)     set_hidden(s_hud, !(scene == DG_SCENE_IN_GAME || scene == DG_SCENE_MENU));

    /* v0.5 转场：瞬间 show/hide 是「不像游戏」观感的来源之一。
     * 各页入场动画见上方助手；背景色 / bg_opa 均走普通绘制路径，无离屏 layer。 */
    if (scene == DG_SCENE_TITLE)
        fade_tree(s_title_layer, 260);
    else if (scene == DG_SCENE_CLASS_SELECT) {
        fade_tree(s_class_layer, 260);
        class_sel_reset();
    }
    else if (scene == DG_SCENE_INVENTORY || scene == DG_SCENE_MENU || scene == DG_SCENE_HERO) {
        lv_obj_t *up = (scene == DG_SCENE_INVENTORY) ? s_inv_layer
                   : (scene == DG_SCENE_MENU) ? s_menu_layer : s_hero_layer;
        layer_slide_up(up);
    }
    else if (scene == DG_SCENE_GAME_OVER || scene == DG_SCENE_WIN) {
        bool win = (scene == DG_SCENE_WIN);
        lv_obj_t *ov = win ? s_win_layer : s_over_layer;
        /* 红/金氛围雾：层 bg 从透明压到 70%，再整树淡入圆盘 */
        lv_obj_set_style_bg_color(ov, lv_color_hex(win ? 0x2a2208 : 0x2a0a0a), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_anim_t vg;
        lv_anim_init(&vg);
        lv_anim_set_var(&vg, ov);
        lv_anim_set_exec_cb(&vg, anim_bg_opa_cb);
        lv_anim_set_values(&vg, LV_OPA_TRANSP, LV_OPA_70);
        lv_anim_set_time(&vg, 420);
        lv_anim_start(&vg);
        fade_tree(ov, 380);
    }

    if (scene == DG_SCENE_INVENTORY) refresh_inventory();
    if (scene == DG_SCENE_HERO) refresh_hero();
    /* 换层横幅状态复位：回标题后新局重新触发「第 1 层」 */
    if (scene == DG_SCENE_TITLE) s_last_depth = 0;

    /* 首次进入游戏：弹一次操作引导（NVS 持久化，点「开玩」后不再出现；
     * 未关闭时去背包/菜单再回来会重新弹出，直到被确认）。
     * 注意：NVS 检查必须先于显隐计算——首次进 IN_GAME 时才置位
     * s_guide_active，若后算则本次 show_scene 永远看不到它。 */
    if (scene == DG_SCENE_IN_GAME && !s_guide_done) {
        s_guide_done = true;
        /* key 不存在 = 新用户：nvs_get 不写入 out 参数，初值必须为 0
         * （曾误初始化为 1，导致引导永远不弹） */
        uint8_t seen = 0;
        nvs_handle_t h;
        if (nvs_open("dgdungeon", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "guide_seen", &seen);
            nvs_close(h);
        }
        if (!seen) s_guide_active = true;
    }
    if (s_guide_layer) {
        bool guide_show = s_guide_active && scene == DG_SCENE_IN_GAME;
        set_hidden(s_guide_layer, !guide_show);
        if (guide_show) fade_tree(s_guide_layer, 240);
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
                      sc == DG_SCENE_HERO ||
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

                /* ---- v0.5 表现层驱动 ---- */
                /* 受击抖屏：两次 HUD 刷新间血量下降 → 视口短抖（拾金/治疗不触发） */
                if (s_prev_hp >= 0 && hud.hp < s_prev_hp) viewport_shake();
                s_prev_hp = hud.hp;
                /* 低血告警：≤30% 血条指示器闪烁，回升后停止 */
                bool hp_low = (hud.hp > 0 && hud.hp * 3 <= hud.hp_max);
                if (hp_low != s_hp_low) {
                    s_hp_low = hp_low;
                    if (hp_low) {
                        lv_anim_t bl;
                        lv_anim_init(&bl);
                        lv_anim_set_var(&bl, s_hp_bar);
                        lv_anim_set_exec_cb(&bl, anim_hp_opa_cb);
                        lv_anim_set_values(&bl, LV_OPA_COVER, LV_OPA_30);
                        lv_anim_set_time(&bl, 300);
                        lv_anim_set_playback_time(&bl, 300);
                        lv_anim_set_repeat_count(&bl, LV_ANIM_REPEAT_INFINITE);
                        lv_anim_start(&bl);
                    } else {
                        lv_anim_del(s_hp_bar, anim_hp_opa_cb);
                        lv_obj_set_style_opa(s_hp_bar, LV_OPA_COVER, LV_PART_INDICATOR);
                    }
                }
                /* 出口路点 + 换层横幅（开局/读档下楼都会触发） */
                waypoint_update(&hud);
                if (hud.depth != s_last_depth) {
                    s_last_depth = hud.depth;
                    banner_show(hud.depth, hud.chapter);
                }
            }
            char msg[96];
            if (dg_api_get_message(msg, sizeof(msg), 0) > 0) {
                /* 只在“换了新消息”时重置 toast，同一句不反复刷新计时；
                 * 横幅显示期间压制：不弹也不记 s_msg_last（记了窗口后
                 * strcmp 相同就永远不弹了），窗口结束后照常弹一次 */
                if (strcmp(msg, s_msg_last) != 0 && now >= s_msg_suppress_until) {
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
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= 4) return;
    /* 两段式：第一次点只看介绍 + 高亮描边（上游 WndClass 也是先展示
     * 描述再确认），再点同一职业才真正出发 */
    if (s_class_sel != i) {
        s_class_sel = i;
        if (s_class_desc) {
            lv_label_set_text(s_class_desc, s_classes[i].desc);
            lv_obj_set_style_text_color(s_class_desc, lv_color_hex(0xf0e0a0),
                                        LV_PART_MAIN);
        }
        for (int k = 0; k < 4; k++)
            if (s_class_btns[k])
                lv_obj_set_style_border_color(s_class_btns[k],
                    k == i ? lv_color_hex(0xf0d060) : lv_color_hex(0x556072),
                    LV_PART_MAIN);
        return;
    }
    dg_api_pick_class(s_classes[i].cls);
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
    if (act == 3) {                       /* 关闭：不需选中物品 */
        dg_api_on_button(DG_BTN_INVENTORY);
        s_shown_scene = dg_api_current_scene();
        show_scene(s_shown_scene);
        return;
    }
    if (s_inv_sel < 0) { dg_audio_play_sfx(DG_SFX_ERROR); return; }
    /* 执行前先拿物品快照：结果写进详情行。此前成功只播个音，
     * 面板内又看不到 toast，玩家以为按钮坏了（用户反馈） */
    dg_item_info_t it;
    bool have = dg_api_inv_get(s_inv_sel, &it);
    const char *nm = (have && it.name) ? it.name : "?";
    bool was_eq = have && it.equipped;
    bool ok = false;
    switch (act) {
    case 0: ok = dg_api_inv_use(s_inv_sel);   break;
    case 1: ok = dg_api_inv_equip(s_inv_sel); break;
    case 2: ok = dg_api_inv_drop(s_inv_sel);  break;
    }
    char fb[128];
    if (ok) {
        if (act == 2) s_inv_sel = -1;
        refresh_inventory();
        if (act == 0)      snprintf(fb, sizeof(fb), "已使用：%s", nm);
        else if (act == 1) snprintf(fb, sizeof(fb), "%s：%s", was_eq ? "已卸下" : "已装备", nm);
        else               snprintf(fb, sizeof(fb), "已丢弃：%s", nm);
    } else {
        /* 失败优先透传引擎消息（如「你实在吃不下了」），比笼统文案清楚 */
        char msg[96];
        if (act == 1 && have && it.cursed) snprintf(fb, sizeof(fb), "诅咒物品，无法卸下");
        else if (dg_api_get_message(msg, sizeof(msg), 0) > 0)
            snprintf(fb, sizeof(fb), "%s", msg);
        else
            snprintf(fb, sizeof(fb), "这个物品不能这样用");
    }
    lv_label_set_text(s_inv_detail, fb);
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
    if (c == 'u') { ESP_LOGI(TAG, "ext_cmd: u (M6 fullrun)"); dg_api_debug_m6(); return; }      /* 取证：M6 全程通关链路 */

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
    case 'o': dg_api_on_button(DG_BTN_HERO);      break;
    case 'r': dg_api_on_button(DG_BTN_DESCEND);   break;
    case 'f': dg_api_on_button(DG_BTN_EQUIP);     break;
    case 'm': dg_api_on_button(DG_BTN_MENU);      break;
    default:
        ESP_LOGI(TAG, "ext_cmd: %c (h g 1-4 w a x d q e j r f m t n v)", c);
        break;
    }
}
