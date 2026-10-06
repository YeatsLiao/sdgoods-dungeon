/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dg_types.h —— 引擎内部核心类型（Actor / Item / Level / Game 前向声明）
 *
 * 设计约束（架构决策）：
 *   1. 无 heap 分配在 hot path（act() / render）—— 所有 Actor / Item 从
 *      Game 内部的静态池分配（kActorPool / kItemPool）
 *   2. 禁止 exceptions / RTTI —— 见 sdkconfig.defaults
 *   3. Tile 状态用固定数组 Tile map[DG_MAP_W * DG_MAP_H]，32×32 = 1024 项
 *      × 8B/tile ≈ 8KB，放内部 SRAM 快访问
 *   4. Actor 用虚函数 act()，回合制调度器按时间戳挑最早的执行
 */
#ifndef DG_TYPES_H
#define DG_TYPES_H

#include <stdint.h>
#include "dungeon_api.h"       /* 常量与枚举 */

namespace dg {

/* 格子坐标 packed 到 16 位（32×32 地图最大 1023，uint16_t 足够） */
typedef uint16_t pos_t;

class Level;
class Actor;
class Hero;
class Mob;
class Item;
class Buff;
class JavaRandom;

/* ===== Tile ===== */
struct Tile {
    dg_terrain_t terr : 5;
    uint16_t     vis_seen     : 1;    /* 玩家曾到过，视觉记忆 */
    uint16_t     vis_magical  : 1;    /* 魔法视野 */
    uint16_t     vis_current  : 1;    /* 本帧可见（FOV 内） */
    uint16_t     explored     : 1;    /* 永久揭雾 */
    uint16_t     reserved     : 3;
    uint8_t      ch;                  /* 章节主题（选 tile atlas 用） */
    Item*        item;                /* 掉地上的物品 */
    Actor*       actor;               /* 占位的 Actor（Hero 或 Mob） */
};

/* ===== Buff / Status Effect ===== */
class Buff {
public:
    enum Type : uint8_t {
        NONE = 0, BURNING, POISON, SLEEP, PARALYSIS, FRIGHT, ROOTS,
        INVISIBILITY, MIND_VISION, LEVITATION, OINTMENT, HUNTERS_CALL,
        TYPE_COUNT
    };
    Type     type;
    int      duration;        /* 剩余 tick；-1 表示永久 */
    Actor*   owner;
    Buff*    next;            /* intrusive linked list */

    Buff(Type t, int dur) : type(t), duration(dur), owner(nullptr), next(nullptr) {}
    virtual ~Buff() = default;
    virtual void attach() {}
    virtual void detach() {}
    virtual bool act() { return --duration > 0; }   /* true=继续存在 */
};

/* ===== Actor 基类 ===== */
class Actor {
public:
    int     x = 0, y = 0;
    int     hp = 1, hp_max = 1;
    int     speed = 1;              /* 行动速度倍率 */
    uint32_t alignment = 0;         /* 0=NEUTRAL 1=ENEMY 2=ALLY */
    uint32_t flags = 0;             /* property bitfield */
    Buff*   first_buff = nullptr;   /* intrusive list head */
    Level*  level = nullptr;
    uint32_t ready_at = 0;          /* 下一次可行动的 game_time */
    const char* name_key = nullptr; /* messages_zh.properties 里的键 */

    virtual ~Actor() = default;
    virtual int  act() = 0;         /* 返回耗时 tick，或 -1 表示等待输入 */
    virtual bool is_alive() const { return hp > 0; }
    virtual void damage(int dmg, const char* src);
    virtual void die();

    pos_t pos() const { return (pos_t)(x + y * DG_MAP_W); }
    void  set_pos(int _x, int _y) { x = _x; y = _y; }

    void add_buff(Buff* b);
    void remove_buff(Buff::Type t);
    Buff* get_buff(Buff::Type t);
    bool has_buff(Buff::Type t) { return get_buff(t) != nullptr; }
};

/* ===== Hero（玩家）===== */
class Hero : public Actor {
public:
    int str = 10;      /* 力量 */
    int exp = 0;
    int lvl = 1;
    int gold = 0;
    int attack_skill = 50;
    int defense_skill = 4;
    dg_class_t cls = DG_CLASS_WARRIOR;

    Item*           inventory[DG_MAX_INVENTORY] = {0};
    int             inv_count = 0;
    Item*           equipped_weapon = nullptr;
    Item*           equipped_armor  = nullptr;

    virtual int act() override;              /* 返回 -1 等待输入 */
    int  attack(Actor* enemy);
    int  defense(Mob* enemy);
    bool pickup(Item* it);
    bool equip(Item* it);
    bool use(int slot);
};

/* ===== Mob（怪物基类）===== */
class Mob : public Actor {
public:
    int  attack_min = 1, attack_max = 1;
    int  defense = 0;
    int  xp_in_kill = 0;
    int  max_level = 5;         /* 出现层数区间上界 */
    uint8_t see_range = 8;
    uint8_t search_range = 4;

    virtual int act() override;
    virtual int damageRoll()   { return attack_min + rand_int(attack_max - attack_min + 1); }
    virtual int attackSkill(Actor* target);
    virtual int defenseSkill(Actor* target);
    virtual bool surprised_by(Actor* target);

    /* 状态机 */
    enum State : uint8_t { SLEEPING, WANDERING, HUNTING, FLEEING, PASSIVE };
    State state = SLEEPING;

protected:
    int rand_int(int bound);
};

/* ===== Item ===== */
class Item {
public:
    enum Kind : uint8_t {
        K_WEAPON, K_ARMOR, K_POTION, K_SCROLL, K_RING, K_WAND,
        K_FOOD, K_KEY, K_GOLD, K_AMULET, K_STONE, K_ARTIFACT,
        KIND_COUNT
    };
    Kind        kind;
    int         x = 0, y = 0;
    int         qty = 1;
    uint16_t    sprite_index = 0;
    const char* name_key = nullptr;

    virtual ~Item() = default;
    virtual void on_pickup(Hero* h)   { (void)h; }
    virtual void on_equip(Hero* h)    { (void)h; }
    virtual void on_use(Hero* h)      { (void)h; }
    virtual void on_drop(Level* l);   /* 掉到 (x,y) 的地上 */
};

/* ===== Level（单层地图）===== */
class Level {
public:
    static constexpr int LENGTH = DG_MAP_W * DG_MAP_H;

    Tile   tiles[LENGTH];
    int    depth = 0;
    uint32_t seed = 0;
    Hero*  hero = nullptr;         /* back-pointer */
    Actor* actors[64] = {0};       /* 本层所有 Actor（含 Hero），最多 64 */
    int    actor_count = 0;
    pos_t  entrance_pos = 0;
    pos_t  exit_pos = 0;

    virtual bool generate(uint32_t seed);   /* 程序化生成，返回成功 */
    virtual void create_mobs_and_items();
    virtual ~Level() = default;

    Tile& at(int x, int y)             { return tiles[x + y * DG_MAP_W]; }
    const Tile& at(int x, int y) const { return tiles[x + y * DG_MAP_W]; }
    bool passable(int x, int y) const;
    bool flammable(int x, int y) const { return false; }
    int  distance(int ax, int ay, int bx, int by) const;
};

/* ===== Game（顶层状态机 + 单例）===== */
class Game {
public:
    static Game& instance();

    void init();
    void new_game(int hero_class, uint32_t seed);
    void tick();                                    /* 由 tick_if_needed 分发 */
    void on_tap(int gx, int gy);
    void on_long_press(int gx, int gy);
    void on_button(dg_btn_id_t btn);

    Hero*        hero  = nullptr;
    Level*       level = nullptr;
    int          depth = 0;
    uint32_t     seed  = 0;
    uint32_t     game_time = 0;                     /* 全局回合时间（tick） */
    dg_scene_t   scene = DG_SCENE_TITLE;
    JavaRandom*  rng   = nullptr;                   /* 世界 RNG，影响关卡生成 */
    JavaRandom*  ui_rng = nullptr;                  /* 不影响世界状态的杂项 RNG */

    /* UI 拉取用 */
    int  get_status_text(char* buf, int cap);
    const uint16_t* get_tilemap_fb(int* w, int* h);
    int  get_message(char* buf, int cap, int index);

    /* v0.2 回合流与渲染 */
    int  cam_x = 0, cam_y = 0;                /* 主视窗左上 tile（渲染时算，视口点击换算也用） */
    int  path_queue[64];                       /* 自动寻路分步队列（packed pos） */
    int  path_len = 0, path_head = 0;
    uint32_t last_step_ms = 0;                 /* 自动行走分步节奏门控（ms） */

    void recalc_fov();
    bool hero_try_step(int gx, int gy);        /* 走/砍/拾取一步；成功返回 true */
    void advance_mobs();                       /* 怪物回合（英雄行动后调） */
    bool descend_stairs();                     /* 踩到 EXIT → 生成下一层 */
    void spawn_level_content();                /* 本层怪物 + 掉落物（静态池） */
    Mob* alloc_mob();

    /* 静态内存池（避免 heap） */
    static constexpr int kMaxMob    = 32;
    static constexpr int kMaxItem   = 64;
    static constexpr int kMaxBuff   = 32;

    /* 消息 log */
    static constexpr int kLogLines = 32;
    const char* log_lines[kLogLines];   /* 存 messages_zh.properties 的键 */
    int         log_head = 0;
    void log(const char* key);

    /* 渲染 dirty flag */
    bool fb_dirty = true;

private:
    Game();
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
};

/* ===== A* 寻路 ===== */
namespace pathfinder {
    /* 从 level 上 (sx,sy) 到 (tx,ty) 找最短可通行路径。
     * 输出步骤数；steps 数组最多 cap 项，每项 (x, y) packed 到 int。
     * 返回 0 表示不可达。 */
    int  find_path(Level* level, int sx, int sy, int tx, int ty,
                   int* steps, int cap);
}

/* ===== Assets blob reader ===== */
namespace assets {
    bool initialize();           /* 从 raw 分区加载 offset 表 */
    bool is_loaded();            /* 分区存在且 SDGA header 有效 */
    const void* get_asset(uint32_t name_hash, uint32_t* out_size);
    const void* get_asset_by_name(const char* name, uint32_t* out_size);
}

/* ===== Save ===== */
namespace save {
    bool initialize();           /* 打开 save NVS 分区，命名空间 "dgsave" */
    bool has_save(int slot);
    bool load(int slot);
    bool store(int slot);
}

}  /* namespace dg */

#endif
