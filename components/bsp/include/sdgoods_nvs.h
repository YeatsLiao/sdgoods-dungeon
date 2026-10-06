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

/* NVS 保障（平台层公共工具）
 *
 * 为什么需要它：**平台层不能假设上层做过 `nvs_flash_init()`**。最小入口可能
 * 既不碰 WiFi 也不碰 BLE，从不初始化 NVS；而平台层有若干模块会写 NVS
 * （控制中心的音量/亮度、i18n 语言……），一旦 NVS 没初始化，
 * 这些写入会静默失败 —— 症状是「设置不保存」，非常难查。平台层自己保证这件事，
 * 上层就不必知道 NVS 的存在。
 *
 * 调用时机（都由平台层自己调，上层无需关心）：
 *   · `sdgoods_app_shell_init()` —— 应用外壳入口，开机调一次即可覆盖整机生命周期
 *   · 控制中心读写设置前 —— 保证即使不调 app_shell_init 也安全
 */

#pragma once

/* 确保 NVS 已初始化（**幂等**，已初始化时直接返回 ESP_OK，开销约一次函数调用）。
 * 仅当 NVS 分区损坏 / 版本不匹配（`NO_FREE_PAGES` / `NEW_VERSION_FOUND`）时才擦除重建，
 * 擦除前会打 WARN —— 那会清空整个 nvs 分区（含槽清单），所以日志里必须看得见。 */
void sdgoods_nvs_ensure(void);
