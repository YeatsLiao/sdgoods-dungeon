/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * save.cpp —— 游戏存档（独立 save NVS 分区）
 *
 * 架构决策：
 *   1. 存档放**独立** save 分区（非系统 nvs）—— 防止 launcher 系统操作
 *      （如恢复出厂）误清玩家进度；本仓库是独立固件，更应隔离
 *   2. 每槽 64KB blob（含 header + Bundle 序列化的世界状态）
 *   3. Header 里 version 字段决定加载时的迁移函数；未来加内容不改旧存档
 */
#include "dg_types.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "dg.save";

namespace dg {
namespace save {

static constexpr const char* NVS_NS   = "dgsave";
static constexpr const char* SAVE_PART_LABEL = "save";   /* partitions.csv 分区名 */
static constexpr const char* KEY_FMT  = "slot%d";
static constexpr uint32_t    SAVE_MAGIC = 0x444E4731;  /* "DNG1" */
static constexpr uint32_t    SAVE_V1    = 1;

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
    /* 先初始化独立 "save" 分区（partition name，见 platform/partitions.csv），
     * 再从该分区打开命名空间 —— 绝不用 nvs_open() 裸开：那会落到系统 "nvs"
     * 分区，与文件头「隔离于系统 NVS」的架构决策相违。 */
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

bool has_save(int slot) {
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    char key[16]; snprintf(key, sizeof(key), KEY_FMT, slot);
    size_t len = 0;
    return nvs_get_blob(s_h, key, nullptr, &len) == ESP_OK && len > sizeof(Header);
}

bool store(int slot) {
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    Game& g = Game::instance();
    char key[16]; snprintf(key, sizeof(key), KEY_FMT, slot);

    Header h = {
        .magic = SAVE_MAGIC,
        .version = SAVE_V1,
        .timestamp = (uint32_t)(g.game_time),
        .hero_class = (uint32_t)g.hero->cls,
        .depth = (uint32_t)g.depth,
        .seed = g.seed,
        .payload_len = 0,   /* TODO: Bundle 序列化 */
    };
    esp_err_t rc = nvs_set_blob(s_h, key, &h, sizeof(h));
    if (rc != ESP_OK) { ESP_LOGE(TAG, "store failed %s", esp_err_to_name(rc)); return false; }
    nvs_commit(s_h);
    ESP_LOGI(TAG, "slot %d saved depth=%u seed=0x%08x", slot, h.depth, h.seed);
    return true;
}

bool load(int slot) {
    if (slot < 0 || slot >= DG_SAVE_SLOTS || s_h == 0) return false;
    char key[16]; snprintf(key, sizeof(key), KEY_FMT, slot);
    Header h; size_t len = sizeof(h);
    esp_err_t rc = nvs_get_blob(s_h, key, &h, &len);
    if (rc != ESP_OK || h.magic != SAVE_MAGIC) return false;

    /* TODO: 迁移 + 反序列化 payload；骨架版仅开新局占位 */
    Game::instance().new_game((int)h.hero_class, h.seed);
    ESP_LOGI(TAG, "slot %d loaded v=%u", slot, h.version);
    return true;
}

}  /* namespace save */
}  /* namespace dg */
