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

/**
 * @file sdgoods_console.c
 * @brief BSP 串口控制台：常驻命令分发。
 *
 * 直接轮询 USB-Serial-JTAG 的 RX FIFO 实现命令触发（不装 usb_serial_jtag driver，
 * 因为该外设被次级 console 占用，driver_install 会被拒；但 RX/TX FIFO 由我们自己
 * 读写是安全的）。本任务只做命令分发，真正的截屏抓帧在 LVGL 线程完成。
 */

#include "sdgoods_console.h"
#include "sdgoods_caps.h"
#include "sdgoods_screenshot.h"   /* sdgoods_screenshot_capture（仅在能力启用时调用） */
#include "sdgoods_tap.h"          /* sdgoods_tap_synth：弱默认 ext_cmd 的平台内置手势自检 */
#include "sdgoods_lcd.h"          /* LCD_WIDTH / LCD_HEIGHT：自检手势的默认起手点 */
#include "sdgoods_power.h"        /* sdgoods_power_suspend_toggle / sdgoods_power_off_preview：电源键调试 */
#include "sdgoods_input.h"        /* sdgoods_power_key_short_action：调试键 'P' 走真实短按路径 */

#include "esp_log.h"
#include "esp_system.h"          /* esp_restart：串口重启命令 'R' */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 控制中心二级页直开（'C'/'D'/'B'）：CC 是**下游 control_center 组件**，BSP 用弱符号引用，
 * 未链接时地址为 NULL（下面判空后再调）。「离线核验非首屏」这条能力暴露到串口上。 */
extern void sdgoods_cc_debug_open(int which) __attribute__((weak));

/* 关闭控制台 VFS 对 TX 的 CRLF 转换：截屏二进制里的 0x0A 不能被膨胀成 0x0D 0x0A，
 * 否则破坏帧边界、挤掉 JPEG EOI。改为 LF 即「不修改」，二进制才能逐字节对齐。 */
#include "esp_vfs_common.h"                       /* esp_line_endings_t / ESP_LINE_ENDINGS_LF */
void usb_serial_jtag_vfs_set_tx_line_endings(esp_line_endings_t mode);
#include "hal/usb_serial_jtag_ll.h"

static const char *TAG = "bsp-console";

/* 诊断扩展点：应用层可覆盖此弱符号，注册额外串口命令。
 * 不破坏平台/应用分层（BSP 不 include 应用层）。仅用于调试。
 *
 * ★ 弱默认实现 = **平台内置自检**，两类能力：
 *   ① 手势自检：'1'..'5' 接到 sdgoods_tap_synth()，于是任何固件（哪怕一行调试代码都没写）
 *      都能在串口上验证手势判别是否正确。为什么放进平台层：手势行为没法用肉眼远程观察，
 *      而「点按 vs 滑动」是最容易写错、也最难回归的一类逻辑；让固件自己跑一遍 + 看日志断言，
 *      是唯一可靠手段。
 *   ② 非首屏直开：'C'/'D'/'B' 直接打开控制中心一/二/三级页 —— 截屏只能拿当前那一屏，
 *      而 CC 二级页要先开 CC 再点中按钮，app 侧没有能做到的合成手势。 */
__attribute__((weak)) void sdgoods_console_ext_cmd(char c)
{
    switch (c) {
    case '0':   /* 点按下排按钮位 (180,218)：留作合成点按坐标，当前单应用页该位置无按钮。 */
        sdgoods_tap_synth(LCD_WIDTH / 2, 218, 0, 0, 1);
        break;
    case '1':   /* 点按屏中央：验证按钮命中 / 点按判定 */
        sdgoods_tap_synth(LCD_WIDTH / 2, LCD_HEIGHT / 2, 0, 0, 1);
        break;
    case '2':   /* 顶部下划 75px：打开控制中心（app 外壳手势） */
        sdgoods_tap_synth(LCD_WIDTH / 2, 40, 0, 25, 3);
        break;
    case '3':   /* 底部上划 75px：关闭系统浮层（回到 DUNGEON） */
        sdgoods_tap_synth(LCD_WIDTH / 2, LCD_HEIGHT - 40, 0, -25, 3);
        break;
    case '4':   /* 左缘右滑 75px：返回上级 */
        sdgoods_tap_synth(20, LCD_HEIGHT / 2, 25, 0, 3);
        break;
    case '5':   /* 起手后滑走（上划 75px）：验证「滑动掠过不算点按」 */
        sdgoods_tap_synth(70, 157, 0, -25, 3);
        break;
    /* 'C' / 'D' / 'B'：直接打开控制中心页 / 电量页（离线核验非首屏）。
     * 为什么需要它：截屏只能拿到**当前那一屏**，而 CC 的二级页要先「下划开 CC」再
     * 点中对应按钮 —— app 侧没有能点到对应按钮的合成手势。
     * ⚠️ 实现内部会**先 close 再 open**，否则 cc_open 见已有浮层会直接返回，
     *   截到的还是上一页。 */
    case 'C':
    case 'D':
    case 'B': {
        if (!sdgoods_cc_debug_open) {
            ESP_LOGW(TAG, "'%c': control_center component not linked, no-op", c);
            break;
        }
        const int which = (c == 'C') ? 0 : (c == 'D') ? 1 : 2;
        ESP_LOGI(TAG, "'%c': opening control center page %d (0/1=main 2=battery)", c, which);
        sdgoods_cc_debug_open(which);
        break;
    }
    case 'p':   /* 调试：模拟「主页短按电源键」的生产路径 = 熄屏 + 进入深度睡眠
                 *（最低功耗；再按电源键唤醒 = 冷启动直回 DUNGEON，不播开机动画）。
                 * 函数只碰背光 + gpio hold + esp_deep_sleep_start，不碰 LVGL 对象树，
                 * 可在 console 任务直接调。深睡后串口断开，需按电源键唤醒后串口才重新枚举。 */
        ESP_LOGI(TAG, "'p': enter deep sleep (simulate home short power press)");
        sdgoods_power_enter_deep_sleep();
        break;
    case 'P':   /* 调试：模拟电源键「短按」的完整生产路径（与真实松手沿共用
                 * sdgoods_power_key_short_action()：上层钩子 → 若 CC 开着则关浮层 → 否则深睡）。
                 * 串口验证电源键单应用语义用，不需要真手指按键。 */
        ESP_LOGI(TAG, "'P': power key short-press action (production path)");
        sdgoods_power_key_short_action();
        break;
    case 'Z':   /* 调试：画出「Power Off」关机画面（与真实关机同款视觉，但不切电），供串口截屏核验。
                 * 必须经 sdgoods_lvgl_post 在 LVGL 线程画，否则乱摸 LVGL 对象树。 */
        ESP_LOGI(TAG, "'Z': preview Power Off screen (no power cut)");
        sdgoods_power_off_preview();
        break;
    default:
        break;
    }
}

/* 串口 RX 读缓冲：console 任务栈仅 4KB，放不下 2KB 的缓冲，故做成文件作用域静态数组。
 * （原「'X' 二进制注入通道」随平台框架层剥离一并移除，这里仅保留普通命令的读取缓冲。） */
#define BIN_RX_BUF_SZ        2048    /* 单次从 FIFO 抽出的字节数 */
static uint8_t     s_bin_rx[BIN_RX_BUF_SZ];

/* 串口命令分发：'?' 查能力；'s'/'S' 触发截屏（若能力已登记）；'R' 重启；其余可打印字符转 ext_cmd。 */
static void console_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ready (send '?' for capabilities, 's' to capture if enabled, 'R' to reboot)");

    for (;;) {
        uint32_t n = usb_serial_jtag_ll_rxfifo_data_available()
                   ? usb_serial_jtag_ll_read_rxfifo(s_bin_rx, sizeof(s_bin_rx)) : 0;
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t b = s_bin_rx[i];
            if (b == '?') {
                /* 能力查询：静音日志避免与回传行交错，回传 SDGOODS-CAPS: 一行 */
                esp_log_level_set("*", ESP_LOG_NONE);
                printf("SDGOODS-CAPS:%s\n", sdgoods_caps_names());
                fflush(stdout);
                esp_log_level_set("*", ESP_LOG_INFO);
            } else if (b == 's' || b == 'S') {
#ifdef CONFIG_SDGOODS_SCREENSHOT
                if (sdgoods_caps_has(SDGOODS_CAP_SCREENSHOT)) {
                    sdgoods_screenshot_capture();
                } else {
                    ESP_LOGW(TAG, "screenshot capability not enabled");
                }
#else
                ESP_LOGW(TAG, "screenshot not built into this firmware");
#endif
            } else if (b == 'R') {
                /* 平台级：立即重启（等价按一下复位键）。
                 *
                 * 为什么必须是平台级、而不是塞进某个应用的 ext_cmd：复位是**与具体应用无关**
                 * 的能力，和 '?'/'s' 同一性质；放在这里则**任何固件行为完全一致**。
                 *
                 * 为什么值得为「调试便利」专门加一个命令：本机 USB-Serial-JTAG 的
                 * DTR/RTS 脉冲复位**并非每次都生效**，有了 'R'，脚本可以用「发 'R' + 等 boot
                 * banner」把复位变成确定性的。 */
                ESP_LOGW(TAG, "reboot requested from console ('R')");
                fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(20));   /* 让上面那行日志先出 FIFO */
                esp_restart();
            } else if (b >= 32 && b < 127) {
                ESP_LOGI(TAG, "serial rx '%c'", b);
                sdgoods_console_ext_cmd((char)b);   /* 应用层 / 平台内置诊断命令（手势自检等） */
            }
        }
    }
}

void sdgoods_console_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;

    /* 关闭 TX 的 CRLF 转换（详见文件头说明）。此设置全局，对普通日志无影响。 */
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    /* 栈 4KB：接收缓冲是文件作用域静态数组，这 4KB 全是留给命令处理调用链的。 */
    if (xTaskCreate(console_rx_task, "bsp_console", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "rx task create failed");
    }
}
