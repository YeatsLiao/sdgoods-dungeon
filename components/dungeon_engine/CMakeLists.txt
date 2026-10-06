# components/dungeon_engine/
#
# 自研 C++ 地牢引擎（GPL-3.0）。本组件对外**只暴露** include/dungeon_api.h
# 一个纯 C 头文件，main/game/*.c 通过它调用引擎；所有 C++ 类与内部头
# 藏在 src/ 下（不出 include/）。这样保证：
#   * UI 层可继续用纯 C 编写，编译约束简单
#   * 引擎内部重构不影响 UI 层
#   * 未来若有第二个 UI 皮肤（如英文 / 触屏重排版）可以共用同一份 API
#
# 素材访问：assets_blob.cpp 通过 esp_partition_find("assets") + 编译期
# pack_assets.py 生成的 offset 表读取 raw 分区，不使用文件系统。
#
# 存档：save.cpp 通过独立 "save" NVS 分区读写 3 槽位存档，与系统 nvs 隔离。

idf_component_register(
    SRCS
        "src/api_bridge.cpp"
        "src/core/dungeon.cpp"
        "src/core/level.cpp"
        "src/actor/hero.cpp"
        "src/actor/mob.cpp"
        "src/item/item.cpp"
        "src/rng/java_random.cpp"
        "src/pathfinder/astar.cpp"
        "src/assets/assets_blob.cpp"
        "src/save/save.cpp"

    INCLUDE_DIRS
        "include"           # 只有 dungeon_api.h 会在这

    PRIV_INCLUDE_DIRS
        "src"               # 内部 C++ 头从这里找

    REQUIRES
        esp_partition
        esp_timer
        spi_flash
        nvs_flash
        freertos
        log
        heap
)

# 强制 -fno-exceptions -fno-rtti（与顶层 sdkconfig.defaults 一致）
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-exceptions
    -fno-rtti
    -std=gnu++17
    -Wall
    -Wextra
    -Wno-unused-parameter
)
