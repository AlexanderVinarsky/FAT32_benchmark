#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./build.sh                 # build + make image + run
#   DO_BUILD=1 ./build.sh      # only build
#   DO_IMAGE=1 ./build.sh      # only create image
#   DO_RUN=1   ./build.sh      # only run
#   CLEAN=1    ./build.sh      # remove bench.bin and image after run
#
# Env vars:
#   IMG=image.img
#   SIZE=64MiB
#   ITERS=100
#   MODE=release|debug
#   OUT=results.txt

IMG="${IMG:-image.img}"
SIZE="${SIZE:-64MiB}"
ITERS="${ITERS:-100}"
MODE="${MODE:-release}"
OUT="${OUT:-results.txt}"

if [[ "${DO_BUILD:-}" != "1" && "${DO_IMAGE:-}" != "1" && "${DO_RUN:-}" != "1" ]]; then
  DO_BUILD=1
  DO_IMAGE=1
  DO_RUN=1
fi

SRC_DIR=""
if [[ -d "src" ]]; then
  SRC_DIR="src"
elif [[ -d "source" ]]; then
  SRC_DIR="source"
else
  echo "ERROR: cannot find source directory: 'src' or 'source'" >&2
  exit 1
fi

if [[ "$MODE" == "debug" ]]; then
  CFLAGS="-std=c11 -O0 -g -Wall -Wextra"
else
  CFLAGS="-std=c11 -O2 -DNDEBUG -Wall -Wextra"
fi

if [[ "${DO_IMAGE:-0}" == "1" ]]; then
  echo "[image] creating $IMG (size=$SIZE)"
  python3 fat_builder.py --size "$SIZE" --output "$IMG" --init-root-dir
fi

if [[ "${DO_BUILD:-0}" == "1" ]]; then
  echo "[build] gcc $MODE"
  gcc $CFLAGS -Iinclude main.c "$SRC_DIR"/*.c -o bench.bin
fi

if [[ "${DO_RUN:-0}" == "1" ]]; then
  if [[ ! -f "$IMG" ]]; then
    echo "ERROR: image not found: $IMG (run with DO_IMAGE=1 first)" >&2
    exit 1
  fi
  if [[ ! -f "./bench.bin" ]]; then
    echo "ERROR: bench.bin not found (run with DO_BUILD=1 first)" >&2
    exit 1
  fi

  echo "[run] ./bench.bin $ITERS $IMG"
  {
    echo "-----"
    echo "date: $(date -Is 2>/dev/null || date)"
    echo "mode: $MODE"
    echo "iters: $ITERS"
    echo "image: $IMG"
    echo "size: $SIZE"
    echo "-----"
    ./bench.bin "$ITERS" "$IMG"
    echo
  } | tee -a "$OUT"
fi

if [[ "${CLEAN:-0}" == "1" ]]; then
  echo "[clean] removing bench.bin and $IMG"
  rm -f bench.bin "$IMG"
fi
