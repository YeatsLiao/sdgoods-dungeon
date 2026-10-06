/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * item.cpp —— 物品落格 + 使用效果结算（药水 / 卷轴 / 食物 / 法杖）
 *
 * v0.4 起效果全在这里集中结算：Item 是数据，效果是 switch，
 * 新增一种药水只要改 item_def 表 + 本文件一个 case。
 */
#include "dg_types.h"
#include "item/item_def.h"
#include "actor/mob_spec.h"
#include "rng/java_random.h"
#include "esp_log.h"
#include <cstdio>

static const char *TAG = "dg.item";

namespace dg {

void Item::on_drop(Level* l)
{
    if (l && x >= 0 && y >= 0 && x < DG_MAP_W && y < DG_MAP_H) {
        Tile& t = l->at(x, y);
        /* 同格已有掉落：金币合并，其他物品挤到邻近空格（上游是堆叠，
         * 我们只堆金币，装备保持一格一件，免得 UI 网格要处理链表） */
        if (t.item && t.item->kind == K_GOLD && kind == K_GOLD) {
            t.item->qty += qty;
            Game::instance().free_item(this);
            return;
        }
        if (!t.item) { t.item = this; return; }
        static const int dx8[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
        static const int dy8[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
        for (int k = 0; k < 8; k++) {
            int nx = x + dx8[k], ny = y + dy8[k];
            if (!l->passable(nx, ny) || l->at(nx, ny).item) continue;
            x = nx; y = ny;
            l->at(x, y).item = this;
            return;
        }
        /* 八邻全满：回收到池，不掉出来（宁可不掉也别把地图搞乱） */
        Game::instance().free_item(this);
    }
}

/* 最近的、当前可见的怪（法杖自动索敌 —— 圆屏没有精确瞄准的手感空间） */
static Mob* nearest_visible_mob(Hero* h)
{
    Level* lv = h->level;
    if (!lv) return nullptr;
    Mob* best = nullptr;
    int best_d = 999;
    for (int i = 1; i < lv->actor_count; i++) {
        Actor* a = lv->actors[i];
        if (!a || !a->is_alive()) continue;
        if (!lv->at(a->x, a->y).vis_current) continue;
        int d = lv->distance(a->x, a->y, h->x, h->y);
        if (d < best_d) { best_d = d; best = (Mob*)a; }
    }
    return best;
}

static int potion_use(Hero* h, Item* it)
{
    Game& g = Game::instance();
    int amt;
    switch (it->sub) {
    case POT_HEAL:
        if (h->hp >= h->hp_max) { g.log("你已经满血了。"); return 0; }
        amt = h->hp_max / 2 + 5;
        h->hp += amt;
        if (h->hp > h->hp_max) h->hp = h->hp_max;
        g.add_float(h->x, h->y, "+", 0x4E2C);           /* 亮绿 */
        g.log("伤口带来的灼烧感消退了。");
        break;
    case POT_HASTE:
        h->add_buff(Buff::HASTE, 20);
        g.log("你感到浑身轻快，脚步停不下来。");
        break;
    case POT_INVIS:
        h->add_buff(Buff::INVISIBILITY, 20);
        g.log("你的身影淡入了空气里。");
        break;
    case POT_LEVITATE:
        h->add_buff(Buff::LEVITATION, 30);
        g.log("双脚离开了地面。");
        break;
    case POT_STRENGTH:
        h->str += 2;
        g.add_float(h->x, h->y, "S+2", 0xF800);
        g.log("一股热流涌进肌肉。力量提升了！");
        break;
    case POT_PURIFY:
        h->remove_buff(Buff::POISON);
        h->remove_buff(Buff::BURNING);
        h->remove_buff(Buff::SLEEP);
        h->remove_buff(Buff::PARALYSIS);
        h->remove_buff(Buff::FRIGHT);
        h->remove_buff(Buff::ROOTS);
        h->remove_buff(Buff::SLOW);
        g.log("体内的杂质被冲刷干净。");
        break;
    default:
        g.log("这瓶药水闻所未闻，你犹豫着没喝。");
        return 0;
    }
    g.sfx(DG_SFX_DRINK);
    return 1;
}

static int scroll_use(Hero* h, Item* it)
{
    Game& g = Game::instance();
    Level* lv = h->level;
    switch (it->sub) {
    case SC_MAP:
        for (int i = 0; i < Level::LENGTH; i++) lv->tiles[i].explored = 1;
        g.log("地图在你脑海中铺开 —— 整层都看清了。");
        break;
    case SC_UPGRADE: {
        Item* tgt = h->equipped_weapon ? h->equipped_weapon : h->equipped_armor;
        if (!tgt) { g.log("没有装备可以强化。"); return 0; }
        if (tgt->tier >= 5) { g.log("它已经到极限了。"); return 0; }
        tgt->tier++;
        tgt->name = item_name(tgt->kind, tgt->sub, tgt->tier);
        tgt->icon = (int16_t)item_icon(tgt->kind, tgt->sub, tgt->tier);
        tgt->str_req = (int16_t)(tgt->kind == Item::K_WEAPON
                                 ? weapon_str_req(tgt->tier)
                                 : armor_str_req(tgt->tier));
        g.add_float(h->x, h->y, "+1", 0x07E0);
        g.log("符文没入装备，它变得更锋利了。");
        break;
    }
    case SC_REMOVE_CURSE: {
        int n = 0;
        if (h->equipped_weapon && h->equipped_weapon->cursed) { h->equipped_weapon->cursed = 0; n++; }
        if (h->equipped_armor  && h->equipped_armor->cursed)  { h->equipped_armor->cursed  = 0; n++; }
        if (h->equipped_ring   && h->equipped_ring->cursed)   { h->equipped_ring->cursed   = 0; n++; }
        for (int i = 0; i < h->inv_count; i++)
            if (h->inventory[i] && h->inventory[i]->cursed) { h->inventory[i]->cursed = 0; n++; }
        g.log(n ? "一道黑气从装备上散去。" : "你感到一阵无事发生。");
        if (!n) return 0;
        break;
    }
    case SC_TELEPORT: {
        int tries = 0, nx = 0, ny = 0;
        while (tries++ < 200) {
            nx = g.ui_rng->nextInt(DG_MAP_W);
            ny = g.ui_rng->nextInt(DG_MAP_H);
            if (lv->passable(nx, ny) && !lv->at(nx, ny).actor) break;
        }
        if (tries >= 200) { g.log("空间没有回应你。"); return 0; }
        lv->at(h->x, h->y).actor = nullptr;
        h->from_x = h->x; h->from_y = h->y;
        h->set_pos(nx, ny);
        h->move_anim = 255;                    /* 传送不是走位，不做插值 */
        lv->at(nx, ny).actor = h;
        g.log("世界在你脚下翻转。");
        break;
    }
    default:
        return 0;
    }
    g.sfx(DG_SFX_SCROLL);
    g.recalc_fov();
    return 1;
}

static int wand_use(Hero* h, Item* it)
{
    Game& g = Game::instance();
    Mob* m = nearest_visible_mob(h);
    if (!m) { g.log("视野里没有可以施法的目标。"); return 0; }
    int dmg = 0;
    if (it->sub == WD_BOLT) {
        dmg = 5 + h->lvl;
        m->damage(dmg, "bolt");
        g.add_float(m->x, m->y, "!", 0x0710);          /* 青蓝法术色 */
        g.sfx(DG_SFX_ZAP);
    } else {
        m->add_buff(Buff::SLOW, 12);
        g.sfx(DG_SFX_ZAP);
    }
    g.add_beam(h->x, h->y, m->x, m->y, 0x0710);
    it->qty--;
    g.log("法杖前端迸出一道光。");
    if (it->qty <= 0) { g.log("法杖碎裂成粉。"); return 1; }
    return 0;        /* 不消耗，但本回合已用掉 */
}

static int food_use(Hero* h, Item* it)
{
    Game& g = Game::instance();
    if (h->energy >= Hero::kMaxEnergy) { g.log("你实在吃不下了。"); return 0; }
    int amt = (it->sub == FD_PASTY) ? 200 : 100;
    h->energy += amt;
    if (h->energy > Hero::kMaxEnergy) h->energy = Hero::kMaxEnergy;
    g.add_float(h->x, h->y, "+F", 0xFD20);
    g.log("进食让你恢复了力气。");
    return 1;
}

int item_use(Hero* h, Item* it)
{
    if (!h || !it) return 0;
    Game& g = Game::instance();
    int consumed = 0;
    switch (it->kind) {
    case Item::K_POTION: consumed = potion_use(h, it); break;
    case Item::K_SCROLL: consumed = scroll_use(h, it); break;
    case Item::K_WAND:   consumed = wand_use(h, it);   break;
    case Item::K_FOOD:   consumed = food_use(h, it);   break;
    case Item::K_RING:
        g.log("戒指要靠装备才生效。");
        g.sfx(DG_SFX_ERROR);
        return 0;
    case Item::K_AMULET:
        g.log("你握着护身符 —— 先回到地面！");
        return 0;
    case Item::K_KEY:
        g.log("钥匙是开门用的，不是现在。");
        g.sfx(DG_SFX_ERROR);
        return 0;
    case Item::K_GOLD:
        return 0;
    default:
        ESP_LOGD(TAG, "no use for kind %d", (int)it->kind);
        return 0;
    }
    if (consumed) g.fb_dirty = true;
    return consumed;
}

}  /* namespace dg */
