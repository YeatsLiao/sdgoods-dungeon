/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * ui_dungeon.c —— LVGL 首屏装配（骨架版）
 *
 * 圆屏 360×360 布局：
 *   [顶部状态栏 30px] 让开 CC 手势带，只放 HP / 深度 / 金币文字（只读，不放按钮）
 *   [中部地牢视窗 200×200] 主 tilemap，12×12 视野 × 16px/tile
 *   [底部 6 颗等大圆键]   沿用 sdgoods-doom 的白悬浮圆键方案
 *
 * 与引擎的边界：本文件只 #include "dungeon_api.h"（C ABI），不 #include 任何 C++ 头。
 */

#include "ui_dungeon.h"
#include "dungeon_api.h"          /* 引擎 C ABI 边界 */
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "ui_dungeon";

/* ---- 圆屏几何常量（与 bsp 的 SDG_UI_SAFE 一致）---- */
#define DG_SCREEN_W           360
#define DG_SCREEN_H           360
#define DG_STATUS_H           30          /* 顶部状态栏高度，避开 CC 手势带 */
#define DG_VIEWPORT_SIZE      200         /* 主视窗边长 */
#define DG_TILE_SIZE          16          /* 屏上 tile 像素（Shattered 原版 8px × 2 缩放）*/
#define DG_VIEW_TILES         (DG_VIEWPORT_SIZE / DG_TILE_SIZE)  /* 12×12 */
#define DG_HOTBAR_H           60          /* 底部 6 圆键区高度 */
#define DG_BTN_RADIUS         22          /* 单键直径 */

/* ---- 首屏控件句柄 ---- */
static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_msg_lbl    = NULL;      /* 视窗下方消息行（引擎 log 最新一条） */
static lv_obj_t *s_viewport   = NULL;      /* 主 tilemap 图 */
static lv_obj_t *s_hotbar_btn[DG_BTN_COUNT] = {0};

static bool s_running = false;
static uint32_t s_last_status_ms = 0;

/* 状态栏文字缓冲（引擎写入，UI 拷贝到 LVGL） */
static char s_status_buf[64];

/* 主视窗 framebuffer（引擎持有，UI 只读引用）。
 * 骨架版先给一个静态棋盘做占位，跑通 LVGL img 更新链路。 */
static uint16_t s_placeholder_fb[DG_VIEWPORT_SIZE * DG_VIEWPORT_SIZE];
static lv_img_dsc_t s_viewport_dsc = {
    .header.cf    = LV_IMG_CF_TRUE_COLOR,     /* RGB565 */
    .header.always_zero = 0,
    .header.w     = DG_VIEWPORT_SIZE,
    .header.h     = DG_VIEWPORT_SIZE,
    .data_size    = sizeof(s_placeholder_fb),
    .data         = (const uint8_t *)s_placeholder_fb,
};

/* 引擎 fb 约定：native 小端 RGB565。而本平台 LVGL 配了 LV_COLOR_16_SWAP=1
 *（RGB 面板高字节在前，与 FACEENGINE 基线一致），TRUE_COLOR 图像数据须
 * 按交换字节序存放。边界转换在 UI 做（引擎不感知显示字节序）；
 * bswap32 一次换两像素，200×200 共 20K 条指令，可忽略。 */
static void copy_fb_to_display_order(const uint32_t *src, uint32_t *dst, int words)
{
    for (int i = 0; i < words; i++) {
        dst[i] = __builtin_bswap32(src[i]);
    }
}

/* ---- 前向声明 ---- */
static void on_hotbar_btn_cb(lv_event_t *e);
static void on_viewport_click_cb(lv_event_t *e);
static void paint_placeholder_checkerboard(void);

/* ==================== 构建 ==================== */

void ui_dungeon_start(void)
{
    ESP_LOGI(TAG, "ui_dungeon_start");

    /* 首屏对象 */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0a0a0a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. 顶部状态栏 —— 只读文字，任何可点击元素都不放在这里（避开 CC 手势带） */
    s_status_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_status_lbl, "HP ████▒▒  Depth: 1F  Lv 1  Gold: 0");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_width(s_status_lbl, DG_SCREEN_W - 40);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 12);

    /* 2. 中部主视窗 —— 200×200 的 RGB565 图；可点击，像素坐标直接送引擎
     *（视口像素→tile 换算在引擎内做，UI 不感知相机） */
    paint_placeholder_checkerboard();
    s_viewport = lv_img_create(s_scr);
    lv_img_set_src(s_viewport, &s_viewport_dsc);
    lv_obj_align(s_viewport, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_viewport, on_viewport_click_cb, LV_EVENT_CLICKED, NULL);

    /* 2b. 消息行 —— 状态栏与视窗夹缝居中，显示引擎消息 log 最新一条 */
    s_msg_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_msg_lbl, "");
    lv_obj_set_style_text_color(s_msg_lbl, lv_color_hex(0xd8d0a0), LV_PART_MAIN);
    lv_obj_align(s_msg_lbl, LV_ALIGN_TOP_MID, 0, DG_STATUS_H + 12);

    /* 引擎开局：boot 即新游戏（标题/选职业界面 v0.3 补），seed 用开机时间微秒 */
    uint32_t seed = (uint32_t)esp_timer_get_time();
    dg_api_new_game(DG_CLASS_WARRIOR, seed);

    /* 3. 底部 6 颗等大圆键 —— 沿用 DOOM 白悬浮方案 */
    static const char *btn_labels[DG_BTN_COUNT] = {
        "背", "法", "装", "搜", "待", "菜",
    };
    for (int i = 0; i < DG_BTN_COUNT; i++) {
        lv_obj_t *b = lv_btn_create(s_scr);
        lv_obj_set_size(b, DG_BTN_RADIUS * 2, DG_BTN_RADIUS * 2);
        /* 圆形 + 半透明白底（DOOM 决策：视觉拼接十字键已放弃，等大分离更清晰）*/
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);

        /* 6 键水平居中排布，间距 20px */
        int total_w = DG_BTN_COUNT * DG_BTN_RADIUS * 2 + (DG_BTN_COUNT - 1) * 20;
        int start_x = (DG_SCREEN_W - total_w) / 2 + DG_BTN_RADIUS;
        lv_obj_set_x(b, start_x + i * (DG_BTN_RADIUS * 2 + 20) - DG_BTN_RADIUS);
        lv_obj_set_y(b, DG_SCREEN_H / 2 - 60);
        lv_obj_align(b, LV_ALIGN_CENTER,
                     (start_x + i * (DG_BTN_RADIUS * 2 + 20) - DG_BTN_RADIUS) - DG_SCREEN_W / 2 + DG_BTN_RADIUS,
                     DG_SCREEN_H / 2 - 60);

        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, btn_labels[i]);
        lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(b, on_hotbar_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        s_hotbar_btn[i] = b;
    }

    lv_scr_load(s_scr);
    s_running = true;
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

    /* 状态栏文字 + 消息行 500ms 刷新一次（避免 LVGL 频繁 invalidate） */
    uint32_t now = lv_tick_get();
    if (now - s_last_status_ms >= 500) {
        s_last_status_ms = now;
        if (dg_api_get_status_text(s_status_buf, sizeof(s_status_buf)) > 0) {
            lv_label_set_text(s_status_lbl, s_status_buf);
        }
        char msg[64];
        if (dg_api_get_message(msg, sizeof(msg), 0) > 0) {
            lv_label_set_text(s_msg_lbl, msg);
        }
    }

    /* 主视窗：若引擎返回非 NULL 则说明地图脏了，拷贝到 placeholder_fb 触发 invalidate
     * 骨架版暂时只画棋盘，等素材管线跑通后接真实 tilemap framebuffer */
    int fb_w = 0, fb_h = 0;
    const uint16_t *fb = dg_api_get_tilemap_fb(&fb_w, &fb_h);
    if (fb && fb_w == DG_VIEWPORT_SIZE && fb_h == DG_VIEWPORT_SIZE) {
        copy_fb_to_display_order((const uint32_t *)fb, (uint32_t *)s_placeholder_fb,
                                 sizeof(s_placeholder_fb) / 4);
        lv_obj_invalidate(s_viewport);
    }
}

/* ==================== 输入回调 ==================== */

static void on_hotbar_btn_cb(lv_event_t *e)
{
    int btn = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "hotbar btn %d tapped", btn);
    dg_api_on_button((dg_btn_id_t)btn);
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
    /* 12×12 tile 视野用 8 色循环棋盘填充，方便肉眼验证布局 */
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
            s_placeholder_fb[y * DG_VIEWPORT_SIZE + x] = palette[tile_idx % 8];
        }
    }
    /* 中央画个"玩家"红点 */
    int cx = DG_VIEWPORT_SIZE / 2, cy = DG_VIEWPORT_SIZE / 2;
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (dx*dx + dy*dy <= 9) {
                s_placeholder_fb[(cy + dy) * DG_VIEWPORT_SIZE + (cx + dx)] = 0xF800;
            }
        }
    }
    /* 占位图同样转成 LVGL 显示字节序（引擎 fb 到来前也颜色正确） */
    copy_fb_to_display_order((const uint32_t *)s_placeholder_fb,
                             (uint32_t *)s_placeholder_fb,
                             sizeof(s_placeholder_fb) / 4);
}
