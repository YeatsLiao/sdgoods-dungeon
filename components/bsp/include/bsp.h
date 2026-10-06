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

/*
 * bsp.h —— 谷仓次元屏 / 谷仓电子徽章 板级支持包（BSP）总入口
 *
 * （本组件由官方平台基座 components/sdgoods_board 重命名而来；内部各子系统
 *   头文件名与 sdgoods_ 函数符号保持不变，仅目录名与本伞形头改名。）
 *
 * 设备：ESP32-S3-R8 + 360x360 圆形 ST77916 QSPI 屏 + CST816 电容触摸
 *
 * 二次开发建议
 * ------------
 * · 应用代码请放在 main/game/ 下；本工程为单应用 DUNGEON 直启，无多应用注册表。
 * · 这个目录（components/bsp）是硬件抽象层：引脚、屏驱动、LVGL 移植、
 *   触摸、音频、应用框架。**一般不需要改**，除非你要换硬件或改平台行为。
 * · 唯一鼓励改的文件是 include/board_pins.h（引脚定义）。
 * · 平台大缓冲要显式申请 PSRAM（heap_caps_malloc + MALLOC_CAP_SPIRAM）；
 *   但 LVGL 的绘制缓冲**不能**放 PSRAM（QSPI DMA 弹跳会出黑条），
 *   详见 src/sdgoods_lvgl.c 的注释。
 */

/* 给应用层的接口（UI 栅格常量 + 轮询/电源键钩子） */
#include "sdgoods_ui.h"
#include "sdgoods_hooks.h"

/* 界面语言（默认英文；文案用 SDG_T("中文", "English") 取词） */
#include "sdgoods_i18n.h"

/* 硬件与显示 */
#include "board_pins.h"
#include "sdgoods_lcd.h"
#include "sdgoods_lvgl.h"
#include "sdgoods_input.h"
#include "sdgoods_power.h"

/* 板载外设与服务（WiFi/BLE 已随独立游戏需求移除） */
#include "sdgoods_audio.h"
#include "sdgoods_hw_info.h"

/* 调试能力（改完 UI 用它截屏自证） */
#include "sdgoods_screenshot.h"

/* BSP 基础能力登记表 + 串口控制台：网页端通过串口 '?' 查询固件能力，
 * 据此判断（如截屏 SHOT）是否编入当前固件，未编入时给出「从 BSP 添加」的提示。 */
#include "sdgoods_caps.h"
#include "sdgoods_console.h"

/* 应用外壳：统一「顶部下滑控制中心 / 暂停 / 恢复」体验 */
#include "sdgoods_app_shell.h"

/* 设备级触摸手势（全设备统一口径）：
 *   sdgoods_tap.h       —— 点按位移守卫 + PRESS_LOCK 所有权 + 装饰物穿透
 *   sdgoods_swipe_back.h —— 左缘右滑返回上一级
 *   sdgoods_swipe_up.h   —— 底部上滑（回调由调用方绑定，控制中心用它关浮层）
 * 一般无需手动调用：sdgoods_app_shell_init() 已自动安装手势策略。 */
#include "sdgoods_tap.h"
#include "sdgoods_swipe_back.h"
#include "sdgoods_swipe_up.h"
