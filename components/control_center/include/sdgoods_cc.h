/*
 * 谷仓共创计划 · 谷仓 SDGOODS 开放平台基础工程
 * 平台层（板级支持包 BSP）· 控制中心
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

/*
 * sdgoods_cc.h —— 控制中心（Control Center）：**独立单应用游戏（DUNGEON）定制版系统浮层**
 *
 * 从当前屏「顶部下滑」唤出（`sdgoods_app_shell_bind()` 已自动接好手势，见 sdgoods_tap.h）：
 *   - 一级页 3 键：Settings / About / Power。
 *   - Settings 二级页 3 键：Volume / Brightness（各进滑块页，可滑动实时调节）+ Battery（二级信息页
 *     `Voltage x.xxV` + `Level nn%`）。
 *   - About 报固件名 / 版本 / 编译时间 / 镜像大小；Power 直接断电关机。
 *   （多应用启动器相关的 Data 槽位页、Exit 回启动器、SINGLE/MULTI 模式小字已随启动器机制一并移除。）
 *
 * 二级页交互：从底部横条处上滑直接关闭控制中心；从最左边起手左→右滑返回上一级；
 * 电源键短按在任意页先关闭浮层（见 sdgoods_input.c）。
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* 在当前屏上绑定「从顶部下滑」手势，用于打开控制中心。
 * 实际由 sdgoods_app_shell_bind() 内部接好，应用层无需自己调。 */
void sdgoods_cc_bind(lv_obj_t *scr);

/* 打开控制中心浮层（一级页）。任意任务可调用：内部走 lv_async_call 转到 LVGL 线程执行。 */
void sdgoods_cc_open(void);

/* 关闭控制中心（含二级页），回到当前屏。 */
void sdgoods_cc_close(void);

/* 调试用（串口控制台钩子，供真机离线截图核验，不参与正常交互流程）：
 *   which = 0 → 控制中心一级页；2 → 电量二级页；
 *           3 → 音量滑块页；4 → 亮度滑块页。
 * 任意任务可调用：内部走 lv_async_call 转到 LVGL 线程执行。 */
void sdgoods_cc_debug_open(int which);

/* 控制中心（含二级滑块页）当前是否处于打开状态 */
bool sdgoods_cc_is_open(void);

/* 开机恢复音量 / 亮度：从 NVS 读出掉电前保存的值并应用到硬件（无记录则保持出厂默认）。
 * 由使用方在进入首屏时调用一次。 */
void sdgoods_cc_settings_restore(void);

/* 读「用户设定的亮度」（0~100，无 NVS 记录时为出厂默认 80）。
 *
 * 给**启动流程**用：开机会「先建首屏（内部已 restore 用户亮度）
 * → 再亮屏」，启动流程若硬编 set_backlight(80) 会把刚恢复的用户值覆盖掉。
 * 用本函数取用户意图值去点亮，冷启动与深睡唤醒两条路径就都正确了。
 *
 * ⚠️ 不是 sdgoods_lcd_get_backlight()：那个是**瞬时值**，开机动画期 / 息屏期为 0，
 *   拿它去点屏会把「临时熄灭」当成用户偏好。 */
uint8_t sdgoods_cc_brightness_get(void);

/* 强制立即把当前音量 / 亮度落盘到 NVS（取消挂起的防抖定时器）。
 * 在「关机」等会立即断电前调用，确保最近一次调节不丢失。 */
void sdgoods_cc_flush(void);

/* 读电池电压并换算成电量百分比（统一换算口径，避免多处各写一份阈值后漂移）。
 * 返回 0~100；读数失败（平台层返回 0.f）返回 -1。
 * v_out 可传 NULL，否则回填本次采样的电压（伏特；失败时为 0）。 */
int sdgoods_cc_bat_read(float *v_out);

/* 电源键短按钩子（注册到 sdgoods_set_power_short_handler）：
 * 若控制中心 / 滑块页打开则关闭并返回 true（事件已消费），否则返回 false。
 * 使「短按电源先关浮层」优先于「主页熄屏」。 */
bool sdgoods_cc_power_short(void);
