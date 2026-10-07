/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * dg_types.h —— 引擎内部核心类型（Tile / Actor / Item / Level / Game）
 *
 * 设计约束（架构决策）：
 *   1. 无 heap 分配在 hot path（act() / render）—— 所有 Actor / Item / Buff
 *      从 Game 内部的静态池分配，池槽位用位图标记占用
 *   2. 禁止 exceptions / RTTI —— 见 sdkconfig.defaults
 *   3. Tile 状态用固定数组 Tile map[DG_MAP_W * DG_MAP_H]，32×32 = 1024 项
 *      × 12B/tile ≈ 12KB，放内部 SRAM 快访问
 *   4. Actor 用虚函数 act()，回合制调度器由英雄行动驱动（非全局时间片）
 *   5. v0.4 起 Item / Mob 改为**数据驱动**（kind + sub + tier 查定义表），
 *      不再为每种物品派生子类 —— 34 格背包 × 30 余种物品的类层级在 ESP32
 *      上既费 flash 又费栈，查表 + switch 更好维护
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
class JavaRandom;
struct MobSpec;

/* ===== Tile ===== */
struct Tile {
    uint16_t terr        : 5;    /* dg_terrain_t（v0.4 共 16 种） */
    uint16_t vis_seen    : 1;    /* 玩家曾到过，视觉记忆 */
    uint16_t vis_magical : 1;    /* 魔法视野 */
    uint16_t vis_current : 1;    /* 本帧可见（FOV 内） */
    uint16_t explored    : 1;    /* 永久揭雾 */
    uint16_t trap_known  : 1;    /* 陷阱已被发现（未发现时按地板画） */
    uint16_t chest_open  : 1;    /* 宝箱已被打开 */
    uint16_t mimic       : 1;    /* 宝箱怪伪装（开箱时变身成 Mob，M5）*/
    uint16_t reserved    : 4;
    uint8_t  variant;            /* 生成期算好的装饰哈希 0..255（渲染零成本） */
    Item*    item;               /* 掉地上的物品 */
    Actor*   actor;              /* 占位的 Actor（Hero 或 Mob） */
};

/* ===== Buff / Status Effect =====
 * v0.4 落地：SLOW / HASTE / INVISIBILITY / MINDVISION / LEVITATION / POISON /
 * BURNING / SLEEP / PARALYSIS / FRIGHT / ROOTS / OINTMENT。全部是「计时器 +
 * 若干判定读数」，没有多态行为，所以退化成结构体 + 静态池（旧版 remove_buff
 * 里对池对象 delete 是致命 bug，一并修掉）。 */
struct Buff {
    enum Type : uint8_t {
        NONE = 0,
        SLOW, HASTE, INVISIBILITY, MINDVISION, LEVITATION,
        POISON, BURNING, SLEEP, PARALYSIS, FRIGHT, ROOTS, OINTMENT,
        TYPE_COUNT
    };
    Type     type;
    int      duration;        /* 剩余回合；-1 = 永久（装备类） */
    int      value;           /* 附价值（如力量加成、隐身来源） */
    Actor*   owner;
    Buff*    next;            /* intrusive linked list */
};

/* ===== Actor 基类 ===== */
class Actor {
public:
    int      x = 0, y = 0;
    int      from_x = 0, from_y = 0;   /* 平滑移动：本段动画起点 */
    int      hp = 1, hp_max = 1;
    int      speed = 16;               /* 行动速度（1/16 定点：16=1.0×，32=2.0×，上游口径）*/
    uint16_t act_accum = 0;            /* 速度调度累加器（1/16 回合），M2 真实速度 */
    uint8_t  sheet = 0;                /* gfx::Sheet 句柄（渲染取图用） */
    uint8_t  flash_ticks = 0;          /* 受击闪白剩余动画帧 */
    uint8_t  move_anim = 255;          /* 0..254 = 正在从 from→(x,y) 插值 */
    uint8_t  anim_seed = 0;            /* idle 帧相位抖动，避免全场同拍 */
    uint32_t alignment = 0;            /* 0=NEUTRAL 1=ENEMY 2=ALLY */
    uint32_t flags = 0;                /* property bitfield */
    Buff*    first_buff = nullptr;     /* intrusive list head */
    Level*   level = nullptr;
    uint32_t ready_at = 0;             /* 下一次可行动的 game_time */
    const char* name_key = nullptr;    /* 中文名（消息与飘字用） */

    virtual ~Actor() {}
    virtual int  act() = 0;            /* 返回耗时 tick，或 -1 表示等待输入 */
    virtual bool is_alive() const { return hp > 0; }
    /* 基础伤害结算：掉血 + 闪白 + 飘字 + 死亡判定（子类先改数再调基类） */
    virtual void damage(int dmg, const char* src);
    virtual void die();

    pos_t pos() const { return (pos_t)(x + y * DG_MAP_W); }
    void  set_pos(int _x, int _y) { x = _x; y = _y; }

    /* 渲染用：把 from→to 的进度换算成 0..255（255 = 已到位） */
    bool  moving() const { return move_anim < 255; }

    void add_buff(Buff::Type t, int duration, int value = 0);
    void remove_buff(Buff::Type t);
    Buff* get_buff(Buff::Type t) const;
    bool  has_buff(Buff::Type t) const { return get_buff(t) != nullptr; }
    int   buff_turns(Buff::Type t);     /* 无该 buff 返回 0 */
    void  act_buffs();                  /* 计时递减 + 周期效果结算 */
};

/* ===== Hero（玩家）===== */
class Hero : public Actor {
public:
    static constexpr int kMaxEnergy = 300;   /* 上游 hunger 口径：300 满 */

    int str = 10;                /* 力量 */
    int exp = 0;
    int lvl = 1;
    int gold = 0;
    int attack_skill = 50;
    int defense_skill = 4;
    int energy = kMaxEnergy;     /* 饥饿：走到 1/3 以下开始掉血 */
    int keys = 0;                /* 钥匙数量（上锁的门/宝箱消费） */
    bool has_amulet = false;     /* 通关信物 */
    dg_class_t cls = DG_CLASS_WARRIOR;

    Item* inventory[DG_MAX_INVENTORY] = {0};
    int   inv_count = 0;
    Item* equipped_weapon = nullptr;
    Item* equipped_armor  = nullptr;
    Item* equipped_ring   = nullptr;

    int  act() override;
    int  attack(Actor* enemy);
    int  defenseRoll(Mob* enemy);
    void damage(int dmg, const char* src) override;   /* 先过护甲减伤 */
    void die() override;
    bool pickup(Item* it);
    bool equip(Item* it);
    bool unequip(Item* it);
    bool use(int slot);
    void drop(int slot);
    void gainExp(int n);          /* 经验 + 升级结算（击杀 / 经验药水共用）*/

    int  maxExp() const { return 5 + lvl * 5; }
    /* 命中 / 闪避要把戒指加成算进去，所以不在头文件里内联（RG_* 枚举属于
     * item_def.h，dg_types.h 不能反向 include） */
    int  attackSkill() const;
    int  defenseSkill() const;
    int  armorDrMax() const;             /* 当前护甲最大减伤 */
    bool starving() const { return energy <= 0; }
    int  STR_RATION() const { return 150; }   /* 一次进食恢复量 */
};

/* ===== Mob（怪物：单一具体类 + 物种表驱动）=====
 * 上游是 Mob → Rat → Snake 的类层级；本移植把差异压进 MobSpec 常量表
 * （见 actor/mob_spec.h），AI 差异用 flags 位（远程 / 首领 / 潜行）分支，
 * 少 20 个 vtable 换 1 个 switch，flash 与调试都更划算。 */
class Mob : public Actor {
public:
    const MobSpec* spec = nullptr;
    int  attack_min = 1, attack_max = 1;
    int  defense = 0;
    int  xp_in_kill = 0;
    int  see_range = 8;
    uint8_t state = 0;             /* 见 State */
    int  home_x = 0, home_y = 0;   /* 游荡锚点（别把怪放风筝拉走）*/
    uint32_t last_hurt_time = 0;   /* 上次受伤的英雄回合（Goo 愈合判定，M3）*/
    uint32_t last_summon_time = 0; /* 上次召唤的英雄回合（召唤系冷却，M3）*/

    int act() override;
    int damageRoll();
    int attackSkill(Actor* target);
    int defenseSkill(Actor* target);
    void damage(int dmg, const char* src) override;   /* 分裂怪在此一分为二 */
    void die() override;                              /* 掉落 + 经验结算入口 */
    bool surprised_by(Actor* target);

    enum State : uint8_t { SLEEPING, WANDERING, HUNTING, FLEEING, PASSIVE };

protected:
    int rand_int(int bound);
};

/* ===== Item（数据驱动；定义表见 item/item_def.h）=====
 * kind 的取值顺序必须与 dungeon_api.h::dg_item_kind_t 严格一致。 */
class Item {
public:
    enum Kind : uint8_t {
        K_WEAPON = 0, K_ARMOR, K_POTION, K_SCROLL, K_RING, K_WAND,
        K_FOOD, K_KEY, K_GOLD, K_AMULET,
        KIND_COUNT
    };
    Kind        kind = K_GOLD;
    int16_t     sub = 0;           /* 同大类变体号（药水类型 / 法杖元素…） */
    int16_t     tier = 0;          /* 装备档位 1..5，非装备 0 */
    int16_t     qty = 1;
    int16_t     x = 0, y = 0;
    int16_t     icon = 0;          /* items.png 格子号 */
    int16_t     str_req = 0;       /* 力量需求 */
    uint8_t     cursed = 0;        /* 诅咒（读卷轴 / 使用才显现） */
    uint8_t     equipped = 0;      /* 0 背包 1 武器 2 护甲 3 戒指 */
    int8_t      enchant = 0;       /* 近战附魔（WeaponEnchant：0 无…），高档武器随机带上 */
    const char* name = nullptr;    /* 中文名（item_def 静态表） */

    bool is_equipment() const {
        return kind == K_WEAPON || kind == K_ARMOR || kind == K_RING;
    }
    void on_drop(Level* l);
};

/* ===== Level（单层地图）===== */
class Level {
public:
    static constexpr int LENGTH = DG_MAP_W * DG_MAP_H;

    Tile     tiles[LENGTH];
    int      depth = 0;            /* 0 基（HUD 显示 +1） */
    uint32_t seed = 0;
    Hero*    hero = nullptr;       /* back-pointer */
    Actor*   actors[64] = {0};     /* 本层所有 Actor（含 Hero），最多 64 */
    int      actor_count = 0;
    pos_t    entrance_pos = 0;
    pos_t    exit_pos = 0;
    pos_t    amulet_pos = 0;       /* 12F 基座（其余层 = 0 且无意义） */

    bool generate(uint32_t seed, int depth);   /* 程序化生成，返回成功 */
    void reset_view();                          /* 视野/占用指针清零 */

    Tile& at(int x, int y)             { return tiles[x + y * DG_MAP_W]; }
    const Tile& at(int x, int y) const { return tiles[x + y * DG_MAP_W]; }
    Tile& at(pos_t p)                  { return tiles[p]; }

    bool passable(int x, int y) const;   /* 英雄/怪可站 */
    bool solid(int x, int y) const;      /* 墙类：渲染缝合与遮挡判定 */
    bool water_at(int x, int y) const;
    bool flameable(int x, int y) const { return at(x, y).terr == DG_TERR_GRASS ||
                                                at(x, y).terr == DG_TERR_HIGH_GRASS; }
    int  distance(int ax, int ay, int bx, int by) const;
    int  chapter() const;                /* 0 下水道 1 监狱 2 洞穴 */

    void add_actor(Actor* a);
    void del_actor(Actor* a);
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
    bool step(int dx, int dy);         /* 方向键逐格移动（M1） */

    /* --- 场景流 --- */
    void goto_title();
    void goto_class_select();
    void pick_class(int cls);
    void menu_action(int action, int slot);
    int  selected_class = 0;

    Hero*        hero  = nullptr;
    Level*       level = nullptr;
    int          depth = 0;
    uint32_t     seed  = 0;
    uint32_t     game_time = 0;                     /* 全局回合计数 */
    uint32_t     anim_ms = 0;                       /* 动画时钟（每 tick 刷新） */
    dg_scene_t   scene = DG_SCENE_TITLE;
    JavaRandom*  rng   = nullptr;                   /* 世界 RNG，影响关卡生成 */
    JavaRandom*  ui_rng = nullptr;                  /* 不影响世界状态的杂项 RNG */

    /* UI 拉取用 */
    int  get_status_text(char* buf, int cap);
    const uint16_t* get_tilemap_fb(int* w, int* h);
    int  get_message(char* buf, int cap, int index);
    void get_hero_pos(int* x, int* y) { if (x) *x = hero->x; if (y) *y = hero->y; }
    void get_cam(int* x, int* y)      { if (x) *x = cam_x; if (y) *y = cam_y; }
    int  get_stats_text(char* buf, int cap);
    void get_hud(dg_hud_t* out);
    bool inv_get(int slot, dg_item_info_t* out);
    bool inv_use(int slot);
    bool inv_equip(int slot);
    void inv_drop(int slot);
    int  equip_mask();

    /* 调试注入用（串口 'v'）：一行导出英雄/相机/出口/物品/怪物 tile 坐标 */
    int  debug_dump(char* buf, int cap);
    int  hero_buffs(char* buf, int cap);   /* 英雄生效 buff 列表（取证） */
    void debug_m3_selftest();              /* M3 怪物 AI / 首领机制自检（取证，串口 'k'）*/
    void debug_m4_selftest();              /* M4 物品全谱自检（取证，串口 'p'）*/
    void debug_m5_selftest();              /* M5 关卡生成可达性/房间多样/宝箱怪自检（取证，串口 'y'）*/
    void debug_m6_fullrun();               /* M6 全程通关链路（1→12F→护身符→WIN，取证，串口 'u'）*/

    /* 鉴定系统：药水/卷轴/戒指/法杖按 (kind,sub) 全局鉴定一次，之后所有同类都显示真名。
     * 装备（武器/护甲）恒已知。bit 索引 = kind，位 = sub（各类型变体数 ≤8）。*/
    uint8_t ident_bits[6] = {0};
    bool is_identified(int kind, int sub) const {
        /* K_POTION=2 K_SCROLL=3 K_RING=4 K_WAND=5 才需要鉴定 */
        if (kind < 2 || kind > 5) return true;
        if (sub < 0 || sub >= 8) return true;
        return ((ident_bits[kind] >> sub) & 1) != 0;
    }
    void mark_identified(int kind, int sub) {
        if (kind >= 2 && kind <= 5 && sub >= 0 && sub < 8)
            ident_bits[kind] |= (uint8_t)(1u << sub);
    }
    void identify_all_carried();           /* SC_IDENTIFY：鉴定身上+背包全部 */

    /* --- 回合与移动 --- */
    int  cam_x = 0, cam_y = 0;                /* 主视窗左上 tile */
    int  cam_px = 0, cam_py = 0;              /* 渲染用像素相机（可亚 tile 平滑） */
    int  path_queue[128];                      /* 自动寻路分步队列 */
    int  path_len = 0, path_head = 0;
    uint32_t last_step_ms = 0;                 /* 自动行走分步节奏门控（ms） */

    void recalc_fov();
    bool hero_try_step(int gx, int gy);        /* 走/砍/拾取一步 */
    void end_turn();                           /* 视野 + 怪物回合 + 饥饿 */
    void advance_mobs();
    bool descend_stairs();                     /* 楼梯 → 下一层 */
    bool take_amulet();
    void spawn_level_content();
    Mob*  alloc_mob();
    void  free_mob(Mob* m);
    Item* alloc_item();
    void  free_item(Item* it);
    Buff* alloc_buff();
    void  free_buff(Buff* b);
    /* 掉落：按物种表 / 章节掉落表生成一件，落到 (x,y) */
    Item* drop_random_item(int x, int y, int quality_hint);
    Item* make_item(int kind, int sub);

    /* --- 视觉特效（引擎内渲染，UI 只搬运 fb）--- */
    static constexpr int kMaxFloats = 12;
    struct FloatText {
        int16_t  x, y;          /* tile 坐标 */
        uint32_t born_ms;
        uint16_t color;
        char     text[10];      /* "12" / "-7" / "L V!" / MISS */
        bool     used;
    };
    FloatText floats[kMaxFloats] = {};
    void add_float(int x, int y, const char* text, uint16_t color);

    static constexpr int kMaxBeams = 6;
    struct Beam {
        int16_t  x0, y0, x1, y1;
        uint32_t born_ms;
        uint16_t color;
        bool     used;
    };
    Beam beams[kMaxBeams] = {};
    void add_beam(int x0, int y0, int x1, int y1, uint16_t color);

    void flash_actor(Actor* a);               /* 受击闪白 1 段动画 */

    /* --- 音效队列（ring buffer，UI/音频层每帧 pop）--- */
    static constexpr int kMaxSfx = 16;
    uint8_t sfx_ring[kMaxSfx] = {};
    int     sfx_head = 0, sfx_count = 0;
    void sfx(int id);
    bool pop_sfx(int* id);

    /* 静态内存池容量 */
    static constexpr int kMaxMob    = 32;
    static constexpr int kMaxItem   = 64;
    static constexpr int kMaxBuff   = 32;

    /* 消息 log：环形定长缓冲，log() 内部拷贝。早期版本只存 char* ，调用方
     * 传栈上 snprintf 缓冲 → 读出来是乱码（真机才会爆），改成存体。 */
    static constexpr int kLogLines = 16;
    static constexpr int kLogLen   = 64;
    char log_lines[kLogLines][kLogLen];
    int  log_head = 0;
    void log(const char* text);

    /* 渲染 dirty flag */
    bool fb_dirty = true;
    bool anim_running = false;          /* 有动画在跑（渲染器每帧都要重画） */

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
    bool destroy(int slot);
    bool info(int slot, dg_save_info_t* out);
}

}  /* namespace dg */

#endif
