#!/usr/bin/env bash
# Builds and runs the vtable-index proof: read the SDK's d3d8.h, count the
# IDirect3DDevice8 methods in declaration order, and check that
# daemon/worlddraw_abi.h carries exactly those indices. Exits non-zero on drift,
# so it can gate a daemon change.
#
# The same tool is the GENERATOR: `generate` rewrites the enum between the
# BEGIN/END GENERATED markers in the header and then verifies what it wrote, so
# no index is ever typed by hand.
#
#   ./build.sh                 verify
#   ./build.sh generate        rewrite the header's indices, then verify
#   ./build.sh generate /path/to/d3d8.h /path/to/worlddraw_abi.h
#
# This one is a host binary, not a wine one: it reads a Linux-side SDK header
# and rewrites a Linux-side source file, and needs no Windows API to do either.
# The header it writes is then compiled for the target below, which is where the
# ABI's static asserts get checked.
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -o gen_slots gen_slots.cpp
echo "built  : $(pwd)/gen_slots"
./gen_slots "$@"

# Compiling the header for the real target is the other half of the proof: the
# indices can be right and the ABI's size and offset asserts still be wrong.
echo '#include "../../daemon/worlddraw_abi.h"' \
    | i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -x c++ -c - -o /dev/null
echo "compile: daemon/worlddraw_abi.h builds clean for i686-w64-mingw32 (asserts hold)"
