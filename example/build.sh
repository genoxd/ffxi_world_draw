#!/usr/bin/env bash
# Cross-compile the example plugin on Linux. On Windows use MSVC with /LD and a
# 32-bit target; the header needs nothing but the Windows and Direct3D 8 SDKs.
#
# The example loads an image, so it links gdiplus. Drop -lgdiplus and the
# FFXI_WORLD_DRAW_IMAGE_LOADING define if you only ever supply pixels yourself.
set -euo pipefail
cd "$(dirname "$0")"
i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -shared -static -static-libgcc -static-libstdc++ \
    -o example.dll example.cpp exports.def -lgdiplus
echo "built: $(pwd)/example.dll"
