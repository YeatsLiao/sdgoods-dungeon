/*
 * 谷仓共创计划 · 谷仓 SDGOODS 开放平台基础工程
 * 平台层（板级支持包 BSP）
 * https://github.com/SDGOODS/SDGOODS-ESP32S3
 *
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * 「谷仓共创计划」与「谷仓 SDGOODS 开放平台」项目、谷仓次元屏（谷仓电子徽章）设备，
 *   以及本基础代码的著作权与相关权利，均归深圳希德创新网络有限公司所有。
 * SPDX-License-Identifier: Apache-2.0
 *
 * 本文件属于平台层，以 Apache-2.0 发布：可自由商用、可闭源分发，
 * 只需保留本声明并携带 NOTICE 文件。详见 LICENSING.md。
 */

#include "sdgoods_app_shell.h"

#include "lvgl.h"
#include "sdgoods_lcd.h"        /* LCD_WIDTH / LCD_HEIGHT */
#include "sdgoods_audio.h"  /* sdgoods_audio_set_volume / sdgoods_audio_get_volume */
#include "sdgoods_screenshot.h"     /* sdgoods_screenshot_init：串口 's' 触发截屏（菜单按钮已移除） */
#include "sdgoods_console.h"        /* sdgoods_console_init：BSP 串口控制台（'?' 能力查询，始终存在） */
#include "sdgoods_tap.h"            /* 设备级手势策略：随外壳自动安装，app 无需自己接 */
#include "sdgoods_nvs.h"           /* sdgoods_nvs_ensure：平台自己保证 NVS 可用 */
#include "sdgoods_hw_info.h"       /* sdgoods_hw_info_init：电池电压 ADC（控制中心电量页要用） */
#include "esp_log.h"

LV_FONT_DECLARE(si_yuan_black_icon_14);
LV_FONT_DECLARE(cn_font_14);
LV_FONT_DECLARE(si_yuan_black_icon_16);
LV_FONT_DECLARE(cn_font_16);

/* 顶部下滑手势区高度 / 底部上滑手势区高度 / 滑动判定阈值（像素） */
#define TOP_ZONE      50
#define BOTTOM_ZONE   50
#define SWIPE_DY      50

static lv_obj_t *s_app_scr   = NULL;   /* 当前应用屏 */
static int       s_vol_pct   = 0;      /* 音量显示用的镜像；真值以 sdgoods_audio 为准，
                                        * 由 sdgoods_app_shell_init() 在启动时从 NVS 对齐 */
static void    (*s_pause_cb)(void) = NULL; /* 控制中心浮层打开时暂停应用 */
static void    (*s_resume_cb)(void) = NULL;/* 控制中心浮层关闭时恢复应用 */
static lv_coord_t s_top_py;            /* 顶部手势按下时的 y，供 RELEASED 计算滑动 */
static lv_obj_t  *s_bound_scr = NULL;  /* 已经绑过顶部手势的屏（防重复叠加捕获层） */
static lv_timer_t *s_autobind_timer = NULL;  /* 自动给「后来才出现的屏」套用外壳手势 */

/* ----------------------------------------------------------------------------
 * 共享音量
 *
 * ⚠️ 这里的音量**必须与真实音量（sdgoods_audio）和控制中心（NVS）三方一致**，
 *    否则用户会看到「在 A 里调的音量，进 B 又变回去了」。三条约束：
 *      ① 增减以 `sdgoods_audio_get_volume()`（真实值）为基准，**不用** s_vol_pct 镜像
 *         —— 用户在控制中心拖过滑块之后，那个镜像就过期了；
 *      ② 改完立刻落盘 NVS（sdgoods_cc_flush），否则菜单里调的音量跨不了重启；
 *      ③ 启动时由 sdgoods_app_shell_init() 从 NVS 恢复（见那里的说明）。
 * ------------------------------------------------------------------------- */
static void vol_apply(int pct)
{
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    s_vol_pct = pct;
    sdgoods_audio_set_volume(s_vol_pct);
    /* 立即落盘：
     * 用户下一次很可能就是深睡唤醒（=冷启动），不写就丢了。
     * 弱符号：控制中心组件未链接时跳过。 */
    extern void sdgoods_cc_flush(void) __attribute__((weak));
    if (sdgoods_cc_flush) {
        sdgoods_cc_flush();
    }
}

void sdgoods_app_volume_up(void)
{
    vol_apply(sdgoods_audio_get_volume() + 10);
}

void sdgoods_app_volume_down(void)
{
    vol_apply(sdgoods_audio_get_volume() - 10);
}

int sdgoods_app_volume_get(void)
{
    /* 返回**真实**音量而不是 s_vol_pct 镜像：日志/菜单显示都该以真实值为准，
     * 否则控制中心改过之后这里会报一个过期数字。 */
    return sdgoods_audio_get_volume();
}

/* 自动给当前屏套用外壳手势（顶部下滑 → 控制中心）。
 * 目的：让「每个 app 都有控制中心」成为**默认行为**，而不是依赖 app 记得调
 * sdgoods_app_shell_bind()。app 若自己调过 bind，s_bound_scr 已置位，这里会跳过 ⇒ 不重复。
 * 覆盖「后来才创建 / 加载的其他屏」：只比一次指针，换屏时才真正绑一次。 */
static void shell_autobind_scan(lv_timer_t *t)
{
    (void)t;
    lv_obj_t *cur = lv_scr_act();
    if (!cur || cur == s_bound_scr) {
        return;
    }
    sdgoods_app_shell_bind(cur);
}

void sdgoods_app_shell_init(void)
{
    /* ★ 先保障 NVS 可用：平台层有多个模块要写 NVS（控制中心音量/亮度、语言…），
     *   而最小应用可能从不 nvs_flash_init ⇒ 写入静默失败。
     *   做成平台自己的事，app 完全不必知道 NVS 的存在。幂等，开销一次函数调用。 */
    sdgoods_nvs_ensure();

    /* ★★ 音量 / 亮度的**跨重启保存**（2026-09-19 修）★★
     *
     * 为什么必须在这里做：本设备深睡唤醒 = **冷启动**（app_main 重跑），
     * RAM 里的任何状态都不会被带到下次启动 —— 能跨固件传递状态的**只有 NVS**。
     * 所以音量/亮度必须**每次启动都从 NVS 恢复**，否则用户会看到
     * 「调好→休眠唤醒后又变回去了」。写侧在控制中心（改一下防抖 800ms 落盘，
     * 见 sdgoods_cc.c 的 save_timer；关闭 CC / 关机前另有 flush）。
     *
     * ⚠️ 旧实现的 bug（两个叠加，用户可见症状 = 「音量亮度开机后丢失」）：
     *   ① 这里直接 `sdgoods_audio_set_volume(s_vol_pct)`，而 `s_vol_pct` 硬编初值 0
     *      ⇒ **一启动就被压成静音**，日志 `init: default volume=0%`；
     *   ② 恢复函数 sdgoods_cc_settings_restore() 从未在本路径被调用
     *      ⇒ 亮度根本没有恢复点，一直是面板复位后的默认值。
     *
     * 弱符号引用（同 sdgoods_cc_open 的做法）：控制中心组件未链接时符号为 NULL，
     * 跳过即可，不引入 board → control_center 的硬依赖。 */
    extern void sdgoods_cc_settings_restore(void) __attribute__((weak));
    if (sdgoods_cc_settings_restore) {
        sdgoods_cc_settings_restore();      /* NVS 有记录则同时恢复音量与亮度 */
    }

    /* 把外壳的音量镜像与**真实**音量对齐（无论上面恢复成功与否）：
     * 恢复成功时取 NVS 里的值；无记录时取音频模块自己的默认值（sdgoods_audio.c 里是 70）。
     * 这样菜单里的「音量 +/-」是从当前值继续加减，不会像旧版那样从 0 往上跳。 */
    s_vol_pct = sdgoods_audio_get_volume();
    sdgoods_audio_set_volume(s_vol_pct);
    ESP_LOGI("app_shell", "init: volume=%d%% (NVS-restored; cross-app synced)", s_vol_pct);

    /* ★ 电池读数（电压 / 百分比）同样是平台自己的事：
     *   控制中心的「电量页」是**平台层共享代码**，顶部下滑即可打开，
     *   而 `sdgoods_hw_bat_v()` 在 ADC 未初始化时**直接返回 0.f** ⇒ 屏上
     *   `Voltage --V` / `Level --%`。
     *   旧版靠固件自己调 sdgoods_hw_info_init()，不同固件调不调不一致
     *   ⇒ 同一个电量页时好时坏 —— 典型症状「看不到电量和电压」。
     *   现在由平台在 app_shell_init 里统一调；sdgoods_hw_info_init() 已做成
     *   幂等 + 不致命（见 sdgoods_hw_info.c），重复调用安全，失败也只是电量页显示 `--` 而不会 abort。 */
    sdgoods_hw_info_init();

    sdgoods_console_init();      /* BSP 串口控制台（'?' 能力查询等，始终存在） */
#ifdef CONFIG_SDGOODS_SCREENSHOT
    sdgoods_screenshot_init();   /* 截屏能力：串口 's' 触发（须在 LVGL 线程内初始化） */
#endif
    /* ★ 设备级手势策略随应用外壳一起安装（PRESS_LOCK 所有权 + 点按位移守卫 +
     *   装饰物穿透），上层无需自己接这些细节。详见 sdgoods_tap.h。 */
    sdgoods_tap_install();

    /* ★ 控制中心也做成默认：立刻给当前屏套上「顶部下滑唤出」，并起看门狗覆盖
     *   之后才加载的屏。这样 app 即使漏调 sdgoods_app_shell_bind() 也有控制中心。 */
    if (!s_autobind_timer) {
        s_autobind_timer = lv_timer_create(shell_autobind_scan, 100, NULL);
    }
    sdgoods_app_shell_bind(lv_scr_act());
    ESP_LOGI("app_shell", "control center armed on screen %p", (void *)lv_scr_act());
}

bool sdgoods_app_shell_is_app_active(void)
{
    return (s_app_scr != NULL);
}

void sdgoods_app_shell_set_pause_cb(void (*cb)(void))
{
    s_pause_cb = cb;
}

void sdgoods_app_shell_set_resume_cb(void (*cb)(void))
{
    s_resume_cb = cb;
}

/* ----------------------------------------------------------------------------
 * 顶部下滑手势捕获层
 * ------------------------------------------------------------------------- */
static void on_top_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_top_py = p.y;
}

static void on_top_released(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dy = (int)p.y - (int)s_top_py;
    if (s_top_py <= TOP_ZONE && dy >= SWIPE_DY) {
        /* 顶部下滑 → 打开**设备级控制中心**（音量 / 亮度 / 数据 / 电量 / Power）。
         * 原来的简易菜单已被控制中心取代（功能是其超集）。
         * 控制中心实现在平台层 control_center，BSP 通过弱符号调用（见本文件末尾）。 */
        sdgoods_cc_open();
    }
}

void sdgoods_app_shell_bind(lv_obj_t *scr)
{
    if (!scr) {
        return;
    }
    /* 幂等：同一屏重复调用不再叠加捕获层（下面的自动套用看门狗会反复尝试绑定）。 */
    if (scr == s_bound_scr) {
        return;
    }
    s_bound_scr = scr;
    s_app_scr = scr;

    lv_obj_t *cat = lv_obj_create(scr);
    lv_obj_remove_style_all(cat);
    lv_obj_set_size(cat, LCD_WIDTH, TOP_ZONE);
    lv_obj_set_pos(cat, 0, 0);
    lv_obj_set_style_bg_opa(cat, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(cat);   /* 置于游戏全屏 tap 之上，捕获顶部下滑 */
    lv_obj_add_event_cb(cat, on_top_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(cat, on_top_released, LV_EVENT_RELEASED, NULL);

    /* ★ 设备级手势策略：app 只要接了外壳就自动具备，无需自己知道细节。
     *   ① 屏是 lv_obj_create(NULL) 建的 ⇒ **无父对象 ⇒ LVGL 不会给它 PRESS_LOCK**
     *      （lv_obj.c:438 只对「有父对象」的加），这正是「按下落在屏上、滑动掠过
     *      按钮/图标后被接管成点按」的根源，必须先补上。
     *   ② 再对整棵树落实一次：装饰物（canvas）穿透 + 交互对象锁定所有权。
     *   两者共同保证「按在哪就归谁、滑走不算点按」。详见 sdgoods_tap.h。 */
    sdgoods_tap_lock(scr);
    sdgoods_tap_normalize(scr);
}

/* ---- 设备级控制中心：BSP 侧的弱默认实现 ------------------------------------
 * 真正的控制中心在 components/control_center/src/sdgoods_cc.c。
 * 这里给一个**弱符号**空实现，让「不含 control_center 的精简工程」也能编过；
 * 只要工程带了该组件，链接器会选它的强符号。
 * 做法与本工程 `sdgoods_console_ext_cmd`（BSP 弱默认、由工程覆盖）一致。 */
__attribute__((weak)) void sdgoods_cc_open(void)
{
    ESP_LOGW("app_shell", "sdgoods_cc_open(): control center not linked in this firmware");
}

/* 控制中心打开 / 关闭时回调这里：把「系统浮层开着」映射成 app 的暂停 / 恢复，
 * 使游戏类 app 在控制中心盖住画面时停止推进（与旧菜单的 pause/resume 语义一致）。 */
void sdgoods_app_shell_notify_overlay(bool open)
{
    if (open) {
        if (s_pause_cb) {
            s_pause_cb();
        }
    } else {
        if (s_resume_cb) {
            s_resume_cb();
        }
    }
}
