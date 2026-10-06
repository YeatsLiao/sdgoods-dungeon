# 谷仓次元屏 · 次元地牢 sdgoods-dungeon
# 独立游戏固件 · 基于 Shattered Pixel Dungeon 素材
#
# Copyright (c) 2026 Yeats Liao
# SPDX-License-Identifier: GPL-3.0-only
#
# 工程名决定固件产物：编译后得到 build/SDGOODS_DUNGEON.bin。
# IDF project_name / 仓库 slug / 谷仓后台工程名 / appdata 目录 四处字符串保持一致。

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(SDGOODS_DUNGEON)

# C++ 全局约束（架构级，不可回退）：
#   -fno-exceptions -fno-rtti : LVGL + ESP-IDF 惯例，省 flash，禁止业务代码用 try/catch 与 dynamic_cast
#   -std=gnu++17              : 使用 std::optional / if constexpr / structured binding
# 具体 target 层再各自 add_compile_options，此处仅备注给未来读代码的人。
