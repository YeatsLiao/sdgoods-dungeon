# 次元地牢 · sdgoods-dungeon

> 谷仓次元屏（ESP32-S3 · 360×360 圆形触摸屏）上的**独立游戏固件**——把 [Shattered Pixel Dungeon](https://github.com/00-Evan/shattered-pixel-dungeon) 搬上这块可以揣进口袋的圆屏徽章。

与 `sdgoods-doom` 同族：完全脱离 SDGOODS 平台的 launcher 与 appdata 约定，只共用同一块硬件；开机直进地牢，下拉栏保留音量 / 亮度 / 电量 / 关机等系统能力。

## 许可与署名（先读）

| 层 | 许可 | 说明 |
|---|---|---|
| 本工程代码 | **GPL-3.0-only** | 见 [LICENSE](LICENSE)。衍生作品必须同许可开源 |
| Shattered 素材（图 / 音 / 字） | **CC-BY-SA 4.0** | 见 [LICENSE-ASSETS](LICENSE-ASSETS)；完整署名清单见 [ATTRIBUTION.txt](ATTRIBUTION.txt) |
| 平台层 `components/bsp` / `control_center` | Apache-2.0 | 抽自 sdgoods-doom，可随 GPL 固件分发 |
| 上游素材本体 | **不入 git** | 本机用 `tools/fetch_assets.py` 按需拉取（策略见下） |

本工程是 Shattered Pixel Dungeon 的**移植衍生作品**，与 00-Evan / Watabou 无隶属关系。

## 它能跑在哪 / 不能跑在哪

- ✅ 谷仓次元屏 ESP32-S3-R8（8MB OPI PSRAM / 32MB Flash），刷 `full.bin` 整机镜像即可
- ✅ 单点触摸——地牢是**回合制**，点一格走一格 / 点怪砍一刀，天生不需要多点与按键
- ❌ 不能装进 SDGOODS 商店当 app（本固件自定义分区，依赖 12MB raw assets 分区）

## 目录结构

```
sdgoods-dungeon/
├── main/                    # 应用层（纯 C）：boot-direct 装配 + LVGL 首屏
│   └── game/ui_dungeon.c    #   状态栏 / 200×200 主视窗 / 底部 6 颗白悬浮圆键
├── components/
│   ├── dungeon_engine/      # 自研 C++ 引擎（GPL-3.0），对外只有 dungeon_api.h 一个 C ABI 边界
│   │   └── src/             #   rng(Java Random 复刻) / core / actor / pathfinder / assets / save
│   ├── bsp/                 # 板级支持包：ST77916 屏 / CST816 触摸 / LVGL / 音频 / 电源键（Apache-2.0）
│   ├── control_center/      # 下拉控制中心：音量 亮度 电量 关机（Apache-2.0）
│   └── jpegenc/             # 截图通道依赖（Apache-2.0）
├── platform/partitions.csv  # 紧凑单体分区：factory 4MB + assets 12MB raw + save 256KB NVS
├── tools/                   # fetch_assets / pack_assets / flash_local / make_full_bin
├── resources/               # 上游素材落地处（.gitignore 排除，fetch_assets.py 填充）
├── DESIGN.md                # 移植设计方案（分区、素材格式、UI 布局、版本路线图）
└── ATTRIBUTION.txt          # CC-BY-SA 素材署名清单
```

## 架构约定（改动前先读 DESIGN.md）

1. **UI 与引擎严格隔离**：`main/` 全部纯 C，只 `#include "dungeon_api.h"`；C++ 类 / STL / 模板不允许越过该边界。
2. **RNG 是契约**：`java_random.cpp` 逐位复刻 `java.util.Random`（LCG，48-bit 状态）。相同 seed 必须生成与 Shattered 相同的地图，任何"顺手改掉"都会让存档作废。
3. **素材 = raw 分区 + FNV-1a offset 表**：无文件系统。`pack_assets.py`（Python）与 `assets_blob.cpp`（C++）共享同一 SDGA 二进制格式，改一边必须改另一边。
4. **分区表一次定死**：内容按 v0.1→v1.0 分阶段交付，但分区永远不动（改分区 = 老用户存档全丢）。
5. **所有 LVGL 调用在 `sdgoods_lvgl_loop` 线程**；回合推进由 UI 每帧 poll 驱动，引擎不在别处起线程。

## 快速上手

### 环境

- ESP-IDF `v5.1+`（CI 用 v5.5），目标芯片 `esp32s3`
- Python 3.9+，git（用于拉取上游素材）

### 构建

```bash
idf.py set-target esp32s3
idf.py -B build_pub build          # 产物 build_pub/SDGOODS_DUNGEON.bin
```

骨架版**不依赖素材即可开机**：主视窗显示棋盘占位 + 红点玩家，用来验证显示 / 触摸 / 按键链路。

### 素材管线（三件套）

```bash
python tools/fetch_assets.py --ref v25.4   # 1. 浅克隆上游，白名单拷素材到 resources/
python tools/pack_assets.py                # 2. 打包 resources/ -> build_assets/assets.bin (SDGA)
bash tools/flash_local.sh                  # 3. 一键烧录（app @0x10000 + assets @0x410000）
```

### 发布（GitHub Releases 通道）

```bash
python tools/make_full_bin.py build_pub    # 合并出 sdgoods-dungeon-full.bin(.zip)
```

用户拿到单张镜像一键刷：

```bash
esptool.py --chip esp32s3 -p <COM> -b 921600 write_flash 0x0 sdgoods-dungeon-full.bin
```

## 路线图

| 版本 | 内容 |
|---|---|
| **v0.1 骨架**（当前） | 工程全链路：分区 / CI / 素材管线 / 显示触摸装配 / Java RNG 自测 |
| v0.2 | 真实 tilemap 渲染（tiles.png 图集 → 12×12 视窗）、回合制移动与 A* 寻路上屏 |
| v0.3 | 战士 + 第 1 章下水道（Slime King Boss），物品 / 背包 / 存档 3 槽 |
| v0.4 | 四职业 + 全 5 章 26 层，素材全量入 12MB 分区 |
| v1.0 | 平衡性对齐上游当期版本，中文文案校对，正式发布 |

## 致谢

- [00-Evan](https://github.com/00-Evan) —— Shattered Pixel Dungeon 十年维护
- [Watabou](https://watabou.itch.io/) —— Pixel Dungeon 原版与全部基础素材
- Kenney（UI audio pack）、字体与中文翻译社区 —— 明细见 ATTRIBUTION.txt
- [SDGOODS 官方](https://github.com/SDGOODS/SDGOODS-ESP32S3) —— 硬件与平台基座
