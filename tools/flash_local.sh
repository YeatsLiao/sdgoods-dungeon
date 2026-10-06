#!/bin/bash
# 谷仓次元屏 · 次元地牢 本地调试一键烧录（仅用于开发阶段自测，不走平台）
#
# 烧录内容（紧凑单体布局，与 platform/partitions.csv 严格一致）：
#   0x0       <build>/bootloader/bootloader.bin      （本工程自编引导层）
#   0x8000    <build>/partition_table/partition-table.bin
#   0x10000   <build>/SDGOODS_DUNGEON.bin            （factory app 4MB，单应用直启）
#   0x410000  build_assets/assets.bin                （assets raw 分区起点，12MB）
#
# 与 sdgoods-doom 的差异：
#   * factory 从 2MB 扩到 4MB，故素材分区从 0x210000 后移到 0x410000
#   * 素材镜像由 tools/pack_assets.py 生成（SDGA 格式，见脚本头注释）
#   * assets.bin 缺失不阻断烧录 —— 骨架引擎容忍无素材（棋盘占位画面照常起）
#
# 紧凑分区表无 otadata：factory 是唯一 app，bootloader 无选槽记录即直启 factory，
# 无需擦 otadata。save/storage 分区由运行时 NVS/FAT 自管，本脚本不预烧。
#
# 说明：本工程是**独立单应用地牢固件**，不走平台多应用安装链路，引导层 / 分区表
#       都用本工程自己编译产物（而非平台 prebuilt），保证与 app 同一套构建。
#
# 用法：
#   bash tools/flash_local.sh                       # 自动探测串口，默认构建目录 build_pub
#   bash tools/flash_local.sh -p /dev/cu.usbmodem1234
#   bash tools/flash_local.sh -p COM5 -b build_pub -B 921600
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="build_pub"
PORT=""
BAUD="115200"
APP_BIN="SDGOODS_DUNGEON.bin"
ASSETS="$ROOT/build_assets/assets.bin"
ASSETS_ADDR="0x410000"

usage() { echo "用法: $0 [-p 串口] [-b 构建目录] [-B 波特率]"; exit 1; }
while getopts "p:b:B:" o; do
  case "$o" in
    p) PORT="$OPTARG" ;;
    b) BUILD="$OPTARG" ;;
    B) BAUD="$OPTARG" ;;
    *) usage ;;
  esac
done

# 校验本工程自编的引导层 / 分区表存在（必须来自 build 目录，不能用平台 prebuilt）
BL="$ROOT/$BUILD/bootloader/bootloader.bin"
PT="$ROOT/$BUILD/partition_table/partition-table.bin"
for f in "$BL" "$PT"; do
  if [ ! -f "$f" ]; then
    echo "✗ 缺少自编引导/分区表：$f" >&2
    echo "  请先编译：idf.py -B $BUILD build" >&2
    exit 1
  fi
done

APP="$ROOT/$BUILD/$APP_BIN"
if [ ! -f "$APP" ]; then
  echo "✗ 找不到 app 镜像：$APP" >&2
  echo "  请先编译：idf.py -B $BUILD build" >&2
  exit 1
fi

# 自动探测串口（macOS / Linux / Windows-GitBash）
if [ -z "$PORT" ]; then
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyUSB* /dev/tty.usbserial* 2>/dev/null | head -1)"
  if [ -z "$PORT" ]; then
    echo "✗ 未指定串口且自动探测失败，请用 -p 指定（Windows 如 COM5）" >&2
    exit 1
  fi
  echo "· 自动选用串口：$PORT"
fi

# 找 esptool（优先 IDF 环境，否则 PATH）
ESPTOOL_CMD=()
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/components/esptool_py/esptool/esptool.py" ]; then
  ESPTOOL_PY="$IDF_PATH/components/esptool_py/esptool/esptool.py"
  PY="$(ls "$HOME"/.espressif/python_env/*/bin/python 2>/dev/null | head -1 || true)"
  if [ -z "$PY" ] && command -v python3 >/dev/null 2>&1; then
    PY="$(command -v python3)"
  fi
  if [ -n "$PY" ]; then
    ESPTOOL_CMD=("$PY" "$ESPTOOL_PY")
  fi
fi
if [ ${#ESPTOOL_CMD[@]} -eq 0 ] && command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL_CMD=(esptool.py)
fi
if [ ${#ESPTOOL_CMD[@]} -eq 0 ] && command -v esptool >/dev/null 2>&1; then
  ESPTOOL_CMD=(esptool)
fi
if [ ${#ESPTOOL_CMD[@]} -eq 0 ]; then
  echo "✗ 找不到 esptool，请先 source \$IDF_PATH/export.sh 或安装 esptool" >&2
  exit 1
fi

echo "· 烧录到 $PORT (baud=$BAUD)"
echo "    bootloader      -> 0x0"
echo "    partition-table -> 0x8000"
echo "    app ($APP_BIN)  -> 0x10000"
echo

"${ESPTOOL_CMD[@]}" -p "$PORT" -b "$BAUD" --before=default_reset --after=no_reset \
    write_flash 0x0     "$BL" \
                0x8000  "$PT" \
                0x10000 "$APP"

# 素材分区（assets raw @0x410000）：SDGA 镜像由 pack_assets.py 生成。
# 缺失时不阻断 —— 骨架引擎 assets_blob.cpp 对无素材启动是容忍的（LOGW + 棋盘占位）。
if [ -f "$ASSETS" ]; then
  echo "· 烧录素材 assets.bin -> $ASSETS_ADDR (assets)"
  "${ESPTOOL_CMD[@]}" -p "$PORT" -b "$BAUD" --before=default_reset --after=no_reset \
      write_flash "$ASSETS_ADDR" "$ASSETS" || { echo "✗ assets 烧录失败" >&2; exit 1; }
else
  echo "· 跳过 assets（无 build_assets/assets.bin，将运行棋盘占位画面）" >&2
  echo "  生成方法：python tools/pack_assets.py" >&2
fi

"${ESPTOOL_CMD[@]}" -p "$PORT" --before=default_reset --after=hard_reset run

echo
echo "✓ 烧录完成。本地调试固件已写入；正式发布走 GitHub Releases（make_full_bin.py 产物）。"
