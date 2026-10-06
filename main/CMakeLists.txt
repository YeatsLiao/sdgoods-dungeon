# main/ —— 应用层（boot-direct 装配 + LVGL 首屏）
#
# 平台层（屏 / 触摸 / 音频 / LVGL 移植 / 电源键 / 控制中心）来自两个共享组件：
#   components/bsp/            —— 板级支持包（从 sdgoods-doom 抽入，Apache-2.0）
#   components/control_center/ —— 下拉控制中心（从 sdgoods-doom 抽入，Apache-2.0）
# 游戏逻辑与素材访问：
#   components/dungeon_engine/ —— 自研 C++ 引擎（GPL-3.0），对外仅通过
#                                  dungeon_api.h 的 extern "C" 边界交互
#
# UI 层与引擎解耦：main/game/*.c 只 #include "dungeon_api.h" 用 C 函数调用引擎，
# 不感知 C++ 类、命名空间、模板等内部结构。

idf_component_register(
    SRCS
        "main.c"
        "game/ui_dungeon.c"
        "game/dg_audio.c"

    INCLUDE_DIRS
        "game"

    REQUIRES
        dungeon_engine
        bsp
        control_center
        driver
        esp_event
        esp_timer
        freertos
        hal
        heap
        log
        lvgl
        nvs_flash
)

# ---- 自动编译版本号 (MMDDHHMM) ----
set(_bv_dir ${CMAKE_CURRENT_BINARY_DIR})
add_custom_target(gen_build_version ALL
    COMMAND ${CMAKE_COMMAND}
        -D BIN_DIR=${_bv_dir}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/gen_build_version.cmake
    BYPRODUCTS ${_bv_dir}/build_version.h
    COMMENT "Generating build version stamp (MMDDHHMM)"
)
add_dependencies(${COMPONENT_LIB} gen_build_version)
target_include_directories(${COMPONENT_LIB} PRIVATE ${_bv_dir})
