/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * gfx.cpp —— 烘焙图集注册表加载 + blit 原语 + 迷你像素字
 */
#include "gfx/gfx.h"
#include "dg_types.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>

static const char *TAG = "dg.gfx";

namespace dg {
namespace gfx {

/* RGB5 容器头（与 pack_assets.py::bake_rgb565 对偶） */
struct Rgb5Header {
    char     magic[4];   /* "RGB5" */
    uint16_t w;
    uint16_t h;
};

/* 注册表名字：顺序必须与 gfx.h::Sheet 一一对应，且与 pack_assets.py::BAKE_MAP
 * 的产物名严格一致（FNV-1a 查表，拼错就是 nullptr）。 */
static const char* const k_names[SH_COUNT] = {
    "tiles/sewers.rgb565",
    "tiles/prison.rgb565",
    "tiles/caves.rgb565",
    "sprites/rat.rgb565",
    "sprites/snake.rgb565",
    "sprites/gnoll.rgb565",
    "sprites/swarm.rgb565",
    "sprites/crab.rgb565",
    "sprites/slime.rgb565",
    "sprites/skeleton.rgb565",
    "sprites/thief.rgb565",
    "sprites/dm100.rgb565",
    "sprites/guard.rgb565",
    "sprites/necromancer.rgb565",
    "sprites/bat.rgb565",
    "sprites/brute.rgb565",
    "sprites/shaman.rgb565",
    "sprites/spinner.rgb565",
    "sprites/dm200.rgb565",
    "sprites/mimic.rgb565",
    "sprites/goo.rgb565",
    "sprites/tengu.rgb565",
    "sprites/king.rgb565",
    "sprites/hero_warrior.rgb565",
    "sprites/hero_mage.rgb565",
    "sprites/hero_rogue.rgb565",
    "sprites/hero_huntress.rgb565",
    "sprites/items.rgb565",
    "interfaces/icons.rgb565",
};

struct Entry {
    const uint16_t* px;
    int             w;
    int             h;
    uint8_t         frames;      /* 第 0 行有墨连续帧数，0 = 未算 */
};

static Entry s_tab[SH_COUNT];
static uint16_t* s_dim[3];       /* 三张 tileset 的暗化副本（记忆态） */
static bool      s_loaded = false;

/* ---- 3×5 迷你点阵（bit4..bit0 自上而下，每行 3 bit 从左到右）---- */
struct Glyph { char c; uint16_t rows[5]; };
static const Glyph k_glyphs[] = {
    { '0', { 0x7, 0x5, 0x5, 0x5, 0x7 } },
    { '1', { 0x2, 0x6, 0x2, 0x2, 0x7 } },
    { '2', { 0x7, 0x1, 0x7, 0x4, 0x7 } },
    { '3', { 0x7, 0x1, 0x7, 0x1, 0x7 } },
    { '4', { 0x5, 0x5, 0x7, 0x1, 0x1 } },
    { '5', { 0x7, 0x4, 0x7, 0x1, 0x7 } },
    { '6', { 0x7, 0x4, 0x7, 0x5, 0x7 } },
    { '7', { 0x7, 0x1, 0x2, 0x2, 0x2 } },
    { '8', { 0x7, 0x5, 0x7, 0x5, 0x7 } },
    { '9', { 0x7, 0x5, 0x7, 0x1, 0x7 } },
    { '+', { 0x0, 0x2, 0x7, 0x2, 0x0 } },
    { '-', { 0x0, 0x0, 0x7, 0x0, 0x0 } },
    { '!', { 0x2, 0x2, 0x2, 0x0, 0x2 } },
    { 'x', { 0x0, 0x5, 0x2, 0x5, 0x0 } },
    { '?', { 0x7, 0x1, 0x3, 0x0, 0x2 } },
    { '.', { 0x0, 0x0, 0x0, 0x0, 0x2 } },
    { 'L', { 0x4, 0x4, 0x4, 0x4, 0x7 } },
    { 'V', { 0x5, 0x5, 0x5, 0x5, 0x2 } },
    { 'M', { 0x5, 0x7, 0x7, 0x5, 0x5 } },
    { 'S', { 0x3, 0x4, 0x2, 0x1, 0x6 } },
    { 'I', { 0x7, 0x2, 0x2, 0x2, 0x7 } },
};
static const uint16_t k_blank[5] = { 0, 0, 0, 0, 0 };

static const uint16_t* glyph_rows(char c) {
    for (unsigned i = 0; i < sizeof(k_glyphs) / sizeof(k_glyphs[0]); i++) {
        if (k_glyphs[i].c == c) return k_glyphs[i].rows;
    }
    return k_blank;
}

/* ---- 加载 ---- */

static bool load_one(Sheet s) {
    uint32_t size = 0;
    const void* blob = assets::get_asset_by_name(k_names[s], &size);
    if (!blob) {
        ESP_LOGW(TAG, "烘焙图缺失：%s", k_names[s]);
        return false;
    }
    Rgb5Header hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    if (size < sizeof(hdr) || memcmp(hdr.magic, "RGB5", 4) != 0 ||
        size != sizeof(hdr) + (uint32_t)hdr.w * hdr.h * 2) {
        ESP_LOGE(TAG, "%s 不是合法 RGB5 容器", k_names[s]);
        heap_caps_free(const_cast<void*>(blob));
        return false;
    }
    s_tab[s].px = (const uint16_t*)((const uint8_t*)blob + sizeof(hdr));
    s_tab[s].w  = hdr.w;
    s_tab[s].h  = hdr.h;
    return true;
}

/* 第 0 行里「前导有墨格」的个数 = idle 动画帧数。上游帧图常把 idle 放最前，
 * 后面留空，按整行取帧会让精灵忽隐忽现。阈值 8 个像素避开单点杂色。 */
static int count_frames(Sheet s) {
    const Entry& e = s_tab[s];
    if (!e.px) return 1;
    int cells = e.w / 16;
    if (cells < 1) cells = 1;
    if (e.h < 16) return 1;
    int n = 0;
    for (int c = 0; c < cells; c++) {
        int ink = 0;
        for (int j = 0; j < 16; j++) {
            const uint16_t* row = e.px + j * e.w + c * 16;
            for (int i = 0; i < 16; i++) if (row[i] != KEY) ink++;
        }
        if (ink < 8) break;
        n++;
    }
    return n ? n : 1;
}

static uint16_t dim565(uint16_t c) {
    /* 记忆态： darker + 去饱和。此前只简单 ×9/32 压暗，苔原随机花纹、
     * 墙棱条在暗区仍然清晰可见， explored 区域一大就像满屏阴影斑块。
     * 把各通道向统一亮度靠拢（保 1/3 原色）再整体压到 ≈18%，
     * 记忆区变成均匀暗灰：能辨轮廓不抢戏。 */
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    int lum = (r * 77 + g * 151 + b * 28) >> 8;      /* 0..31 */
    int lum6 = lum * 2;                              /* 6bit 空间的亮度 */
    r = ((lum * 2 + r) / 3) * 6 / 32;
    g = ((lum6 * 2 + g) / 3) * 6 / 32;
    b = ((lum * 2 + b) / 3) * 6 / 32;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

bool load() {
    if (s_loaded) return true;
    if (!assets::is_loaded()) {
        ESP_LOGW(TAG, "assets 分区无素材，渲染走棋盘占位");
        return false;
    }
    int ok = 0;
    for (int i = 0; i < SH_COUNT; i++) {
        if (load_one((Sheet)i)) {
            ok++;
            s_tab[i].frames = (uint8_t)count_frames((Sheet)i);
        }
    }
    /* 记忆态暗化副本：一次性成本，换渲染零乘法。缺 tileset 时整体放弃。 */
    for (int c = 0; c < 3; c++) {
        const Entry& e = s_tab[(Sheet)(SH_TILES_SEWERS + c)];
        if (!e.px) continue;
        s_dim[c] = (uint16_t*)heap_caps_malloc((size_t)e.w * e.h * 2, MALLOC_CAP_SPIRAM);
        if (!s_dim[c]) { ESP_LOGE(TAG, "dim alloc fail ch%d", c); continue; }
        int n = e.w * e.h;
        for (int i = 0; i < n; i++) s_dim[c][i] = dim565(e.px[i]);
    }
    ESP_LOGI(TAG, "烘焙图集就绪 %d/%d 张", ok, (int)SH_COUNT);
    if (!s_tab[SH_TILES_SEWERS].px) return false;
    s_loaded = true;
    return true;
}

bool ready() { return s_loaded; }

const uint16_t* px(Sheet s)     { return (s < SH_COUNT) ? s_tab[s].px : nullptr; }
int sheet_w(Sheet s)            { return (s < SH_COUNT) ? s_tab[s].w : 0; }
int sheet_h(Sheet s)            { return (s < SH_COUNT) ? s_tab[s].h : 0; }
int anim_frames(Sheet s)        { return (s < SH_COUNT && s_tab[s].frames) ? s_tab[s].frames : 1; }

static int clamp_ch(int c) {
    if (c < 0) return 0;
    if (c > 2) return 2;
    return c;
}

const uint16_t* tileset(int chapter) {
    int c = clamp_ch(chapter);
    return s_tab[(Sheet)(SH_TILES_SEWERS + c)].px;
}

const uint16_t* tileset_dim(int chapter) {
    int c = clamp_ch(chapter);
    const uint16_t* d = s_dim[c];
    return d ? d : s_tab[(Sheet)(SH_TILES_SEWERS + c)].px;
}

/* ---- blit 原语 ---- */

void blit(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
          const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h) {
    if (!sheet || dx >= fb_w || dy >= fb_h) return;
    int x_end = dx + w > fb_w ? fb_w - dx : w;
    int y_end = dy + h > fb_h ? fb_h - dy : h;
    if (x_end <= 0 || y_end <= 0) return;
    if (dx < 0) { sx += -dx; x_end += dx; dx = 0; if (x_end <= 0) return; }
    if (dy < 0) { sy += -dy; y_end += dy; dy = 0; if (y_end <= 0) return; }
    for (int j = 0; j < y_end; j++) {
        memcpy(&fb[(dy + j) * fb_w + dx],
               &sheet[(sy + j) * sheet_w + sx],
               (size_t)x_end * sizeof(uint16_t));
    }
}

void blit_key(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
              const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h) {
    if (!sheet) return;
    for (int j = 0; j < h; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= fb_h) continue;
        const uint16_t* src = &sheet[(sy + j) * sheet_w + sx];
        uint16_t* dst = &fb[ty * fb_w + dx];
        for (int i = 0; i < w; i++) {
            int tx = dx + i;
            if (tx < 0 || tx >= fb_w) continue;
            uint16_t c = src[i];
            if (c != KEY) dst[i] = c;
        }
    }
}

void blit_key_dim(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
                  const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h) {
    if (!sheet) return;
    for (int j = 0; j < h; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= fb_h) continue;
        const uint16_t* src = &sheet[(sy + j) * sheet_w + sx];
        uint16_t* dst = &fb[ty * fb_w + dx];
        for (int i = 0; i < w; i++) {
            int tx = dx + i;
            if (tx < 0 || tx >= fb_w) continue;
            uint16_t c = src[i];
            if (c != KEY) dst[i] = dim565(c);
        }
    }
}

/* 闪白：把像素三个通道都抬到接近满，保留描边轮廓（比直接填白块更像「被打」） */
static inline uint16_t flash565(uint16_t c) {
    uint16_t r = (uint16_t)(((c >> 11) & 0x1F) | 0x18);
    uint16_t g = (uint16_t)(((c >> 5)  & 0x3F) | 0x30);
    uint16_t b = (uint16_t)((c & 0x1F) | 0x18);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void blit_flash(uint16_t* fb, int fb_w, int fb_h, int dx, int dy,
                const uint16_t* sheet, int sheet_w, int sx, int sy, int w, int h) {
    if (!sheet) return;
    for (int j = 0; j < h; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= fb_h) continue;
        const uint16_t* src = &sheet[(sy + j) * sheet_w + sx];
        uint16_t* dst = &fb[ty * fb_w + dx];
        for (int i = 0; i < w; i++) {
            int tx = dx + i;
            if (tx < 0 || tx >= fb_w) continue;
            uint16_t c = src[i];
            if (c != KEY) dst[i] = flash565(c);
        }
    }
}

void fill_rect(uint16_t* fb, int fb_w, int fb_h, int dx, int dy, int w, int h,
               uint16_t color) {
    for (int j = 0; j < h; j++) {
        int ty = dy + j;
        if (ty < 0 || ty >= fb_h) continue;
        for (int i = 0; i < w; i++) {
            int tx = dx + i;
            if (tx < 0 || tx >= fb_w) continue;
            fb[ty * fb_w + tx] = color;
        }
    }
}

/* ---- 迷你像素字 ---- */

uint16_t rgb565(uint8_t r8, uint8_t g8, uint8_t b8) {
    return (uint16_t)((((uint16_t)(r8 >> 3)) << 11) |
                      (((uint16_t)(g8 >> 2)) << 5) |
                       ((uint16_t)(b8 >> 3)));
}

int text_px_w(const char* s) {
    int n = 0;
    for (const char* p = s; *p; p++) n++;
    return n ? n * 4 - 1 : 0;
}

void text_px(uint16_t* fb, int fb_w, int fb_h, int x, int y,
             const char* s, uint16_t color) {
    int cx = x;
    for (; *s; s++, cx += 4) {
        const uint16_t* rows = glyph_rows(*s);
        for (int j = 0; j < 5; j++) {
            int ty = y + j;
            if (ty < 0 || ty >= fb_h) continue;
            uint16_t bits = rows[j];
            for (int i = 0; i < 3; i++) {
                if (!(bits & (4 >> i))) continue;
                int tx = cx + i;
                if (tx < 0 || tx >= fb_w) continue;
                fb[ty * fb_w + tx] = color;
            }
        }
    }
}

}  /* namespace gfx */
}  /* namespace dg */
