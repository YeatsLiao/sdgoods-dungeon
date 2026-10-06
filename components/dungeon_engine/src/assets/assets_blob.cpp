/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * assets_blob.cpp —— 从 raw "assets" 分区读取 Shattered 素材
 *
 * 架构决策：素材走 raw 分区 + 编译期 offset 表，不用 FAT 文件系统。
 *
 * assets.bin 二进制格式（由 tools/pack_assets.py 生成）：
 *   [Header 32B]
 *     magic    4B  "SDGA"
 *     version  4B  = 1
 *     count    4B  条目数
 *     reserved 20B
 *   [Entry table 16B × count]
 *     hash     4B  FNV-1a(name) 低 32 位
 *     kind     4B  0=png_atlas 1=sprite 2=sound 3=music 4=font 5=properties 6=json
 *     offset   4B  相对分区起点
 *     size     4B  字节数
 *   [Data blobs...]
 *
 * 运行时 esp_partition_read 到 PSRAM，UI/Gfx 层按需引用。
 */
#include "dg_types.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>

static const char *TAG = "dg.assets";

namespace dg {
namespace assets {

static constexpr uint32_t MAGIC   = 0x41474453;   /* "SDGA" little-endian */
static constexpr int      MAX_ENT = 1024;

struct Entry {
    uint32_t hash;
    uint32_t kind;
    uint32_t offset;
    uint32_t size;
};

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved[5];
};

static const esp_partition_t* s_part = nullptr;
static Header                 s_hdr  = {};
static Entry*                 s_tab  = nullptr;   /* PSRAM 常驻 */

/* FNV-1a 32-bit —— 与 pack_assets.py 严格一致 */
static uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

bool initialize() {
    s_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!s_part) {
        ESP_LOGW(TAG, "assets partition not found (骨架阶段可忽略；等素材打包后启用)");
        return false;
    }

    esp_err_t rc = esp_partition_read(s_part, 0, &s_hdr, sizeof(Header));
    if (rc != ESP_OK || s_hdr.magic != MAGIC) {
        ESP_LOGW(TAG, "assets.bin 未烧录或 header 不符，等待 tools/pack_assets.py + flash");
        s_part = nullptr;
        return false;
    }

    if (s_hdr.count > MAX_ENT) {
        ESP_LOGE(TAG, "count=%u 超上限", s_hdr.count);
        return false;
    }

    size_t tab_bytes = s_hdr.count * sizeof(Entry);
    s_tab = (Entry*)heap_caps_malloc(tab_bytes, MALLOC_CAP_SPIRAM);
    if (!s_tab) { ESP_LOGE(TAG, "PSRAM alloc fail"); return false; }

    rc = esp_partition_read(s_part, sizeof(Header), s_tab, tab_bytes);
    if (rc != ESP_OK) { ESP_LOGE(TAG, "read entry table failed"); return false; }

    ESP_LOGI(TAG, "assets loaded: %u entries, partition size %u KB",
             s_hdr.count, (unsigned)(s_part->size / 1024));
    return true;
}

bool is_loaded() {
    return s_tab != nullptr && s_part != nullptr;
}

const void* get_asset(uint32_t name_hash, uint32_t* out_size) {
    if (!s_tab || !s_part) return nullptr;
    for (uint32_t i = 0; i < s_hdr.count; i++) {
        if (s_tab[i].hash == name_hash) {
            /* 骨架：每次从 flash 读，未来加 PSRAM LRU cache */
            void* buf = heap_caps_malloc(s_tab[i].size, MALLOC_CAP_SPIRAM);
            if (!buf) return nullptr;
            if (esp_partition_read(s_part, s_tab[i].offset, buf, s_tab[i].size) != ESP_OK) {
                heap_caps_free(buf);
                return nullptr;
            }
            if (out_size) *out_size = s_tab[i].size;
            return buf;
        }
    }
    return nullptr;
}

const void* get_asset_by_name(const char* name, uint32_t* out_size) {
    return get_asset(fnv1a(name), out_size);
}

}  /* namespace assets */
}  /* namespace dg */
