#!/usr/bin/env bash
# Usage:
#   ./build.sh                      native build
#   ./build.sh <toolchain-file>     cross build (e.g. Buildroot toolchainfile.cmake)
set -euo pipefail

LVGL_TAG="release/v9.2"
CJSON_TAG="v1.7.18"
TOOLCHAIN="${1:-}"

# 1. Fetch LVGL (fbdev driver is part of the v9 core)
[ -d lvgl ] || git clone --depth 1 --branch "$LVGL_TAG" https://github.com/lvgl/lvgl.git

# 2. Fetch the single-file cJSON parser (vendored, no system libcjson needed)
if [ ! -f third_party/cJSON/cJSON.c ]; then
    mkdir -p third_party/cJSON
    base="https://raw.githubusercontent.com/DaveGamble/cJSON/${CJSON_TAG}"
    curl -fsSL "$base/cJSON.c" -o third_party/cJSON/cJSON.c
    curl -fsSL "$base/cJSON.h" -o third_party/cJSON/cJSON.h
fi

# 3. Fetch lodepng (used to decode gcode thumbnails before converting them
#    to LVGL's RGB565 .bin format; LVGL's own PNG decoder is disabled)
if [ ! -f third_party/lodepng/lodepng.c ]; then
    mkdir -p third_party/lodepng
    base="https://raw.githubusercontent.com/lvandeve/lodepng/master"
    curl -fsSL "$base/lodepng.cpp" -o third_party/lodepng/lodepng.c
    curl -fsSL "$base/lodepng.h"   -o third_party/lodepng/lodepng.h
fi

# 3. Configure + build
if [ -n "$TOOLCHAIN" ]; then
    cmake -B build -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release
else
    cmake -B build -DCMAKE_BUILD_TYPE=Release
fi
cmake --build build -j"$(nproc)"

echo
echo "Done -> ./build/klipper_status"
file build/klipper_status
