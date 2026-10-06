/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * ui_dungeon.h —— 首屏装配（boot-direct 单应用）
 */
#ifndef SDG_UI_DUNGEON_H
#define SDG_UI_DUNGEON_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 建首屏并挂载到 LVGL 活动屏幕。app_main 在点亮背光前调用一次。 */
void ui_dungeon_start(void);

/* 每帧由 sdgoods_lvgl_loop 通过 sdgoods_apps_set_poll 回调。
 * 职责：
 *   - 读 pending touch tap，翻译到 tile 坐标后送交引擎
 *   - 引擎若有地图变更，拷贝 tilemap framebuffer 到 LVGL img 对象
 *   - 状态栏文字刷新（低频，500ms 一次即可） */
void ui_dungeon_poll(void);

/* 供 debug 与串口命令使用。 */
bool ui_dungeon_is_running(void);

#ifdef __cplusplus
}
#endif
#endif
