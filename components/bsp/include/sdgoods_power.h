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

#pragma once

#include <stdbool.h>

/**
 * Cut device power and turn the badge off.
 *
 * Releases the self-holding battery latch (BOARD_BAT_CONTROL_GPIO) so the
 * power MOSFET opens and the whole board loses power. If power is not actually
 * cut (e.g. the physical power button is still held), it falls back to deep
 * sleep and wakes on the physical key press.
 *
 * Only call this when the user explicitly requests shutdown.
 */
void sdgoods_power_off(void);

/**
 * DEBUG ONLY: draw the exact "Power Off" overlay on the LVGL thread (same visual
 * as sdgoods_power_off) but do NOT cut board power. Lets a serial console command
 * trigger the shutdown screen so it can be captured by the screenshot tool.
 * Implemented via sdgoods_lvgl_post so it runs on the LVGL thread (safe).
 */
void sdgoods_power_off_preview(void);

/**
 * Quick "screen off / low-power" toggle.
 *
 * First call saves the current backlight level, turns the backlight off and
 * asks the LVGL loop to pause rendering (see sdgoods_lvgl_loop). The next call
 * restores the saved brightness. Short-press power -> sleep, short-press
 * again -> wake.
 *
 * This is a light, instantly-resumable low-power state (CPU mostly idle, no
 * render work); it does NOT cut board power. Use sdgoods_power_off() for that.
 */
void sdgoods_power_suspend_toggle(void);

/**
 * Enter ultra-low-power light sleep (power-key short press).
 *
 * Turns the backlight off and puts the SoC into light sleep. Wake-up is wired to
 * the power key via ext0, so a single press wakes the device and lights the
 * screen again — WITHOUT a reboot: LVGL / peripherals / app state are all
 * preserved and execution resumes right after this call returns. (sdgoods_power_off()
 * is the real deep-sleep path where wake == cold boot.) Use this for "screen off,
 * tap power to resume".
 */
void sdgoods_power_enter_light_sleep(void);

/**
 * Enter deep sleep (power-key short press).
 *
 * The lowest-power state: the SoC is fully powered down and wakes ONLY on the
 * physical power key (ext0). Wake == a cold boot (app_main runs again), so it is
 * slower to come back than light sleep but consumes far less.
 *
 * The battery self-latching MOSFET (BOARD_BAT_CONTROL_GPIO) is held across the
 * deep sleep via gpio_hold_en + gpio_deep_sleep_hold_en so the board stays
 * powered and can actually be woken. On wake, the device cold-boots straight
 * back into DUNGEON (this project boots directly to the game and plays no boot
 * animation), so a deep-sleep wake simply resumes the game.
 *
 * Use this for "screen off, lowest power"; use sdgoods_power_enter_light_sleep()
 * when a fast, in-place wake (no reboot) is preferred.
 */
void sdgoods_power_enter_deep_sleep(void);

/** True while the screen is in the suspended (low-power) state. */
bool sdgoods_power_is_suspended(void);
