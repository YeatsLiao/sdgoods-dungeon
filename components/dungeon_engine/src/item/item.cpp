/*
 * sdgoods-dungeon · 次元地牢
 * Copyright (c) 2026 Yeats Liao
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * item.cpp —— 物品基类（骨架）
 */
#include "dg_types.h"

namespace dg {

void Item::on_drop(Level* l) {
    if (l && x >= 0 && y >= 0 && x < DG_MAP_W && y < DG_MAP_H) {
        Tile& t = l->at(x, y);
        t.item = this;
    }
}

}  /* namespace dg */
