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

#include "sdgoods_hooks.h"

#include <stddef.h>   /* NULL */

/*
 * 应用层回调的注册表。见 include/sdgoods_hooks.h。
 * 单应用（DUNGEON）后只剩两项：逐帧 poll 汇总 + 电源键短按消费钩子。
 * 未注册即空操作——平台层可独立跑起来（比如只做屏点亮测试），不会空指针崩溃。
 */

static sdgoods_cb_t s_apps_poll = NULL;
static sdgoods_power_short_cb_t s_power_short = NULL;

void sdgoods_apps_set_poll(sdgoods_cb_t fn)
{
    s_apps_poll = fn;
}

void sdgoods_set_power_short_handler(sdgoods_power_short_cb_t fn)
{
    s_power_short = fn;
}

bool sdgoods_ui_power_short(void)
{
    if (s_power_short) {
        return s_power_short();
    }
    return false;
}

/* ---- 平台层内部调用 ------------------------------------------------------- */

void sdgoods_apps_poll(void)
{
    if (s_apps_poll) {
        s_apps_poll();
    }
}
