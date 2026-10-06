/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dungeon.cpp —— Game 状态机核心（骨架）
 */
#include "dg_types.h"
#include "rng/java_random.h"
#include "esp_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "dg.game";

namespace dg {

/* 静态内存 —— 骨架版先用 new，后续改静态池 */
Game::Game() {
    rng     = new JavaRandom(0);
    ui_rng  = new JavaRandom(0);
    hero    = new Hero();
    level   = new Level();
    memset(log_lines, 0, sizeof(log_lines));
}

Game::~Game() {
    delete rng; delete ui_rng; delete hero; delete level;
}

Game& Game::instance() {
    static Game g;
    return g;
}

void Game::init() {
    ESP_LOGI(TAG, "Game::init");
    assets::initialize();
    save::initialize();
    scene = DG_SCENE_TITLE;
}

void Game::new_game(int hero_class, uint32_t seed) {
    ESP_LOGI(TAG, "Game::new_game class=%d seed=0x%08x", hero_class, seed);
    this->seed = seed;
    rng->setSeed(seed);
    game_time = 0;
    depth = 0;

    hero->cls = (dg_class_t)hero_class;
    hero->hp_max = 20; hero->hp = 20;
    hero->str = 10; hero->lvl = 1; hero->exp = 0; hero->gold = 0;
    hero->level = level;

    level->generate(seed);
    level->depth = 0;
    level->hero = hero;
    level->create_mobs_and_items();

    hero->set_pos(level->entrance_pos % DG_MAP_W,
                  level->entrance_pos / DG_MAP_W);

    scene = DG_SCENE_IN_GAME;
    fb_dirty = true;
    log("welcome");
}

void Game::tick() {
    /* 回合调度：按 ready_at 挑最早的 Actor 行动 */
    if (scene != DG_SCENE_IN_GAME) return;

    /* 骨架版：仅递增时间戳 + 让 Mob 依次 act */
    game_time++;
    for (int i = 0; i < level->actor_count; i++) {
        Actor* a = level->actors[i];
        if (!a || !a->is_alive()) continue;
        if ((int32_t)(game_time - a->ready_at) < 0) continue;
        int spend = a->act();
        if (spend > 0) a->ready_at = game_time + spend;
    }
    fb_dirty = true;
}

void Game::on_tap(int gx, int gy) {
    if (scene != DG_SCENE_IN_GAME) return;
    if (gx < 0 || gx >= DG_MAP_W || gy < 0 || gy >= DG_MAP_H) return;

    int dx = gx - hero->x, dy = gy - hero->y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;

    if (adx <= 1 && ady <= 1 && (adx || ady)) {
        /* 相邻 8 格 → 走一步 / 攻击 */
        Tile& t = level->at(gx, gy);
        if (t.actor && t.actor->alignment == 1 /* ENEMY */) {
            hero->attack(t.actor);
        } else if (level->passable(gx, gy)) {
            hero->set_pos(gx, gy);
        }
        fb_dirty = true;
    } else {
        /* 远处 → A* 走一步（骨架：一步后停止；实际应存 path queue） */
        int steps[64];
        int n = pathfinder::find_path(level, hero->x, hero->y, gx, gy, steps, 64);
        if (n > 0) {
            int nx = steps[0] % DG_MAP_W, ny = steps[0] / DG_MAP_W;
            hero->set_pos(nx, ny);
            fb_dirty = true;
        }
    }
}

void Game::on_long_press(int gx, int gy) {
    ESP_LOGD(TAG, "inspect (%d,%d)", gx, gy);
    /* TODO: 弹检视卡 */
}

void Game::on_button(dg_btn_id_t btn) {
    ESP_LOGI(TAG, "button %d", (int)btn);
    switch (btn) {
    case DG_BTN_WAIT:
        /* 原地等 1 回合 */
        game_time++;
        break;
    case DG_BTN_INVENTORY:
        scene = (scene == DG_SCENE_INVENTORY) ? DG_SCENE_IN_GAME : DG_SCENE_INVENTORY;
        break;
    default:
        break;
    }
}

int Game::get_status_text(char *buf, int cap) {
    if (scene != DG_SCENE_IN_GAME) return 0;
    return snprintf(buf, cap, "HP %d/%d  Depth:%dF  Lv %d  Gold:%d",
                    hero->hp, hero->hp_max, depth + 1, hero->lvl, hero->gold);
}

const uint16_t* Game::get_tilemap_fb(int *w, int *h) {
    if (w) *w = DG_VIEWPORT_W;
    if (h) *h = DG_VIEWPORT_H;
    /* TODO: 从 level->tiles + sprite atlas 渲染到内部 fb；本骨架返 NULL */
    return nullptr;
}

int Game::get_message(char *buf, int cap, int index) {
    /* 从最新一条往前数 index 行 */
    int from = (log_head - 1 - index + kLogLines * 2) % kLogLines;
    const char* key = log_lines[from];
    if (!key) return 0;
    return snprintf(buf, cap, "%s", key);
}

void Game::log(const char* key) {
    log_lines[log_head] = key;
    log_head = (log_head + 1) % kLogLines;
}

}  /* namespace dg */
