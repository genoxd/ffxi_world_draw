#!/usr/bin/env bash
# Cross-compile the example plugin on Linux. On Windows use MSVC with /LD and a
# 32-bit target; the header needs nothing but the Windows and Direct3D 8 SDKs.
#
# The plugin ships TWO files, and both go in <windower>/plugins/:
#
#     example.dll
#     worlddraw_daemon.dll        <- staged beside example.dll by this script
#
# The daemon owns the six device vtable slots; the plugin registers observer
# handlers with it. The daemon is looked for beside the image that wants it.
# There is no fallback: without it the plugin loads, says so in chat, and draws
# nothing.
#
# The example loads an image, so it links gdiplus. Drop -lgdiplus and the
# FFXI_WORLD_DRAW_IMAGE_LOADING define in example.cpp if you only ever supply
# pixels yourself.
set -euo pipefail
cd "$(dirname "$0")"

flags=(-std=c++17 -O2 -Wall -Wextra -Wpedantic)

# The daemon is built from source rather than assumed present: the plugin and
# the daemon compile against the same daemon/worlddraw_abi.h, so a pair
# produced together cannot disagree about the ABI.
bash ../daemon/build.sh | sed 's/^/daemon : /'

i686-w64-mingw32-g++ "${flags[@]}" \
    -shared -static -static-libgcc -static-libstdc++ \
    -Wl,--no-insert-timestamp \
    -o example.dll example.cpp exports.def -lgdiplus

echo "plugin : $(pwd)/example.dll"
echo "md5    : $(md5sum example.dll | cut -d' ' -f1)"

# Staged into THIS folder, never into a live Windower plugins/ directory:
# copying over a file the client has mapped corrupts it. Copy both files out of
# here yourself, with the client closed or the plugin unloaded.
cp ../daemon/worlddraw_daemon.dll worlddraw_daemon.dll
echo "daemon : $(pwd)/worlddraw_daemon.dll"
echo "md5    : $(md5sum worlddraw_daemon.dll | cut -d' ' -f1)"
echo "Copy BOTH files into <windower>/plugins/, then //load example."
