# sdgoods-dungeon · 次元地牢 · 移植设计方案

> 独立游戏固件 · 基于 Shattered Pixel Dungeon 素材 · 谷仓次元屏 ESP32-S3

- **仓库代号**：`sdgoods-dungeon`
- **中文对外名**：`次元地牢`
- **英文对外名**：`Dungeon`
- **副标题**：*Powered by Shattered Pixel Dungeon assets*
- **命名族**：与 `sdgoods-doom` / `sdgoods-arcade` 并列，同属"谷仓次元屏 · 独立游戏固件"矩阵
- **文档版本**：v0.1 · 2026-10

---

## 1. 项目定位

- **独立固件**，完全脱离 SDGOODS launcher 与 appdata 约定，只是硬件用同一块 ESP32-S3 徽章板
- 目标：把 **Shattered Pixel Dungeon** 的核心游戏体验搬到 360×360 圆屏 + 单点触摸 + 无按键的形态上
- 许可：**GPL-3.0 代码（独立开源发布）** + **CC-BY-SA 4.0 素材（保留原作者署名）**
- 首发定位：v0.1 只出第 1 章（下水道 + Slime King Boss），后续 3-4 次迭代扩到全 5 章

---

## 2. 硬件与内存预算

### 硬件规格（回顾）

| 组件 | 型号 | 关键约束 |
|---|---|---|
| MCU | ESP32-S3-R8 双核 @ 240MHz | 无 GPU |
| PSRAM | 8MB Octal | 主 framebuffer + 素材缓存 |
| Flash | 32MB | 分区自主设计 |
| LCD | ST77916 QSPI 360×360 圆屏 RGB565 | 内切方形 ~254×254 |
| Touch | CST816 **单点电容触摸** | 多点不可实现（硬件限制） |
| IMU | QMI8658 六轴 | 本作不用 |
| Audio | I2S 喇叭 + 数字麦克风 | 只用喇叭 |
| BLE | 5.0 | 本作不用（未来可加双人） |

### 内存映射

| 区域 | 用途 | 预算 |
|---|---|---|
| Internal SRAM (512KB) | 栈 + 快代码 + 当前帧地图 + AI 状态 | ~200KB 使用 |
| PSRAM (8MB) | Framebuffer (254×254×2 = 125KB) + 当前章节 tile atlas (2-4MB) + 音效缓存 (2MB) + BGM 解码缓冲 (500KB) + 字体 (1MB) | ~6MB 使用，2MB 富余 |
| Flash assets | 全量 Shattered 素材（裁剪后） | 12-15MB |

---

## 3. 分区表设计（自主定义，无 appdata 约束）

沿用 `sdgoods-doom` 的紧凑单体分区模式（无 otadata / 无 OTA 槽，factory 直启），扩 `assets` 数据分区。**以下为已落盘并编译验证的最终表**（platform/partitions.csv）：

```csv
# Name,      Type, SubType,  Offset,     Size,       Flags
nvs,         data, nvs,      0x9000,     0x6000,
phy_init,    data, phy,      0xF000,     0x1000,
factory,     app,  factory,  0x10000,    0x400000,
assets,      data, 0x40,     0x410000,   0xC00000,
save,        data, nvs,      0x1010000,  0x40000,
storage,     data, fat,      0x1050000,  0x80000,
```

坑位备忘（gen_esp32part.py 两条硬规则，本表已遵守）：
- 数据行**不允许行尾注释**（第六列会被当成 Flags 解析报错），注释全放整行区
- SubType 不认 `raw` 关键字，裸数据分区用自定义数字子类型 `0x40`；
  运行时按分区名 "assets" + SUBTYPE_ANY 查找，与子类型取值无关

**总用量约 21MB / 32MB flash**，剩余 ~11MB 留给未来扩展（OTA 双槽、多职业皮肤包、隐藏章节）。

### 素材访问策略（raw 分区，无文件系统）

- 编译期用 Python 工具（参考 `sdgoods-doom/tools/` 里 WAD 处理脚本）把 PNG/mp3/字体打成**一个 offset 表 + 二进制 blob**
- 运行时通过 `esp_partition_read()` 直接按 offset 读入 PSRAM 缓存
- **好处**：无 FAT 开销、无路径字符串、随机访问 O(1)、启动秒加载

---

## 4. 素材清单（从 Shattered PD 直接搬）

### 源仓库
https://github.com/00-Evan/shattered-pixel-dungeon → `core/src/main/assets/`

### 需要拷贝的文件（v0.1 版）

```
assets/
├── tiles/
│   ├── tiles.png                主图集
│   ├── tiles_entrance.png       第 1 章下水道
│   ├── tiles_caves.png          第 2 章（v0.2 加）
│   ├── tiles_prison.png         第 3 章（v0.2 加）
│   ├── tiles_city.png           第 4 章（v0.3 加）
│   └── tiles_hell.png           第 5 章（v0.3 加）
├── environment/                 地形装饰物 sprite
├── interface.png                UI 图集（按钮、背包框、状态条）
├── items.png                    物品图标（约 200 个，v0.1 只前 30 武器）
├── characters.png               怪物 + 角色 sprite
├── effects.png                  法术 / 粒子特效
├── statusbar.png                HP / XP 条
├── sounds/                      ~60 个短音效 mp3
├── music/                       8 首循环 BGM mp3（v0.1 只用 2 首）
├── fonts/
│   └── pixel font.png           原版位图字体（ASCII 部分）
└── messages/
    └── messages_zh.properties   ✅ 官方简体中文文本直接用
```

**压缩后总大小 ~12-15 MB**（PNG 保留无损、mp3 保留原码率、只保留 zh-hans 一种语言）。

### 中文字体（⚠ 需自补）

Shattered 原版 `pixel font.png` 只带 ASCII；中文文本用的字体需要另外子集化：
- 从 `messages_zh.properties` 抽出全部用到的汉字 → 去重
- 用现有工具链 `tools/gen_fonts.py` + `tools/font_metrics.py` 生成 LVGL 位图字体 bin
- 字体文件预计 500KB-1MB（约 3000 汉字 × 12×12 像素）
- 推荐源字体：Noto Sans CJK / Fusion Pixel Font（CC0 开源像素字体）

### 许可文件（必须原样保留）

- `LICENSE-CODE-GPL-3.0.txt` — 代码衍生部分
- `LICENSE-ASSETS-CC-BY-SA-4.0.txt` — 素材部分
- `ATTRIBUTION.txt` — 列出 Watabou / 00-Evan / Kenney.nl / 全部美术贡献者 / 中文翻译社区
- `NOTICE` — 说明本项目为 Shattered PD 素材衍生作品，非商业授权，非官方关联

---

## 5. Java → C++ 移植策略

### 不做机械翻译，做"照规范重写"

- 把 Java 类当作**行为规格说明书**：读逻辑、记公式、在 C++ 里用合适的数据结构重写
- Shattered 大量使用 Java 反射 + libGDX Scene2D UI，这两块**不可能 1:1 翻译**，需要按 LVGL 思维重新设计
- 每个 Java 类对应一个 C++ 模块：文件命名保留原类名（`Rat.java` → `mob/rat.cpp`）方便对照

### 关键 PRNG 必须复刻

Shattered 手感依赖 Java `Random`（LCG 线性同余，乘数 `0x5DEECE66D`）。用 C 标准 `rand()` 会漂手感：

```cpp
// 复刻 java.util.Random
class JavaRandom {
  uint64_t seed;
  int next(int bits) {
    seed = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1);
    return (int)(seed >> (48 - bits));
  }
public:
  float nextFloat() { return (next(24) & 0xFFFFFF) / (float)(1 << 24); }
  int nextInt(int n) { /* 与 Java 一致的实现 */ }
};
```

### 存档格式

- 用 Shattered 的 `Bundle` 概念（JSON 变体），版本号字段前置
- 每次版本迁移写 upgrade function，兼容旧 v0.1 存档
- 存到独立的 `save` NVS 分区，不与 launcher 系统 NVS 混用

---

## 6. 触屏交互设计（单点，与 SPD 手机版几乎一致）

| 用户操作 | 屏幕响应 | 触发条件 |
|---|---|---|
| 点击相邻 8 方向格 | 玩家移动一步；若格上有怪则攻击 | 距离 = 1 格 |
| 点击远处可见格 | A* 自动寻路（每帧最多 200 节点，跨帧完成） | 距离 > 1 且已探索 |
| 点击物品格 | 拾取 | 玩家相邻 |
| 长按任意格 | 检视（弹出信息卡片，不消耗回合） | 按住 ≥ 400ms |
| 双击玩家 | 原地等待一回合 | — |
| 底部圆键"背包" | 全屏背包 UI（分页 5×5 网格） | — |
| 底部圆键"施法" | 快捷法术槽（法师）/ 远程射击模式（猎人） | — |
| 底部圆键"搜索" | 消耗回合搜索隐藏门 / 陷阱 | — |
| 底部圆键"菜单" | 存档 / 设置 / 退出 | — |

**关键约束**：所有交互都是**一次一个动作**，无同时按下需求 → 完美匹配单点触控。

### 顶部手势带规避（沿用 sdgoods-doom 经验）

外壳顶部下滑捕获带会吃掉上排虚拟键 → **状态栏放在 top 30-40px 只做只读显示，不放任何可点击元素**；所有可点击按钮都在屏幕中下部圆屏安全区。

---

## 7. 圆屏 UI 布局（254×254 内切方形）

```
┌───────────────── 360px ─────────────────┐
│  [状态栏 30px,只读,避开 CC 手势带]         │
│   HP ██████▒▒▒  深度: 2F  💰 142  LV 3   │
│                                          │
│      ┌──── 主视窗 200×200 ────┐           │
│      │ 12×12 tile 视野         │           │
│      │ · @ 玩家 sprite          │           │
│      │ · 怪物 / NPC / 掉落      │           │
│      │ · 视野雾 + 记忆半暗层    │           │
│      │ · 右侧竖排 buff 图标     │           │
│      └──────────────────────────┘          │
│                                          │
│  [底部 6 颗等大圆键,DOOM 白悬浮方案]        │
│   ( 背包 )( 施法 )( 装备 )( 搜索 )( 等待 )( 菜单 ) │
└──────────────────────────────────────────┘
```

- **tile 尺寸**：原版 8×8 → 屏上 16×16（整数 2× 缩放，像素风保持锐利）
- **视野**：12×12 格（Shattered 原版 ~13×13，无损）
- **底部圆键**：完全复用 `sdgoods-doom` 的 6 颗等大分离式白悬浮方案 + 15ms 触控轮询

---

## 8. MVP 内容范围（v0.1 · 第 1 章版）

### 保留（Shattered 完整系统）

- ✅ 战士职业（不做子职业）
- ✅ 下水道 5 层程序化生成
- ✅ Slime King Boss（第 4 层）+ 第 5 层传送门
- ✅ 30 武器 + 15 护甲 + 15 药水 + 10 卷轴 + 10 戒指 + 5 口粮 + 金币/钥匙
- ✅ 15-20 种怪物（第 1 章原生怪组）
- ✅ Buff / Status 效果（中毒、燃烧、睡眠、减速等基础 10 种）
- ✅ 附魔基础（+d / +STR）
- ✅ 秘密门 / 陷阱 / 隐藏宝藏（每层 1-2 处）
- ✅ 存档 3 槽位 + 死亡统计（层数、击杀数、金币）

### 砍掉（v0.2+ 或永久）

- ❌ 其他 3 职业（v0.2 加法师、盗贼、猎人）
- ❌ 12 个子职业（v0.3 加）
- ❌ 神器 Artifact 系统（v0.4+）
- ❌ 铭刻 Glyphs（v0.4+）
- ❌ 挑战模式 12 种（v0.4+）
- ❌ Daily Run（永久不做，无服务器）
- ❌ 成就 100+（永久不做，无 UI 展示位）
- ❌ 炼金 / 酿造系统（v0.4+）
- ❌ 任务 NPC / 商店（v0.2 加商店）
- ❌ 猫 / 宠物系统（v0.3+）

### 后续版本路线图

| 版本 | 新增内容 | 预计时间 |
|---|---|---|
| **v0.1** | 战士 + 第 1 章（下水道） + Slime King | 5 周 |
| **v0.2** | + 法师/盗贼/猎人 + 第 2 章（洞穴） + Guardian | +4 周 |
| **v0.3** | + 第 3 章（监狱） + Tyrant + 子职业基础版 | +4 周 |
| **v0.4** | + 第 4 章（矮人城） + 第 5 章（地狱） + Tengu / Yogs | +4 周 |
| **v1.0** | 完整 26 层 + 全部装备 + 挑战模式 + 炼金 | 累计 ~17 周 |

---

## 9. 关键技术风险与提前验证项

| 风险 | 严重度 | 验证方法 |
|---|---|---|
| 中文字体子集能否覆盖全部 `messages_zh.properties` 汉字 | 🟢 低 | 半天，脚本抽取 + `gen_fonts.py` 试生成 |
| Java LCG PRNG 复刻手感一致性 | 🟢 低 | 1 天，写单元测试对比 1000 次随机序列 |
| A* 寻路 200×200 地图性能 | 🟡 中 | 2 天，ESP32-S3 上跑压力测试，考虑分帧 |
| PNG atlas 解码性能（首帧卡顿） | 🟡 中 | 编译期转 LVGL `.bin` 位图，运行时 `esp_partition_read` 直接 memcpy |
| PSRAM 带宽 vs 音效流播放 + 帧渲染竞争 | 🟡 中 | 3-5 天，实测；必要时音效预解码到内部 SRAM 缓存 |
| LVGL 8.3 在 360×360 全屏刷新帧率 | 🟢 低（sdgoods-doom 已验证） | 复用 DOOM 的 partial refresh 方案 |
| ESP32-S3 上 libGDX 依赖的 shader / blend mode 无对应 | 🟢 低 | LVGL 手动实现混合，仅"水面反射 / 阴影层"等少数效果需要变通 |

---

## 10. 工程结构（沿用 sdgoods-doom 模式）

```
sdgoods-dungeon/
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── main.c
│   ├── game/
│   │   ├── dungeon.cpp           主循环 + 状态机
│   │   ├── level.cpp             程序化关卡生成
│   │   ├── actor/                HP / buff / status
│   │   ├── mob/                  怪物 AI（每个 .cpp 对应 Shattered 一个 Java 类）
│   │   ├── item/                 物品
│   │   ├── hero/                 玩家
│   │   ├── gfx/                  LVGL 渲染层
│   │   ├── audio/                I2S 播放
│   │   ├── pathfinder/           A*
│   │   └── rng/                  Java Random 复刻
│   └── ui/
│       ├── game_screen.c         主视窗
│       ├── inventory.c           背包
│       ├── statusbar.c           顶部状态栏
│       ├── hotbar.c              底部 6 悬浮键（复用 DOOM 代码）
│       └── dialogs.c             检视卡 / 消息 log
├── components/
│   └── lvgl/                     (managed_components)
├── partitions.csv                见 §3
├── assets_src/                   原始 PNG/mp3，编译前
├── tools/
│   ├── pack_assets.py            打 blob + offset 表
│   ├── gen_fonts.py              从中文字库抽子集（复用 FACEENGINE 现成）
│   └── flash_local.sh            一键烧录（复用 DOOM 脚本）
├── LICENSE-CODE-GPL-3.0.txt
├── LICENSE-ASSETS-CC-BY-SA-4.0.txt
├── ATTRIBUTION.txt
├── NOTICE
├── README.md                     上手向 + 实机图（沿用 DOOM README 规范）
├── DESIGN.md                     (本文档)
├── sdkconfig
├── sdkconfig.defaults
└── version.txt
```

---

## 11. 上架与发布策略

- **发布形态**：GitHub Release（源码 + 编译产物 `full.bin`）+ 谷仓平台独立游戏固件通道
- **对外文案红线**：
  - ✅ "基于 Shattered Pixel Dungeon 素材的独立衍生作品"
  - ✅ "保留原作者署名,同 CC-BY-SA 4.0 分发"
  - ❌ 不要说"官方移植"（Evan 未授权此说辞）
  - ❌ 不要在谷仓商店暗示"官方合作"
- **发布包结构**：
  ```
  sdgoods-dungeon-v0.1.zip
  ├── full.bin                     整机镜像（合并 bootloader + partition + app + assets + save）
  ├── firmware.bin                 仅固件（OTA 用）
  ├── assets.bin                   仅素材（首次烧录或素材更新用）
  ├── flash_local.sh               一键烧录脚本
  ├── README.md                    上手指南 + 实机截图
  ├── ATTRIBUTION.txt
  └── LICENSE-*
  ```
- **CI**：GitHub Actions 自动构建 ESP-IDF v5.1+，产出可下载 artifact（沿用 sdgoods-doom 的 `.github/workflows/build.yml` 模式）

---

## 12. 下一步（本周可执行）

按优先级 3 项：

1. **半天预验证 3 个技术假设**
   - 从 Shattered 仓库拉一份 `messages_zh.properties` 跑字体子集生成
   - 写 100 行 Java `Random` 复刻 + 单元测试对比原版
   - 在 ESP32-S3 开发板上跑一次 200×200 地图 A* 寻路性能 benchmark

2. **搭骨架能编译能刷**
   - clone 空仓库到 `d:\2.Project\SDGOODS\sdgoods-dungeon\`
   - 拷 `sdgoods-doom` 的 `CMakeLists.txt` / `partitions.csv` / `main.c` / `.github/workflows/build.yml` 作起点
   - 改分区表按 §3 布局
   - 刷进板子跑出空 LVGL screen

3. **写第一版 README**（沿用 sdgoods-doom 上手向规范：简单有趣 + 实机图 + 许可表 + 叠甲声明）

---

**文档结束**。等 clone 完把这份 DESIGN.md 搬进仓库根目录即可开工。
