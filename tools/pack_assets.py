#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pack_assets.py - 把 Shattered 素材目录打包成单张 assets.bin（SDGA 格式）。

格式与 components/dungeon_engine/src/assets/assets_blob.cpp 严格一致，
任何一侧改动必须同步另一侧（架构级契约，勿动）：

    [Header 32B]
      magic    4B  "SDGA"
      version  4B  = 1  (uint32 LE)
      count    4B  条目数 (uint32 LE)
      reserved 20B (全 0)
    [Entry table 16B x count]
      hash     4B  FNV-1a(name) 低 32 位 (uint32 LE)
      kind     4B  0=png_atlas 1=sprite 2=sound 3=music 4=font 5=properties 6=json
      offset   4B  相对 assets.bin 起点（= 相对 assets 分区起点，分区头部不放别的镜像）
      size     4B  字节数
    [Data blobs...]  每个 blob 4 字节对齐，间隙填 0x00

    注意：C++ 侧 MAGIC = 0x41474453，即小端字节序 'S','D','G','A' ——
    Python struct.pack("<4s", b"SDGA") 写出的原始字节正是这个语义。

用法：
    python tools/pack_assets.py                     # 默认 resources/ -> build_assets/assets.bin
    python tools/pack_assets.py -r resources -o build_assets/assets.bin
    python tools/pack_assets.py --manifest assets.csv  # CSV 行：kind,name,path（覆盖扩展名推断）

kind 推断规则（无 manifest 时，按扩展名 + 路径前缀）：
    .png/.jpg            -> 0 png_atlas
    .bmp/.raw/.dat       -> 1 sprite
    music/ 前缀或 bgm_*  -> 3 music
    其余 .mp3/.wav/.ogg  -> 2 sound
    fonts/ 前缀或 .fnt   -> 4 font
    .properties          -> 5
    .json                -> 6

素材名（参与 FNV-1a 的 key）= 相对资源根目录的 POSIX 风格路径，
例如 "tiles.png"、"interface.png"、"sounds/zh/impossible.mp3"。
运行时 C++ 侧用同一规则算 hash 查表，故目录结构即命名空间。

预烘焙（v0.2 架构决策：ESP32 侧不做 PNG 解码，零解码依赖）：
    BAKE_MAP 里的源图在打包时用 Pillow 转成自定义 RGB565 裸镜像，
    以新名字（kind=1 sprite）追加进同一张 SDGA。容器格式（与
    components/dungeon_engine/src/gfx/gfx.cpp 严格对偶，改动需双侧同步）：
        magic   4B  "RGB5"
        width   2B  uint16 LE
        height  2B  uint16 LE
        pixels  w*h*2 字节，RGB565 小端（与 LVGL TRUE_COLOR 同序）
    Pillow 不可用时跳过烘焙只出原图条目（引擎自动回退棋盘占位）。

版权红线：resources/ 下的 Shattered 素材受 CC-BY-SA 4.0 保护，
署名清单见仓库根 ATTRIBUTION.txt；本脚本产物 assets.bin 不提交进 git
（.gitignore 已排除），随固件 full.bin 一起以 GPL/CC-BY-SA 整体分发。
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    Image = None

try:
    sys.stdout.reconfigure(encoding="utf-8")
except (AttributeError, ValueError):
    pass

MAGIC   = b"SDGA"
VERSION = 1
HEADER_SIZE   = 32
ENTRY_SIZE    = 16
PARTITION_CAP = 0xC00000          # platform/partitions.csv: assets = 12MB

KIND_PNG_ATLAS, KIND_SPRITE, KIND_SOUND, KIND_MUSIC = 0, 1, 2, 3
KIND_FONT, KIND_PROPERTIES, KIND_JSON = 4, 5, 6
KIND_NAME = {0: "png_atlas", 1: "sprite", 2: "sound", 3: "music",
             4: "font", 5: "properties", 6: "json"}

# 预烘焙表：源 png -> 烘焙产物名（kind=1，内容为 RGB5 容器）。
# 引擎 gfx.cpp 按这些名字查 hash 表；新增图集时两侧同步。
BAKE_MAP = {
    "environment/tiles_sewers.png": "tiles/sewers.rgb565",   # 256×256 地形图集（第 1 章下水道）
    "sprites/rat.png":              "sprites/rat.rgb565",    # 256×64  怪物帧图（第 0 帧=idle）
    "sprites/rogue.png":            "sprites/hero.rgb565",   # 256×128 英雄帧图（idle 帧在 (1,0) 12×15）
}


def bake_rgb565(png_bytes):
    """PNG 字节 -> RGB5 容器字节（RGBA 直转 565，丢弃 alpha 合成到黑底）。"""
    import io
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    w, h = img.size
    rgba = img.tobytes()
    out = bytearray(b"RGB5" + struct.pack("<HH", w, h))
    for i in range(0, w * h * 4, 4):
        r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
        out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)



def fnv1a(s: str) -> int:
    """FNV-1a 32-bit —— 与 assets_blob.cpp::fnv1a 严格一致。"""
    h = 2166136261
    for ch in s.encode("utf-8"):
        h ^= ch
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def guess_kind(name: str) -> int:
    p = name.lower().replace("\\", "/")
    ext = Path(p).suffix
    base = Path(p).name
    if p.startswith("fonts/") or ext == ".fnt":
        return KIND_FONT
    if p.startswith("music/") or base.startswith("bgm_"):
        return KIND_MUSIC
    if ext in (".png", ".jpg", ".jpeg"):
        return KIND_PNG_ATLAS
    if ext in (".bmp", ".raw", ".dat"):
        return KIND_SPRITE
    if ext in (".mp3", ".wav", ".ogg"):
        return KIND_SOUND
    if ext == ".properties":
        return KIND_PROPERTIES
    if ext == ".json":
        return KIND_JSON
    return KIND_SPRITE          # 未知二进制按 sprite 装，运行时按 name 自取


def collect(root, manifest):
    """返回 [(kind, name, bytes)]，name 为 POSIX 相对路径。"""
    items = []
    if manifest:
        with manifest.open(newline="", encoding="utf-8") as fp:
            for row in csv.reader(fp):
                if not row or row[0].startswith("#"):
                    continue
                kind, name = int(row[0]), row[1]
                rel = row[2] if len(row) > 2 else name
                f = root / rel.replace("\\", "/")
                items.append((kind, name, f.read_bytes()))
    else:
        for f in sorted(root.rglob("*")):
            if not f.is_file() or f.name.startswith("."):
                continue
            name = f.relative_to(root).as_posix()
            items.append((guess_kind(name), name, f.read_bytes()))
    return items


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-r", "--resources", default="resources",
                    help="素材根目录（默认 resources/）")
    ap.add_argument("-o", "--out", default="build_assets/assets.bin",
                    help="输出镜像（默认 build_assets/assets.bin）")
    ap.add_argument("--manifest", default=None,
                    help="可选 CSV：kind,name[,path]，覆盖扩展名推断")
    ap.add_argument("--exclude", action="append", default=[],
                    help="按路径前缀排除（如 music/），可多次）")
    args = ap.parse_args()

    root = Path(args.resources)
    if not root.is_dir():
        sys.exit(f"✗ 资源目录不存在：{root}\n"
                 "  骨架阶段可先 mkdir resources 放测试 png；正式素材见 README『素材管线』一节。")

    items = collect(root, Path(args.manifest) if args.manifest else None)
    if args.exclude:
        before = len(items)
        items = [it for it in items
                 if not any(it[1].startswith(p) for p in args.exclude)]
        print(f"· --exclude 过滤掉 {before - len(items)} 个条目")
    if not items:
        sys.exit(f"✗ {root} 为空，没有可打包的素材")

    # 预烘焙：源图存在才烘，Pillow 缺失时告警不阻断（引擎回退棋盘占位）
    if Image is not None:
        by_name = {name: data for _, name, data in items}
        for src, baked in BAKE_MAP.items():
            if src in by_name:
                items.append((KIND_SPRITE, baked, bake_rgb565(by_name[src])))
                print(f"· 烘焙 {src} -> {baked} ({len(items[-1][2])/1024:.0f} KB)")
    else:
        print("⚠ 未安装 Pillow，跳过 RGB565 预烘焙 —— 引擎将无真实贴图！"
              "（pip install pillow）")

    # 名字 hash 冲突检测（FNV-1a 32 位，千级条目撞概率极低，但撞了必须 fail）
    seen = {}
    for kind, name, data in items:
        h = fnv1a(name)
        if h in seen:
            sys.exit(f"✗ hash 冲突：'{name}' 与 '{seen[h]}' 同为 0x{h:08x}，请改名")
        seen[h] = name

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    blob_off = HEADER_SIZE + ENTRY_SIZE * len(items)
    total_data = sum(len(d) for _, _, d in items)
    img_max = blob_off + total_data + 3 * len(items)
    if img_max > PARTITION_CAP:
        sys.exit(f"✗ 打包后约 {img_max/1048576:.1f} MB，超过 assets 分区 12 MB —— 裁剪音轨/字体子集")

    with out.open("wb") as fp:
        fp.write(MAGIC + struct.pack("<I", VERSION) + struct.pack("<I", len(items)))
        fp.write(b"\x00" * 20)                       # reserved

        offset = blob_off
        table = []
        for kind, name, data in items:
            table.append((fnv1a(name), kind, offset, len(data)))
            pad = (-len(data)) & 3
            offset += len(data) + pad

        for h, kind, off, size in table:
            fp.write(struct.pack("<IIII", h, kind, off, size))

        for _, _, data in items:
            fp.write(data)
            fp.write(b"\x00" * ((-len(data)) & 3))   # 4 字节对齐

    size = out.stat().st_size
    print(f"✓ {out}  {size:,} B = {size/1048576:.2f} MB（分区余量 {(PARTITION_CAP-size)/1048576:.2f} MB）")
    per_kind = {}
    for kind, _, _ in items:
        per_kind[KIND_NAME[kind]] = per_kind.get(KIND_NAME[kind], 0) + 1
    for k, v in sorted(per_kind.items()):
        print(f"    {k:<11} x {v}")
    print("\n下一步：tools/flash_local.sh 会把它烧到 0x410000（assets 分区起点）")


if __name__ == "__main__":
    main()
