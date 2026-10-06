/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * api_bridge.cpp —— C ABI 边界实现（把 dungeon_api.h 的 extern "C"
 *                    函数转成 Game 单例的方法调用）
 *
 * 这一层是唯一允许 UI 层看到的地方，也是引擎对外承诺的稳定接口。
 * 任何 C++ 类、STL 容器、命名空间都不允许泄漏到本文件之外。
 */
#include "dungeon_api.h"
#include "dg_types.h"
#include "rng/java_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dg.api";

using namespace dg;

extern "C" {

void dg_api_init(void) {
    Game::instance().init();
    int rc = dg_test_java_random_selftest();
    if (rc == 0) {
        ESP_LOGI(TAG, "selftest: JavaRandom OK");
    } else {
        ESP_LOGE(TAG, "selftest: JavaRandom FAIL rc=%d", rc);
    }
}

void dg_api_new_game(int hero_class, uint32_t seed) {
    Game::instance().new_game(hero_class, seed);
}

void dg_api_tick_if_needed(void) {
    Game::instance().tick();
}

void dg_api_on_tap(int gx, int gy) {
    Game::instance().on_tap(gx, gy);
}

void dg_api_on_viewport_tap(int px, int py) {
    Game& g = Game::instance();
    if (g.scene != DG_SCENE_IN_GAME) return;
    /* 视口像素 → 世界 tile：加当前相机左上角（渲染时维护，clamp 到图内） */
    int gx = g.cam_x + px / DG_TILE_PX;
    int gy = g.cam_y + py / DG_TILE_PX;
    g.on_tap(gx, gy);
}

void dg_api_on_long_press(int gx, int gy) {
    Game::instance().on_long_press(gx, gy);
}

void dg_api_on_button(dg_btn_id_t btn) {
    Game::instance().on_button(btn);
}

int dg_api_get_status_text(char *buf, int cap) {
    return Game::instance().get_status_text(buf, cap);
}

const uint16_t* dg_api_get_tilemap_fb(int *out_w, int *out_h) {
    return Game::instance().get_tilemap_fb(out_w, out_h);
}

int dg_api_get_message(char *buf, int cap, int index) {
    return Game::instance().get_message(buf, cap, index);
}

bool dg_api_has_save(int slot) {
    return save::has_save(slot);
}

bool dg_api_load_game(int slot) {
    return save::load(slot);
}

bool dg_api_save_game(int slot) {
    return save::store(slot);
}

dg_scene_t dg_api_current_scene(void) {
    return Game::instance().scene;
}

const char* dg_api_version(void) {
    return "0.2.0-playable";
}

int dg_api_run_selftest(void) {
    return dg_test_java_random_selftest();
}

}  /* extern "C" */
