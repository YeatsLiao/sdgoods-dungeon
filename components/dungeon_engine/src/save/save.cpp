/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * save.cpp —— 游戏存档（独立 save NVS 分区）
 *
 * 架构决策：
 *   1. 存档放**独立** save 分区（非系统 nvs）—— 防止 launcher 系统操作
 *      （如恢复出厂）误清玩家进度
 *   2. 地图不序列化！只存 (seed, depth) + 英雄/背包。读档时用同一 seed 把
 *      世界「确定性重放」到存档深度（generate + spawn 走同一 JavaRandom 序列，
 *      逐层 descend 复现），再把存好的英雄数值/背包覆盖上去。这样 blob 只有
 *      ~520B，省 flash，也天然保证「同 seed 同图」的上游语义。
 *   3. Header.version 决定迁移函数；未来加字段 bump 版本，旧档按 v 迁移。
 *
 * 已知简化（v0.4，可接受）：读档后本层怪物按重放刷新（玩家此前击杀的会"复活"），
 *   因为怪物位置由重放 spawn 决定、未纳入存档。要做真·怪物持久化需把 actor 表
 *   也序列化，留待后续版本；不影响英雄进度与地图结构。
 */
#include "dg_types.h"
#include "item/item_def.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "dg.save";

namespace dg {
namespace save {

static constexpr const char* NVS_NS   = "dgsave";
static constexpr const char* SAVE_PART_LABEL = "save";
static constexpr const char* KEY_FMT  = "slot%d";
static constexpr uint32_t    SAVE_MAGIC = 0x444E4732;  /* "DNG2"（v0.4 换了 payload） */
static constexpr uint32_t    SAVE_V2    = 2;

struct SavedItem {
    int16_t kind, sub, tier, qty, str_req;
    uint8_t cursed, equipped;
};

struct SaveData {
    int32_t  hp, hp_max, str, lvl, exp, gold;
    int32_t  attack_skill, defense_skill, energy, keys;
    int32_t  depth;
    uint32_t game_time;
    int32_t  hero_x, hero_y;
    uint8_t  has_amulet;
    uint8_t  cls;
    int32_t  inv_count;
    SavedItem inv[DG_MAX_INVENTORY];
};

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t timestamp;
    uint32_t hero_class;
    uint32_t depth;
    uint32_t seed;
    uint32_t payload_len;
};

static nvs_handle_t s_h = 0;

bool initialize() {
    esp_err_t rc = nvs_flash_init_partition(SAVE_PART_LABEL);
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "save 分区未格式化（首次开机/分区表变更），擦除重建");
        nvs_flash_erase_partition(SAVE_PART_LABEL);
        rc = nvs_flash_init_partition(SAVE_PART_LABEL);
    }
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init_partition(save) failed rc=%s", esp_err_to_name(rc));
        return false;
    }
    rc = nvs_open_from_partition(SAVE_PART_LABEL, NVS_NS, NVS_READWRITE, &s_h);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open %s failed rc=%s", NVS_NS, esp_err_to_name(rc));
        return false;
    }
    ESP_LOGI(TAG, "save NVS ready (partition=%s namespace=%s)", SAVE_PART_LABEL, NVS_NS);
    return true;
}

static void slot_key(int slot, char* out, int cap) { snprintf(out, cap, KEY_FMT, slot); }

bool has_save(int slot) {
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    char key[16]; slot_key(slot, key, sizeof(key));
    size_t len = 0;
    return nvs_get_blob(s_h, key, nullptr, &len) == ESP_OK && len >= sizeof(Header) + sizeof(SaveData);
}

bool store(int slot) {
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    Game& g = Game::instance();
    Hero* h = g.hero;
    char key[16]; slot_key(slot, key, sizeof(key));

    SaveData d = {};
    d.hp = h->hp; d.hp_max = h->hp_max; d.str = h->str; d.lvl = h->lvl;
    d.exp = h->exp; d.gold = h->gold;
    d.attack_skill = h->attack_skill; d.defense_skill = h->defense_skill;
    d.energy = h->energy; d.keys = h->keys;
    d.depth = g.depth; d.game_time = g.game_time;
    d.hero_x = h->x; d.hero_y = h->y;
    d.has_amulet = h->has_amulet ? 1 : 0;
    d.cls = (uint8_t)h->cls;
    d.inv_count = h->inv_count;
    for (int i = 0; i < h->inv_count && i < DG_MAX_INVENTORY; i++) {
        Item* it = h->inventory[i];
        if (!it) { d.inv_count = i; break; }
        d.inv[i].kind = (int16_t)it->kind;
        d.inv[i].sub = it->sub; d.inv[i].tier = it->tier; d.inv[i].qty = it->qty;
        d.inv[i].str_req = it->str_req;
        d.inv[i].cursed = it->cursed; d.inv[i].equipped = it->equipped;
    }

    Header hd = {
        .magic = SAVE_MAGIC,
        .version = SAVE_V2,
        .timestamp = (uint32_t)g.game_time,
        .hero_class = (uint32_t)h->cls,
        .depth = (uint32_t)g.depth,
        .seed = g.seed,
        .payload_len = (uint32_t)sizeof(SaveData),
    };

    uint8_t buf[sizeof(Header) + sizeof(SaveData)];
    memcpy(buf, &hd, sizeof(hd));
    memcpy(buf + sizeof(hd), &d, sizeof(d));

    esp_err_t rc = nvs_set_blob(s_h, key, buf, sizeof(buf));
    if (rc != ESP_OK) { ESP_LOGE(TAG, "store failed %s", esp_err_to_name(rc)); return false; }
    nvs_commit(s_h);
    ESP_LOGI(TAG, "slot %d saved depth=%u seed=0x%08x inv=%d", slot, hd.depth, hd.seed, d.inv_count);
    return true;
}

/* 把英雄背包清空（归还池子），供读档重建 */
static void clear_inventory(Hero* h) {
    for (int i = 0; i < h->inv_count; i++) {
        if (h->inventory[i]) Game::instance().free_item(h->inventory[i]);
        h->inventory[i] = nullptr;
    }
    h->inv_count = 0;
    h->equipped_weapon = h->equipped_armor = h->equipped_ring = nullptr;
}

bool load(int slot) {
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    char key[16]; slot_key(slot, key, sizeof(key));

    uint8_t buf[sizeof(Header) + sizeof(SaveData)];
    size_t len = sizeof(buf);
    esp_err_t rc = nvs_get_blob(s_h, key, buf, &len);
    if (rc != ESP_OK || len < sizeof(Header) + sizeof(SaveData)) {
        ESP_LOGW(TAG, "slot %d load fail rc=%s len=%u", slot, esp_err_to_name(rc), (unsigned)len);
        return false;
    }
    Header hd; memcpy(&hd, buf, sizeof(hd));
    SaveData d; memcpy(&d, buf + sizeof(hd), sizeof(d));
    if (hd.magic != SAVE_MAGIC) { ESP_LOGW(TAG, "bad magic"); return false; }

    Game& g = Game::instance();
    /* 1. 用 seed 重放到存档深度：new_game 建 0 层，再逐层 generate+spawn 复现世界 */
    g.new_game((int)hd.hero_class, hd.seed);
    for (int dep = 1; dep <= (int)hd.depth && dep < DG_MAX_DEPTH; dep++) {
        g.depth = dep;
        g.level->generate(hd.seed, dep);
        g.level->depth = dep;
        Hero* h = g.hero;
        h->set_pos(g.level->entrance_pos % DG_MAP_W, g.level->entrance_pos / DG_MAP_W);
        h->from_x = h->x; h->from_y = h->y; h->move_anim = 255;
        g.spawn_level_content();
    }

    /* 2. 覆盖英雄数值与背包（重放给的是「干净新局」的默认值，需还原存档进度） */
    Hero* h = g.hero;
    clear_inventory(h);
    h->hp = d.hp; h->hp_max = d.hp_max; h->str = d.str; h->lvl = d.lvl;
    h->exp = d.exp; h->gold = d.gold;
    h->attack_skill = d.attack_skill; h->defense_skill = d.defense_skill;
    h->energy = d.energy; h->keys = d.keys;
    h->has_amulet = d.has_amulet;
    g.game_time = d.game_time;

    for (int i = 0; i < d.inv_count && i < DG_MAX_INVENTORY; i++) {
        Item* it = g.alloc_item();
        if (!it) break;
        fill_item(it, d.inv[i].kind, d.inv[i].sub, d.inv[i].tier);
        it->qty = d.inv[i].qty;
        it->cursed = d.inv[i].cursed;
        it->equipped = d.inv[i].equipped;
        it->x = it->y = -1;
        h->inventory[h->inv_count++] = it;
        if (it->equipped == EQ_WEAPON) h->equipped_weapon = it;
        else if (it->equipped == EQ_ARMOR) h->equipped_armor = it;
        else if (it->equipped == EQ_RING) h->equipped_ring = it;
    }

    /* 3. 英雄落回存档坐标 */
    if (hd.depth == (uint32_t)g.depth) {
        int tx = d.hero_x, ty = d.hero_y;
        if (tx >= 0 && tx < DG_MAP_W && ty >= 0 && ty < DG_MAP_H && g.level->passable(tx, ty)) {
            g.level->at(h->x, h->y).actor = nullptr;
            h->set_pos(tx, ty); h->from_x = tx; h->from_y = ty; h->move_anim = 255;
            g.level->at(tx, ty).actor = h;
        }
    }

    g.recalc_fov();
    g.scene = DG_SCENE_IN_GAME;
    g.fb_dirty = true;
    g.log("进度已读取。");
    ESP_LOGI(TAG, "slot %d loaded depth=%u v=%u", slot, hd.depth, hd.version);
    return true;
}

bool destroy(int slot) {
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    char key[16]; slot_key(slot, key, sizeof(key));
    esp_err_t rc = nvs_erase_key(s_h, key);
    if (rc != ESP_OK && rc != ESP_ERR_NVS_NOT_FOUND) return false;
    nvs_commit(s_h);
    return true;
}

bool info(int slot, dg_save_info_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    char key[16]; slot_key(slot, key, sizeof(key));
    uint8_t buf[sizeof(Header) + sizeof(SaveData)];
    size_t len = sizeof(buf);
    esp_err_t rc = nvs_get_blob(s_h, key, buf, &len);
    if (rc != ESP_OK || len < sizeof(Header)) return false;
    Header hd; memcpy(&hd, buf, sizeof(hd));
    if (hd.magic != SAVE_MAGIC) return false;
    out->valid = 1;
    out->hero_class = (int16_t)hd.hero_class;
    out->depth = (int16_t)hd.depth;
    out->game_time = hd.timestamp;
    if (len >= sizeof(Header) + sizeof(SaveData)) {
        SaveData d; memcpy(&d, buf + sizeof(hd), sizeof(d));
        out->gold = d.gold;
    }
    return true;
}

}  /* namespace save */
}  /* namespace dg */
