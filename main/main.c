/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * main.c —— 应用层装配点（boot-direct 单应用）
 *
 * 平台能力（屏 / 触摸 / 电源键 / 音频 / 应用框架 / 字体）来自 components/bsp。
 * 游戏逻辑与素材来自 components/dungeon_engine，通过 dungeon_api.h 的 extern "C"
 * 边界访问，main.c 与 UI 层都不感知 C++ 内部结构。
 *
 * 启动顺序（不可乱，理由同 sdgoods-doom）：
 *   1. 电池自锁 GPIO
 *   2. LCD 面板 init + vendor MADCTL（背光仍关）
 *   3. 电源键 init
 *   4. i18n init（决定首屏用中文还是英文）
 *   5. LVGL init + 触摸 init + 硬件信息 init
 *   6. 音频 init（内存原因排在 LVGL 之后）
 *   7. 注册 poll + power_short 钩子到平台层
 *   8. ui_dungeon_start() 建首屏 + lv_refr_now 刷一帧
 *   9. 点亮背光（避免白屏闪烁）
 *  10. sdgoods_app_shell_init（共享音量等）
 *  11. sdgoods_lvgl_loop（永不返回）
 */

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp.h"                /* 硬件抽象层：板级支持包总入口 */
#include "dungeon_api.h"        /* 引擎 C ABI 边界（dg_api_init） */
#include "ui_dungeon.h"          /* 应用层：boot-direct 首屏 */
#include "build_version.h"      /* 自动生成：版本号 + 品牌信息 */
#include "lvgl.h"                /* lv_refr_now 首帧刷新前不点亮背光 */

static const char *TAG = "SDGOODS-DG";

/* 电源键短按「一级返回」钩子：单应用全屏地牢无子页可返回 ——
   始终返回 false，交平台默认导航（控制中心浮层若开着由其自身消费；
   否则熄屏 + 浅睡，回合状态保留在 PSRAM 中）。 */
static bool dungeon_power_short_handler(void)
{
    return false;
}

void app_main(void)
{
    /* 品牌与版本横幅 —— 独立游戏固件自报家门，串口日志一眼看出源头。 */
    ESP_LOGI(TAG, "========================================================");
    ESP_LOGI(TAG, " %s · %s", SDG_APP_NAME, SDG_APP_NAME_EN);
    ESP_LOGI(TAG, " 运行于 %s（%s）", SDGOODS_PRODUCT, SDGOODS_BRAND);
    ESP_LOGI(TAG, " 上游素材：%s", SDG_APP_UPSTREAM);
    ESP_LOGI(TAG, " 工程：%s", SDG_APP_REPO);
    ESP_LOGI(TAG, " 固件版本 %s", BUILD_VERSION_STR);
    ESP_LOGI(TAG, "========================================================");

    /* 静音噪音大的子系统日志（只留 warn 以上）。 */
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("i2c",  ESP_LOG_ERROR);

    /* 电池供电自锁：拉高保持上电。 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BAT_CONTROL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_BAT_CONTROL_GPIO, BOARD_BAT_CONTROL_LATCH_LEVEL);

    /* 屏控制器上电（背光仍关）。 */
    sdgoods_lcd_init_panel();
    sdgoods_lcd_apply_vendor_madctl();

    sdgoods_key_init();
    sdg_i18n_init();     /* 语言必须在任何界面创建前初始化 */

    sdgoods_lvgl_init();
    ESP_ERROR_CHECK(sdgoods_touch_init());
    ESP_ERROR_CHECK(sdgoods_hw_info_init());

    /* 音频排在 LVGL / 触摸之后 —— 内部 DMA RAM 分配顺序，勿提前。 */
    ESP_ERROR_CHECK(sdgoods_audio_init());

    /* 引擎 C ABI 初始化（读 assets 分区、初始化 RNG、建立 Game 单例）。 */
    dg_api_init();

    /* ★ 接线：单应用直启 —— 地牢逐帧 poll + 电源键短按钩子注册给平台层。 */
    sdgoods_apps_set_poll(ui_dungeon_poll);
    sdgoods_set_power_short_handler(dungeon_power_short_handler);

    /* 建首屏 → 刷一帧 → 点亮背光 → 应用框架 → 永不返回主循环。 */
    ui_dungeon_start();
    lv_refr_now(NULL);
    sdgoods_lcd_set_backlight(60);

    sdgoods_app_shell_init();

    sdgoods_lvgl_loop();
}
