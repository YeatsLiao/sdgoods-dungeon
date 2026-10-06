#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fetch_assets.py - 从 Shattered Pixel Dungeon 上游仓库拉取素材到 resources/。

策略（与 .gitignore 一致）：上游素材**不入本仓库 git**，本机按需拉取后由
pack_assets.py 打成 assets.bin 烧进 assets 分区。发布时分两种通道：
  * GitHub Releases 的 full.bin —— 已把素材合并进整机镜像（CC-BY-SA 4.0 允许再分发，
    署名见 ATTRIBUTION.txt，这是合法的）；
  * 源码仓库不含 resources/ —— 自行 build 的用户跑本脚本补齐。

拉取内容（浅克隆上游仓库，只拷渲染/音频必需目录，保留相对路径）：
    android/assets/tiles/        -> resources/tiles/        （tiles*.png 等图集）
    android/assets/interface/    -> resources/interface/    （UI 图集）
    android/assets/images/       -> resources/images/       （物品/状态图标）
    android/assets/sounds/       -> resources/sounds/       （音效 mp3）
    android/assets/music/        -> resources/music/        （BGM mp3）
    android/assets/interactions/ -> resources/interactions/ （对话框纹理，可选）
只取白名单扩展名（.png/.mp3），其余 Java/其他资源一概不拷。

用法：
    python tools/fetch_assets.py                  # 默认分支 master，缓存到 .cache/upstream
    python tools/fetch_assets.py --ref v25.4      # 锁定版本标签（发布时务必锁！）
    python tools/fetch_assets.py --url <fork地址> # 用自家 fork

注意：上游目录结构可能随版本演进，若某目录 MISSING 脚本会告警并跳过；
第一次跑通后请把有效目录写回本脚本白名单（v0.1 素材管线里程碑）。
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8")
except (AttributeError, ValueError):
    pass

ROOT = Path(__file__).resolve().parent.parent      # sdgoods-dungeon/
CACHE = ROOT / ".cache" / "upstream"
DEFAULT_URL = "https://github.com/00-Evan/shattered-pixel-dungeon.git"

# (上游相对目录, resources 相对目录, 扩展名白名单)
WHITELIST = [
    ("android/assets/tiles",        "tiles",        (".png",)),
    ("android/assets/interface",    "interface",    (".png",)),
    ("android/assets/images",       "images",       (".png",)),
    ("android/assets/sounds",       "sounds",       (".mp3", ".ogg")),
    ("android/assets/music",        "music",        (".mp3", ".ogg")),
    ("android/assets/interactions", "interactions", (".png",)),
]


def git(args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def clone_or_update(url: str, ref: str):
    if (CACHE / ".git").is_dir():
        git(["fetch", "--depth", "1", "origin", ref], cwd=CACHE)
        git(["checkout", "FETCH_HEAD"], cwd=CACHE)
    else:
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        git(["clone", "--depth", "1", "--branch", ref, url, str(CACHE)])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--url", default=DEFAULT_URL, help="上游 git 地址（默认 00-Evan 主仓）")
    ap.add_argument("--ref", default="master", help="分支/标签（发布务必锁标签，默认 master）")
    ap.add_argument("-o", "--out", default="resources", help="输出目录（默认 resources/）")
    args = ap.parse_args()

    out = ROOT / args.out
    print(f"· 浅克隆 {args.url} @ {args.ref} …")
    clone_or_update(args.url, args.ref)

    total_files = total_bytes = 0
    for src_rel, dst_rel, exts in WHITELIST:
        src = CACHE / Path(src_rel)
        if not src.is_dir():
            print(f"  ⚠ 上游目录不存在，跳过：{src_rel}")
            continue
        dst = out / dst_rel
        dst.mkdir(parents=True, exist_ok=True)
        n = b = 0
        for f in src.rglob("*"):
            if f.is_file() and f.suffix.lower() in exts:
                target = dst / f.relative_to(src)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, target)
                n += 1
                b += target.stat().st_size
        total_files += n
        total_bytes += b
        print(f"  ✓ {src_rel} -> {dst_rel}/  {n} 个文件 {b/1048576:.2f} MB")

    print(f"\n✓ 共 {total_files} 个文件 {total_bytes/1048576:.2f} MB -> {out}/")
    print("下一步：python tools/pack_assets.py   # 打成 build_assets/assets.bin")
    print("再烧录：bash tools/flash_local.sh     # 或 python tools/make_full_bin.py 出整机镜像")


if __name__ == "__main__":
    main()
