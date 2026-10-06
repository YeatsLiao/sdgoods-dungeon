/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * ui_dungeon.c —— LVGL 首屏装配（v0.3 圆屏适配版）
 *
 * 圆屏 360×360 布局（满圆设计：地图铺满整个可见圆，HUD 浮在上面）：
 *   [主视窗 352×352 居中]  原点 (4,4)，22×22 tile。半侧 176 < 半径 180，
 *                          赤道弦铺满；四角出圆部分被物理边框吃掉，不浪费可见像素
 *   [HUD 胶囊]              状态行 y=24 / 消息行 y=48，半透黑底 + 圆角，
 *                          浮在画布上（圆屏顶部无专用文字带，只能叠画）
 *   [底部圆弧悬浮 3 颗圆键] 待/搜/包，d=52 半透明白（压在画布 z 上层）
 *   [背包/死亡 overlay]     scene 驱动，黑 90% 浮层 + 中文 stats
 *
 * 圆屏红线（docs/README：SDG_UI_SAFE_*）：任何可点控件的矩形必须完整落在圆内；
 * 顶部 y<50 是控制中心下滑捕获带，那里不放可点元素（只放只读文字）。
 *
 * 与引擎的边界：本文件只 #include "dungeon_api.h"（C ABI），不 #include 任何 C++ 头。
 */

#include "ui_dungeon.h"
#include "dungeon_api.h"          /* 引擎 C ABI 边界 */
#include "sdgoods_tap.h"          /* sdgoods_tap_synth：串口调试注入（ext_cmd） */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

/* 平台中文子集字体（bsp/fonts，tools/gen_fonts.py 生成）：
 * si_yuan 主字体只含 UI 精选集，编译期 .fallback 指向 cn_font 全量集，
 * 游戏文案缺字自动回退，新增文案后重跑 gen_fonts.py 即可。 */
LV_FONT_DECLARE(si_yuan_black_icon_14);

static const char *TAG = "ui_dungeon";

/* ---- 圆屏几何常量（360 直径，安全内接方形推导见各注释）---- */
#define DG_SCREEN_W           360
#define DG_SCREEN_H           360
#define DG_VIEWPORT_SIZE      DG_VIEWPORT_W      /* 352，与引擎常量同源 */
#define DG_VP_ORIGIN         ((DG_SCREEN_W - DG_VIEWPORT_SIZE) / 2)   /* 4 */
#define DG_TILE_SIZE          DG_TILE_PX
#define DG_VIEW_TILES         DG_VIEW_TILE_W     /* 22×22 */
#define DG_BTN_D              52                 /* 悬浮键直径（DOOM 同款命中率基准） */

/* ---- 首屏控件句柄 ---- */
static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_msg_lbl    = NULL;      /* 消息行（引擎 log 最新一条） */
static lv_obj_t *s_viewport   = NULL;      /* 主 tilemap 图 */
static lv_obj_t *s_overlay    = NULL;      /* 背包/死亡浮层 */
static lv_obj_t *s_overlay_lbl = NULL;

static bool s_running = false;
static uint32_t s_last_status_ms = 0;
static dg_scene_t s_shown_scene = DG_SCENE_TITLE;   /* overlay 当前呈现的 scene */

/* 状态栏文字缓冲（引擎写入，UI 拷贝到 LVGL） */
static char s_status_buf[64];

/* 主视窗 framebuffer（引擎持有，UI 只读引用）。
 * 352×352×2 = 248KB 放不了内部 SRAM，显示缓存走 PSRAM（DOOM 同方案）。 */
static uint16_t *s_display_fb = NULL;
static lv_img_dsc_t s_viewport_dsc = {
    .header.cf    = LV_IMG_CF_TRUE_COLOR,     /* RGB565 */
    .header.always_zero = 0,
    .header.w     = DG_VIEWPORT_SIZE,
    .header.h     = DG_VIEWPORT_SIZE,
    .data_size    = DG_VIEWPORT_SIZE * DG_VIEWPORT_SIZE * sizeof(uint16_t),
    .data         = NULL,
};

/* 引擎 fb 约定：native 小端 RGB565。而本平台 LVGL 配了 LV_COLOR_16_SWAP=1
 *（RGB 面板高字节在前，与 FACEENGINE 基线一致），TRUE_COLOR 图像数据须
 * 按交换字节序存放。边界转换在 UI 做（引擎不感知显示字节序）；
 * bswap32 一次换两像素。 */
static void copy_fb_to_display_order(const uint32_t *src, uint32_t *dst, int words)
{
    for (int i = 0; i < words; i++) {
        dst[i] = __builtin_bswap32(src[i]);
    }
}

/* ---- 前向声明 ---- */
static void on_hotbar_btn_cb(lv_event_t *e);
static void on_viewport_click_cb(lv_event_t *e);
static void on_overlay_click_cb(lv_event_t *e);
static void paint_placeholder_checkerboard(void);
static void overlay_show(dg_scene_t scene);

/* ==================== 构建 ==================== */

void ui_dungeon_start(void)
{
    ESP_LOGI(TAG, "ui_dungeon_start (v0.3 round-screen)");

    s_display_fb = heap_caps_malloc(DG_VIEWPORT_SIZE * DG_VIEWPORT_SIZE * sizeof(uint16_t),
                                    MALLOC_CAP_SPIRAM);
    if (!s_display_fb) {
        ESP_LOGE(TAG, "display fb PSRAM alloc fail");
        s_display_fb = calloc(DG_VIEWPORT_SIZE * DG_VIEWPORT_SIZE, sizeof(uint16_t));
    }
    s_viewport_dsc.data = (const uint8_t *)s_display_fb;

    /* 首屏对象 */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0a0a0a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. 主视窗 352×352 居中（满圆）—— 先建，保证后续控件/按键天然在其上层 */
    paint_placeholder_checkerboard();
    s_viewport = lv_img_create(s_scr);
    lv_img_set_src(s_viewport, &s_viewport_dsc);
    lv_obj_set_pos(s_viewport, DG_VP_ORIGIN, DG_VP_ORIGIN);
    lv_obj_add_flag(s_viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_viewport, on_viewport_click_cb, LV_EVENT_CLICKED, NULL);

    /* 2. HUD 胶囊：满圆画布上没有专用文字带，状态/消息以半透黑胶囊叠在
     *    画布上方。宽度按圆弦收窄（y=24 处可用弦宽 ~195px，y=48 处 ~245px），
     *    否则字会转到圆外看不见。顶部 y<50 手势带内依旧不放可点元素。 */
    s_status_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_status_lbl, "生命 20/20 · 1层 · 金币 0");
    lv_obj_set_style_text_font(s_status_lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_lbl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_lbl, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(s_status_lbl, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_status_lbl, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_status_lbl, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_status_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 22);

    s_msg_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_msg_lbl, "");
    lv_obj_set_style_text_font(s_msg_lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_msg_lbl, lv_color_hex(0xd8d0a0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_msg_lbl, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_msg_lbl, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg_lbl, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_msg_lbl, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg_lbl, 0, LV_PART_MAIN);
    lv_obj_set_width(s_msg_lbl, 216);                       /* 含 pad：弦内安全宽 */
    lv_obj_set_style_max_height(s_msg_lbl, 46, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_msg_lbl, LV_TEXT_ALIGN_CENTER, 0);   /* v8 兼容层：对齐走 text style */
    lv_obj_clear_flag(s_msg_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_msg_lbl, LV_ALIGN_TOP_MID, 0, 48);

    /* 引擎开局：boot 即新游戏（标题/选职业界面 v0.4 补），seed 用开机时间微秒 */
    uint32_t seed = (uint32_t)esp_timer_get_time();
    dg_api_new_game(DG_CLASS_WARRIOR, seed);

    /* 3. 底部圆弧悬浮 3 键（DOOM 白色悬浮方案：d=52、半透明白、白描边；
     *    点击移动是主交互，这里只留真正改变回合策略的键）
     *    圆心 y=280（距屏心 100，弦半宽 149 > 115+26 外键边缘，完整在圆内） */
    static const struct {
        const char *label;
        dg_btn_id_t btn;
        int dx;                     /* 相对屏心 x 偏移 */
    } hotkeys[] = {
        { "待", DG_BTN_WAIT,      -115 },
        { "搜", DG_BTN_SEARCH,      0 },
        { "包", DG_BTN_INVENTORY, 115 },
    };
    for (unsigned i = 0; i < sizeof(hotkeys) / sizeof(hotkeys[0]); i++) {
        lv_obj_t *b = lv_btn_create(s_scr);
        lv_obj_set_size(b, DG_BTN_D, DG_BTN_D);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_align(b, LV_ALIGN_CENTER, hotkeys[i].dx, 100);   /* y=280 */

        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, hotkeys[i].label);
        lv_obj_set_style_text_font(lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(b, on_hotbar_btn_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)hotkeys[i].btn);
    }

    /* 4. 背包/死亡 overlay（默认藏，scene 驱动） */
    s_overlay = lv_obj_create(s_scr);
    lv_obj_set_size(s_overlay, 236, 200);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(s_overlay, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_overlay, lv_color_hex(0x8a7a50), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_overlay, 2, LV_PART_MAIN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, on_overlay_click_cb, LV_EVENT_CLICKED, NULL);
    s_overlay_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_overlay_lbl, &si_yuan_black_icon_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_overlay_lbl, lv_color_hex(0xd8d0a0), LV_PART_MAIN);
    lv_obj_center(s_overlay_lbl);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_scr_load(s_scr);
    s_running = true;
    s_shown_scene = dg_api_current_scene();
}

bool ui_dungeon_is_running(void)
{
    return s_running;
}

/* ==================== 逐帧 poll ==================== */

void ui_dungeon_poll(void)
{
    if (!s_running) return;

    /* 引擎回合状态推进（若有 pending 输入或 A* 寻路分帧） */
    dg_api_tick_if_needed();

    /* overlay 跟随 scene（切换时才动对象树，避免每帧 invalidate） */
    dg_scene_t sc = dg_api_current_scene();
    if (sc != s_shown_scene) {
        s_shown_scene = sc;
        overlay_show(sc);
    }

    /* 状态栏文字 + 消息行 300ms 刷新一次（视窗变大后仍低开销） */
    uint32_t now = lv_tick_get();
    if (now - s_last_status_ms >= 300) {
        s_last_status_ms = now;
        if (dg_api_get_status_text(s_status_buf, sizeof(s_status_buf)) > 0) {
            lv_label_set_text(s_status_lbl, s_status_buf);
        }
        char msg[96];
        if (dg_api_get_message(msg, sizeof(msg), 0) > 0) {
            lv_label_set_text(s_msg_lbl, msg);
        }
        if (sc == DG_SCENE_INVENTORY || sc == DG_SCENE_GAME_OVER) {
            static char stats[256];
            if (dg_api_get_stats_text(stats, sizeof(stats)) > 0) {
                const char *tail = (sc == DG_SCENE_GAME_OVER)
                                   ? "\n—— 点按此处重新开始 ——" : "\n—— 再按 包 关闭 ——";
                strncat(stats, tail, sizeof(stats) - strlen(stats) - 1);
                lv_label_set_text(s_overlay_lbl, stats);
            }
        }
    }

    /* 主视窗：引擎返回非 NULL = 地图脏，转显示字节序后 invalidate */
    int fb_w = 0, fb_h = 0;
    const uint16_t *fb = dg_api_get_tilemap_fb(&fb_w, &fb_h);
    if (fb && fb_w == DG_VIEWPORT_SIZE && fb_h == DG_VIEWPORT_SIZE) {
        copy_fb_to_display_order((const uint32_t *)fb, (uint32_t *)s_display_fb,
                                 DG_VIEWPORT_SIZE * DG_VIEWPORT_SIZE / 2);
        lv_obj_invalidate(s_viewport);
    }
}

/* ==================== overlay ==================== */

static void overlay_show(dg_scene_t scene)
{
    if (scene == DG_SCENE_INVENTORY || scene == DG_SCENE_GAME_OVER) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        lv_obj_invalidate(s_overlay);
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_overlay_click_cb(lv_event_t *e)
{
    (void)e;
    /* 死亡画面点按 = 重开（菜单键的快捷替身，玩家直觉路径） */
    if (dg_api_current_scene() == DG_SCENE_GAME_OVER) {
        dg_api_on_button(DG_BTN_MENU);
        s_shown_scene = dg_api_current_scene();
        overlay_show(s_shown_scene);
    }
}

/* ==================== 输入回调 ==================== */

static void on_hotbar_btn_cb(lv_event_t *e)
{
    dg_btn_id_t btn = (dg_btn_id_t)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "hotbar btn %d tapped", (int)btn);
    dg_api_on_button(btn);
}

static void on_viewport_click_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_viewport) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t r;
    lv_obj_get_coords(s_viewport, &r);
    int px = pt.x - r.x1;
    int py = pt.y - r.y1;
    ESP_LOGI(TAG, "viewport tap px=(%d,%d)", px, py);
    dg_api_on_viewport_tap(px, py);
}

/* ==================== 辅助绘制 ==================== */

static void paint_placeholder_checkerboard(void)
{
    /* 16×16 tile 视野用 8 色循环棋盘填充（无素材时的降级画面），
     * 中央画"玩家"红点 —— 用于肉眼确认视窗几何与刷新链路。 */
    static const uint16_t palette[8] = {
        0x4A22, /* 深绿  */
        0x6B4E, /* 土黄  */
        0x8C31, /* 灰   */
        0x2965, /* 蓝   */
        0x52A0, /* 棕   */
        0x0861, /* 深红 */
        0x39E7, /* 米   */
        0x0000, /* 黑（墙）*/
    };
    for (int y = 0; y < DG_VIEWPORT_SIZE; y++) {
        for (int x = 0; x < DG_VIEWPORT_SIZE; x++) {
            int tile_idx = (x / DG_TILE_SIZE) + (y / DG_TILE_SIZE) * DG_VIEW_TILES;
            s_display_fb[y * DG_VIEWPORT_SIZE + x] = palette[tile_idx % 8];
        }
    }
    int cx = DG_VIEWPORT_SIZE / 2, cy = DG_VIEWPORT_SIZE / 2;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (dx*dx + dy*dy <= 9) {
                s_display_fb[(cy + dy) * DG_VIEWPORT_SIZE + (cx + dx)] = 0xF800;
            }
        }
    }
    /* 占位图同样转成 LVGL 显示字节序（引擎 fb 到来前也颜色正确） */
    copy_fb_to_display_order((const uint32_t *)s_display_fb,
                             (uint32_t *)s_display_fb,
                             DG_VIEWPORT_SIZE * DG_VIEWPORT_SIZE / 2);
}

/* ==================== 串口调试注入（覆盖 bsp 弱符号） ====================
 * 用途：无手指条件下真机验证「点击移动 → 砍怪 → 拾金 → 下楼」链路，
 * 配合 tools/screenshot_recv.py 截屏取证。tap_synth 内部投 LVGL 线程，
 * 走真实触摸事件路径（比直调引擎更接近玩家行为）。
 *   w/a/x/d   向上/左/下/右走一步（x 代替 s：'s' 被截屏占用）
 *   q=等待   e=搜索   j=背包开关
 *   u/i/o/k   画圆内四个斜向探针 → 触发 A* 自动寻路（长途探索用）
 *             （不能用视口真四角：352 画布角落在圆外，且右下角会误触“包”键）
 *   t + "x,y;" 坐标模式：下一串数字当作屏幕像素坐标直接合成点击
 *             （配合截屏可精确定向金币/楼梯/怪，例：t175,268;）
 *   n + "x,y;" 同上但给 tile 坐标（直推引擎，不受相机限制）
 *   v         导出一行关卡关键坐标（日志以 "D h=.." 开头，PC 脚本据此定向）
 */
void sdgoods_console_ext_cmd(char c)
{
    /* 坐标模式：'t'/'n' 开启，累积 '0'-'9' 与 ','，遇 ';' 解析并注入点击 */
    static char s_coord[24];
    static char s_coord_mode = 0;      /* 0 = 未进入，'t' = 像素，'n' = tile */
    if (s_coord_mode) {
        if ((c >= '0' && c <= '9') || c == ',') {
            if (strlen(s_coord) < sizeof(s_coord) - 1) {
                strncat(s_coord, &c, 1);
            }
            return;
        }
        char *comma = strchr(s_coord, ',');
        if (comma && (c == ';' || c == '\n')) {
            *comma = '\0';
            int x = atoi(s_coord);
            int y = atoi(comma + 1);
            if (s_coord_mode == 't') {
                ESP_LOGI(TAG, "coord tap px (%d,%d)", x, y);
                sdgoods_tap_synth(x, y, 0, 0, 1);
            } else {
                ESP_LOGI(TAG, "coord tap tile (%d,%d)", x, y);
                dg_api_on_tap(x, y);
            }
        } else {
            ESP_LOGW(TAG, "coord tap bad: '%s' end='%c'", s_coord, c);
        }
        s_coord_mode = 0;
        return;
    }
    if (c == 't' || c == 'n') {
        s_coord_mode = c;
        s_coord[0] = '\0';
        return;
    }
    if (c == 'v') {
        char dump[160];
        if (dg_api_debug_dump(dump, sizeof(dump)) > 0) {
            ESP_LOGI(TAG, "%s", dump);
        }
        return;
    }

    int hx = 0, hy = 0, cx = 0, cy = 0;
    dg_api_get_hero_pos(&hx, &hy);
    dg_api_get_camera(&cx, &cy);
    /* 英雄屏幕像素 = 视口原点 + (hero - cam) * TILE + 半格 */
    int scx = DG_VP_ORIGIN + (hx - cx) * DG_TILE_SIZE + DG_TILE_SIZE / 2;
    int scy = DG_VP_ORIGIN + (hy - cy) * DG_TILE_SIZE + DG_TILE_SIZE / 2;
    /* 斜向探针：距屏心 116/116（半径 164 < 180 在圆内），下探针取 y=236
     * 以避开 y>=254 的“包”悬浮键（否则长途注入会顺手打开背包） */
    const int probe_min = DG_SCREEN_W / 2 - 116;         /* 64  */
    const int probe_max = DG_SCREEN_W / 2 + 116;         /* 296 */
    const int probe_low = DG_SCREEN_H / 2 + 56;          /* 236 */

    switch (c) {
    case 'w': sdgoods_tap_synth(scx, scy - DG_TILE_SIZE, 0, 0, 1); break;
    case 'a': sdgoods_tap_synth(scx - DG_TILE_SIZE, scy, 0, 0, 1); break;
    case 'x': sdgoods_tap_synth(scx, scy + DG_TILE_SIZE, 0, 0, 1); break;
    case 'd': sdgoods_tap_synth(scx + DG_TILE_SIZE, scy, 0, 0, 1); break;
    case 'u': sdgoods_tap_synth(probe_min, probe_min, 0, 0, 1);    break;
    case 'i': sdgoods_tap_synth(probe_max, probe_min, 0, 0, 1);    break;
    case 'o': sdgoods_tap_synth(probe_max, probe_low, 0, 0, 1);    break;
    case 'k': sdgoods_tap_synth(probe_min, probe_low, 0, 0, 1);    break;
    case 'q': dg_api_on_button(DG_BTN_WAIT);      break;
    case 'e': dg_api_on_button(DG_BTN_SEARCH);    break;
    case 'j': dg_api_on_button(DG_BTN_INVENTORY); break;
    default:
        ESP_LOGI(TAG, "ext_cmd unused: '%c' (w/a/x/d u i o k q e j t)", c);
        break;
    }
}
