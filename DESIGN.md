# sdgoods-dungeon · 次元地牢 · 移植设计方案

> 独立游戏固件 · 基于 Shattered Pixel Dungeon 素材 · 谷仓次元屏 ESP32-S3

- **仓库代号**：`sdgoods-dungeon`
- **中文对外名**：`次元地牢`
- **英文对外名**：`Dungeon`
- **副标题**：*Powered by Shattered Pixel Dungeon assets*
- **命名族**：与 `sdgoods-doom` / `sdgoods-arcade` 并列，同属"谷仓次元屏 · 独立游戏固件"矩阵
- **文档版本**：v0.4 · 2026-10（上游对齐：真素材 + 全流程场景 + 程序化音频）

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
| LCD | ST77916 QSPI 360×360 圆屏 RGB565 | 满圆画布 352×352 |
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
| **左下十字键**（上/下/左/右） | 向该方向迈一步；格上有怪则攻击；**按住 240ms/步连走，松手即停** | 已实现 |
| 点击相邻 8 方向格 | 玩家移动一步；若格上有怪则攻击（斜向靠这条，与十字键互补） | 距离 = 1 格 |
| 点击远处可见格 | A* 自动寻路（每帧最多 200 节点，跨帧完成） | 距离 > 1 且已探索 |
| 点击物品格 | 拾取 | 玩家相邻 |
| 长按任意格（未实现） | 检视（弹出信息卡片，不消耗回合） | 按住 ≥ 400ms |
| 双击玩家（未实现） | 原地等待一回合 | — |
| 圆键"背包" | 全屏背包 UI（v0.3 先做 stats 面板，5×5 网格 v0.4） | — |
| 圆键"施法"（未接线） | 快捷法术槽（法师）/ 远程射击模式（猎人） | — |
| 圆键"搜索" | 消耗回合搜索隐藏门 / 陷阱 | — |
| 圆键"菜单"（未实现） | 存档 / 设置 / 退出 | — |

**关键约束**：所有交互都是**一次一个动作**，无同时按下需求 → 完美匹配单点触控。

**为什么点击移动不够**：点击是效率最高的走法，但 affordance 为零——真机用户第一
句话就是「不知道怎么移动的」。回合制策略游戏在手掌大的圆屏上，必须同时给一个
肉眼可见的方向键（见 §7），并在开机弹一页「怎么玩」。

### 顶部手势带规避（沿用 sdgoods-doom 经验）

外壳顶部下滑捕获带会吃掉上排虚拟键 → **状态栏放在 top 30-40px 只做只读显示，不放任何可点击元素**；所有可点击按钮都在屏幕中下部圆屏安全区。

---

## 7. 圆屏 UI 布局（176 逻辑 ×2 放大 = 352 满圆画布）

可见区是直径 360 的圆，四角物理不存在。布局按「地图铺满可见圆 + 控件半透明悬浮」
组织（z 序自下而上：画布 → HUD 胶囊 → 十字键/功能键 → 引导页 → overlay）：

```
              360px 圆屏（可见区 = 半径 180 的圆）
        +------------ 主视窗 352x352 @ (4,4) -------------+
        |         ( 生命 20/20 · 1层 · 金币 0 )            |  HUD 胶囊 y=22
        |         ( 欢迎来到次元地牢！...             )     |  消息胶囊 y=48
        |                                                  |
        |   11x11 tile 地图，tile 逻辑 16px -> 显示 32px    |  FOV 半径 6
        |                                                  |
        |    [上]                          (包)             |
        |  [左][右]                     (搜)                |  十字键 4 臂 44x44
        |    [下]                       (待)                |  功能键 d=48 圆弧
        +--------------------------------------------------+
        四角出圆的像素被物理边框吃掉 -> 不浪费可见面积
```

- **两级尺寸口径**：引擎只渲染**逻辑 176x176**（11x11 tile @16px），UI 侧整数
  2x 最近邻放大到 **物理 352x352**。352 是 360 直径上能铺满赤道弦的最大 16 整除
  正方形（半侧 176 < 半径 180），四角出圆部分被物理边框吃掉。
  缩放放在 UI 而非引擎：引擎的 tile 几何 / 素材索引 / 点击换算全部保持 16px 基准。
- **为什么必须 2x**：1.85 寸圆屏上 16px 格子物理尺寸约 1.4mm，玩家看不清地形与
  怪物（实测反馈「画面好小」）。放大后 tile 32px，可见地图面积不变而可读性质变；
  视口 tile 数从 22x22 降到 11x11，与 Shattered 手机版视距同量级，不算视野损失。
- **视野**：FOV 半径 6 tile = 96 逻辑 px = 192 物理 px > 可见圆半径 180，圆内全亮。
- **HUD 只能叠画**：圆屏没有专用文字带，状态/消息以半透黑胶囊浮在画布上；宽度按
  所在高度的圆弦收窄（y=24 弦宽约 195px / y=48 弦宽约 245px），否则字会转到圆外。
  消息胶囊 max_height 64（3 行），底边 y=112 角点 hypot(108,68)=128 < 180 仍在圆内。
- **左下十字方向键**：4 个 44x44 臂形按钮拼成十字（圆屏复古 UI 既有决策：视觉拼接
  优于一整块，命中区独立可控），半透明白 + 白描边。中心 (104,252)，最远角点
  下臂左下 (82,318) -> hypot(98,138)=169 < 180 完整在圆内。交互沿用本工程方向键
  规范：按下立即迈一步，之后 240ms/步连走，松手或滑出即停。
  存在的理由：点击移动虽然更快，但**零 affordance**——真机反馈「不知道怎么移动的」，
  必须有肉眼可见的移动控件；斜向仍走点击（4 向键 + 点击互补）。
- **右下功能键**：3 颗（待/搜/包）d=48 半透明白圆键，沿半径 128 圆弧按
  (240,293) / (282,257) / (305,207) 排开，最外角点 hypot(85,137)=161 < 180，
  与十字键矩形不重叠。v0.2 的 6 键等大热栏已废除——施法/装备/菜单在 v0.4
  有真实内容后再回来，不留空键。
- **开机引导页**：268x224 居中黑底（角点 hypot(134,112)=174 < 180），写明
  「点地图 / 十字键 / 待 / 搜 / 包 / 目标=找楼梯下楼」，点屏消失且只弹一次。
  圆屏设备没有说明书，操作方式必须产品自己讲。
- **顶部 y<50 是控制中心下滑捕获带**：只放只读文字，不放任何可点元素

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

## 13. v0.4 上游对齐落地（本版本实际实现）

v0.4 的验收口径来自一条产品指令：**素材、按钮、交互全部对齐上游
 Shattered Pixel Dungeon，做成一个真正的游戏**。以下是落地的结构决策（代码
 已写完，统一构建与真机取证见工程流程 a11）。

### 13.1 场景流（引擎持有，UI 只切显隐）

```
TITLE ──新冒险──▶ CLASS_SELECT ──选战/法/贼/猎──▶ IN_GAME
  ▲  ──继续(读槽0)──────────────────────────────┤
  │                                             ├─▶ INVENTORY（背包网格）
  │  死亡 ◀── IN_GAME hp=0 ──▶ GAME_OVER ──再来/回标题
  │  通关 ◀── 12F 取护身符 ──▶ WIN
  └──回标题◀── MENU（保存/读取/回标题）
```

- `Game::init()` 把 `scene` 置 `DG_SCENE_TITLE`；UI 不再开机即 `new_game`。
- `dg_api_current_scene()` 驱动 `show_scene()`，每场景一层 LVGL 容器只切 HIDDEN。
- 调试键（同一 COM6 会话，`sdgoods_console_ext_cmd`）：`h`=回标题 `g`=选职业
  `1..4`=选职业并开局，让 `tools/play_test.py flow` 逐页取证确定化。

### 13.2 素材对齐上游（零解码依赖）

- `tools/pack_assets.py` 把上游 PNG 预烘为 RGB5 容器 → SDGA `assets.bin` → raw 分区；
  图集常驻 PSRAM，透明约定 alpha<128 → 色键 `KEY_RGB565`。
- `dg_icons.h` 镜像上游 `DungeonTileSheet`/`ItemSpriteSheet`/`Icons` 索引常量；
  图集格号 idx → 像素 `(idx%16*16, idx/16*16)`。
- UI 所有按钮图标 / 背包物品 / 职业头像经 `dg_api_blit_icon`（最近邻缩放 + 色键
  叠底）取真格，不再自绘色块与中文圆键。
- 地形渲染三段缝合（FLAT / RAISED / OVERHANG）+ 水 4 位掩码 + 门/楼梯/基座/草/
  陷阱 known 态，全部走 `render.cpp` 真 tileset。

### 13.3 数据驱动模型（省 vtable / 省 RAM）

- `Item` 纯数据（kind×sub×tier 查 `item_def` 表）；`Mob` 单具体类持 `const MobSpec*`
  （flags 位分支）；`Buff` 降为 struct + 静态池；`Mob/Item/Buff` 均走 placement-new
  于静态池，无每局堆分配。
- 章节生成 `level.cpp`：房间+并集补连通走廊+环路+对穿墙改门+监狱上锁门+秘密门
  +水塘+高草+隐藏陷阱+按章宝箱 variant+装饰柱+12F 基座护身符。

### 13.4 确定性存档（~520B blob）

- `level::generate` 内 `seed'=mix(base_seed, depth)`，同 seed+同 depth 重放同图。
- 存档只存 seed+depth+英雄数值+背包数组（含 equipped 角标）；读档 `new_game` 后
  逐层 generate/spawn 重放到深度，再覆盖英雄与背包坐标。

### 13.5 程序化音频（16kHz 立体声，无解码器）

- 引擎只排 `DG_SFX_*`（21 个）进环形队列，UI 每帧 `dg_api_pop_sfx → dg_audio_play_sfx`。
- `main/game/dg_audio.c`：FreeRTOS mix_task 持续推 I2S（`stream_write` 被 DMA 自然节流），
  运行时合成 sine/square/saw/tri + AHDSR-lite 包络 + 地牢低频 drone，不读任何音频文件。

### 13.6 HUD 与交互（触屏为主，圆屏适配）

- 结构化 HUD：`dg_api_get_hud` 驱动血/经验/能量 lv_bar + 深度/等级/金币 label +
  消息行；站在楼梯时 DESCEND 键描边变亮。
- 主交互 = 点地图即走/砍/捡（A* 由引擎做）；底部沿半径 116 圆弧 6 键（包/待/搜/
  装/下/菜），键心到屏心 ≈116 + 半对角 26 → 142 < 180，全部完整落圆内。
- 逻辑 176×176（11×11 tile @16px）→ UI 2× 最近邻放大到 352 满圆，像素风无损。

---

## 14. 圆屏可玩性对齐里程碑（M1–M6 · 本仓实际落地）

产品目标：把 Shattered PD 移植到 360×360 圆形手表，**玩法结论对齐**上游
（数值 / 机制 / 手感），而非逐行复刻 Java；同时做真正的圆屏适配并最终可玩
（一路打到 12F 取护身符通关）。验收一律走「引擎内确定性自检 + 真机取证」。

### 14.1 各里程碑结论

- **M1 圆屏操控与布局**：主操控改为屏幕 D-pad 方向键逐格移动（tap-to-move 降为
  辅助）；地图不再被底部按键遮挡；操作反馈走飘字 toast。→ 达到「能玩」底线。
- **M2 速度 / 时间调度 + Buff 真实生效**：Actor 以 1/16 定点 `act_accum` 跨回合累加
  有效速度（蟹/蝠 speed=32 → 每英雄回合动 2 次；0.5× 能正确累积）；英雄 buff 计时
  与周期结算补上（毒 / 烧掉血、加速 / 隐身 / 悬浮到期）。
- **M3 怪物专属 AI + 首领机制**：按物种 `MobSpec.flags` 分化行为；4F/8F/12F 各一只
  首领（`mob_boss_for_depth`），击杀掉落。自检串口 `k`。
- **M4 物品全谱对齐**：戒指 / 法杖 / 卷轴 / 药水 / 附魔 / 鉴定落地；近战附魔 proc
  （雷电加伤、吸血回血、烈焰上燃烧、寒冰上减速）。修复 `Hero::equip` 换装守卫恒真
  拒绝的真机 bug（旧装备槽有人即拒 → 战士永远换不了装）。自检串口 `p`（12/12）。
- **M5 关卡生成与房间多样**：step 5b 特殊房间主题（花园 / 雕像室 / 储藏 vault /
  陷阱房 / 水池，每层 1~3 个，深层更多）；宝箱怪 Mimic（开箱瞬间长出腿变 ENEMY）；
  可达性硬约束 —— 固态装饰只放开敞内场，generate 末尾洪水填充兜底（上锁门降级 →
  秘密门显形 → 清固态装饰），杜绝稀 seed 软锁。自检串口 `y`（12/12 + 跨 seed 96 全达）。
- **M6 平衡与全程通关验证**：`debug_m6_fullrun`（串口 `u`）用超配英雄沿真实
  `hero_try_step → descend_stairs → 12F 护身符 → WIN` 走满 8 个 seed 全通关；顺带
  修复真机也会踩的「12F 护身符被 `spawn_level_content` 误回收」软锁（见 14.3）。

### 14.2 已知差异（结论对齐 ≠ 逐行复刻，诚实标注）

- **地图生成**：取上游 Section/Room 的「形状结论」（房间矩形互不重叠 + 走廊 union
  补连通 + 墙整形），不搬 DungeonRoom 类继承树。房间主题为等效实现。
- **暂缓系统**（性价比过低于 ESP32 圆屏，未静默省略）：炼金 / 商店 / 神器 / 任务 /
  宠物 / 挑战模式 / 多职业专精。护身符通关为唯一胜利条件。
- **超配自检 ≠ 平衡实验**：`debug_m6_fullrun` 抹平战斗随机性以验证链路可达；平衡靠
  逐层 HP/EXP/GOLD 曲线 + 真机 `descend`（未超配英雄 4F 首领层阵亡 → 重开）交叉印证，
  难度真实单调、死亡 / 重开路径通畅。

### 14.3 本里程碑修复的真机软锁（两处）

1. **换装拒绝**（M4）：`Hero::equip` 旧守卫 `(*slot)->equipped == which` 恒真 → 改为
   `*slot == it` 才 no-op，换装自动卸旧件。
2. **护身符丢失**（M6）：12F 护身符在 `generate(reset_view 之后)` 才落基座，随后
   `descend_stairs → spawn_level_content` 的「清非持有地上物」把它误回收、槽位被后续
   mob/drop 复用 → 到 12F 拿不到护身符无法通关。修复：回收循环跳过 `K_AMULET`。

### 14.4 取证入口一览（同一 COM6 会话）

`k`=M3 自检 · `p`=M4 物品自检 · `y`=M5 关卡生成自检 · `u`=M6 全程通关链路 ·
`play_test.py descend N`=真机逐层下楼冒烟（站楼梯按 `r` 触发 DG_BTN_DESCEND）。

---

**文档结束**。等 clone 完把这份 DESIGN.md 搬进仓库根目录即可开工。
