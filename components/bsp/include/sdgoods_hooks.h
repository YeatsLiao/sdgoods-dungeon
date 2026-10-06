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

/*
 * sdgoods_hooks.h —— 应用层「注册」给平台层的回调（单应用游戏定制后仅两项）
 *
 * 平台层（components/bsp）负责屏 / 触摸 / 音频 / 应用框架，它不知道游戏长什么样。
 * 独立单应用（DUNGEON）后只保留两处必须回调应用层的点：
 *   1. LVGL 主循环每轮推进游戏的逐帧 poll；
 *   2. 电源键短按的「上层先消费」钩子（浮层开着则先关）。
 *
 * 原多应用时代的屏幕导航（home/apps_show）、启动器槽位事件、单/多应用模式判定
 * 已随平台框架剥离一并移除。未注册时退化为空操作，不会空指针崩溃。
 * 装配点在 main/main.c（boot-direct）。
 */

typedef void (*sdgoods_cb_t)(void);

/* ---- 1) 应用轮询 ----------------------------------------------------------
 * sdgoods_lvgl_loop() 每轮（约 2ms）调用一次。游戏的逐帧 *_poll() 由应用层注册。 */
void sdgoods_apps_set_poll(sdgoods_cb_t fn);

/* ---- 2) 电源键短按「消费」钩子 -------------------------------------------
 * 应用层可注册一个回调：返回 true 表示上层已处理该次短按（例如关闭控制中心的浮层），
 * 平台层不再执行默认导航（熄屏低功耗）。返回 false 则走默认逻辑。
 * 未注册则为 NULL，平台层照常执行默认导航。 */
typedef bool (*sdgoods_power_short_cb_t)(void);
void sdgoods_set_power_short_handler(sdgoods_power_short_cb_t fn);
/* 平台层内部：poll 调用，转调已注册的钩子；无钩子时返回 false。 */
bool sdgoods_ui_power_short(void);

/* ---- 以下为平台层内部使用（应用层不需要调用） ----------------------------- */
void sdgoods_apps_poll(void);            /* 由 sdgoods_lvgl_loop() 调用 */
