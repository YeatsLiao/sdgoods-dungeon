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
 * v0.4 新增：场景流 / 结构化 HUD / 背包 / 图标取图 / 音效队列 / 存档摘要。
 */
#include "dungeon_api.h"
#include "dg_types.h"
#include "gfx/gfx.h"
#include "rng/java_random.h"
#include "esp_log.h"

static const char *TAG = "dg.api";

using namespace dg;

/* dg_sheet_t → gfx::Sheet 显式映射（C 边界枚举与引擎内部枚举数值解耦） */
static gfx::Sheet map_sheet(int sheet)
{
    switch (sheet) {
    case DG_SHEET_ITEMS:        return gfx::SH_ITEMS;
    case DG_SHEET_ICONS:        return gfx::SH_ICONS;
    case DG_SHEET_HERO_WARRIOR: return gfx::SH_HERO_WARRIOR;
    case DG_SHEET_HERO_MAGE:    return gfx::SH_HERO_MAGE;
    case DG_SHEET_HERO_ROGUE:   return gfx::SH_HERO_ROGUE;
    case DG_SHEET_HERO_HUNTRESS:return gfx::SH_HERO_HUNTRESS;
    default:                    return gfx::SH_ITEMS;
    }
}

extern "C" {

void dg_api_init(void) {
    Game::instance().init();
    int rc = dg_test_java_random_selftest();
    ESP_LOGI(TAG, "selftest JavaRandom %s (rc=%d)", rc == 0 ? "OK" : "FAIL", rc);
}

void dg_api_new_game(int hero_class, uint32_t seed) { Game::instance().new_game(hero_class, seed); }
void dg_api_tick_if_needed(void) { Game::instance().tick(); }
void dg_api_on_tap(int gx, int gy) { Game::instance().on_tap(gx, gy); }

void dg_api_on_viewport_tap(int px, int py) {
    Game& g = Game::instance();
    if (g.scene != DG_SCENE_IN_GAME) return;
    int gx = g.cam_x + px / DG_TILE_PX;
    int gy = g.cam_y + py / DG_TILE_PX;
    g.on_tap(gx, gy);
}

void dg_api_on_long_press(int gx, int gy) { Game::instance().on_long_press(gx, gy); }
void dg_api_on_button(dg_btn_id_t btn) { Game::instance().on_button(btn); }
bool dg_api_step(int dx, int dy) { return Game::instance().step(dx, dy); }

/* ===== 场景流 ===== */
void dg_api_goto_title(void) { Game::instance().goto_title(); }
void dg_api_goto_class_select(void) { Game::instance().goto_class_select(); }
void dg_api_pick_class(int hero_class) { Game::instance().pick_class(hero_class); }
void dg_api_menu_action(int action, int slot) { Game::instance().menu_action(action, slot); }
int  dg_api_selected_class(void) { return Game::instance().selected_class; }

/* ===== 渲染输出 ===== */
int dg_api_get_status_text(char *buf, int cap) { return Game::instance().get_status_text(buf, cap); }
const uint16_t* dg_api_get_tilemap_fb(int *out_w, int *out_h) { return Game::instance().get_tilemap_fb(out_w, out_h); }
int dg_api_get_message(char *buf, int cap, int index) { return Game::instance().get_message(buf, cap, index); }
int dg_api_get_stats_text(char *buf, int cap) { return Game::instance().get_stats_text(buf, cap); }
void dg_api_get_hero_pos(int *x, int *y) { Game::instance().get_hero_pos(x, y); }
void dg_api_get_camera(int *x, int *y) { Game::instance().get_cam(x, y); }
int  dg_api_debug_dump(char *buf, int cap) { return Game::instance().debug_dump(buf, cap); }
int  dg_api_hero_buffs(char *buf, int cap) { return Game::instance().hero_buffs(buf, cap); }
void dg_api_debug_buff(int type, int duration) {
    Hero* h = Game::instance().hero;
    if (h && type > 0 && type < Buff::TYPE_COUNT) h->add_buff((Buff::Type)type, duration);
}
void dg_api_debug_m3(void) { Game::instance().debug_m3_selftest(); }
void dg_api_debug_m4(void) { Game::instance().debug_m4_selftest(); }
void dg_api_debug_m5(void) { Game::instance().debug_m5_selftest(); }
void dg_api_debug_m6(void) { Game::instance().debug_m6_fullrun(); }

/* ===== 素材透出 ===== */
bool dg_api_assets_ready(void) { return gfx::ready(); }

bool dg_api_blit_icon(int sheet, int sx, int sy, int w, int h,
                      uint16_t *dst, int dst_pitch, int dst_w, int dst_h,
                      int scale, bool key_skip) {
    gfx::Sheet s = map_sheet(sheet);
    const uint16_t* px = gfx::px(s);
    if (!px || !dst || scale < 1) return false;
    int sw = gfx::sheet_w(s);
    for (int j = 0; j < h; j++) {
        int srcy = sy + j;
        for (int i = 0; i < w; i++) {
            uint16_t c = px[srcy * sw + sx + i];
            if (key_skip && c == gfx::KEY) continue;
            /* 最近邻放大：一个源像素铺成 scale×scale 目标块 */
            for (int oy = 0; oy < scale; oy++) {
                int ty = j * scale + oy;
                if (ty >= dst_h) break;
                for (int ox = 0; ox < scale; ox++) {
                    int tx = i * scale + ox;
                    if (tx >= dst_w) break;
                    dst[ty * dst_pitch + tx] = c;
                }
            }
        }
    }
    return true;
}

/* ===== HUD / 背包 ===== */
bool dg_api_get_hud(dg_hud_t *out) { Game::instance().get_hud(out); return out != nullptr; }
int  dg_api_inv_count(void) { return Game::instance().hero->inv_count; }
bool dg_api_inv_get(int slot, dg_item_info_t *out) { return Game::instance().inv_get(slot, out); }
bool dg_api_inv_use(int slot) { return Game::instance().inv_use(slot); }
bool dg_api_inv_equip(int slot) { return Game::instance().inv_equip(slot); }
bool dg_api_inv_drop(int slot) { Game::instance().inv_drop(slot); return true; }
int  dg_api_equip_mask(void) { return Game::instance().equip_mask(); }
void dg_api_get_stats(dg_stats_t *out) { Game::instance().get_stats(out); }

/* ===== 音效队列 ===== */
bool dg_api_pop_sfx(int *id) { return Game::instance().pop_sfx(id); }

/* ===== 存档槽位 ===== */
bool dg_api_has_save(int slot) { return save::has_save(slot); }
bool dg_api_load_game(int slot) { return save::load(slot); }
bool dg_api_save_game(int slot) { return save::store(slot); }
bool dg_api_delete_save(int slot) { return save::destroy(slot); }
bool dg_api_save_info(int slot, dg_save_info_t *out) { return save::info(slot, out); }

/* ===== 场景查询 ===== */
dg_scene_t dg_api_current_scene(void) { return Game::instance().scene; }

const char* dg_api_version(void) { return "0.4.0-upstream"; }

int dg_api_run_selftest(void) { return dg_test_java_random_selftest(); }

}  /* extern "C" */
