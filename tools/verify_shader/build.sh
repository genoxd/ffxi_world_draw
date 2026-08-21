#!/usr/bin/env bash
# Builds and runs the shader-token proof: assemble the listing that documents
# gpu_shader_function in ffxi_world_draw.h and byte-compare it with the tokens
# embedded there. Exits non-zero if they differ, so it can gate a change.
#
# The same assembler is also the GENERATOR: `generate` rewrites the token
# arrays in the header from what it assembles, between their BEGIN/END
# GENERATED markers, and then verifies what it wrote. Nothing is ever encoded
# by hand -- change the listing, regenerate, and the bytes follow.
#
#   ./build.sh                 verify: the first assembler DLL that answers
#   ./build.sh d3dx8d.dll      force one (any d3dx8 name gets the D3DX8 ABI)
#   ./build.sh generate        regenerate the header's token arrays, then verify
#   ./build.sh generate - path/to/ffxi_world_draw.h    (a dash: no forced DLL)
#
# Wine ships no d3dx8, so the default run assembles through Wine's own
# d3dx9_43.dll. A vs_1_1 shader assembles to the same D3D8-era token stream
# either way; pass a real d3dx8 here if one is installed and see for yourself.
set -euo pipefail
cd "$(dirname "$0")"

i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -static -static-libgcc -static-libstdc++ \
    -o verify_shader.exe verify_shader.cpp

echo "built: $(pwd)/verify_shader.exe"
WINEDEBUG="${WINEDEBUG:--all}" wine verify_shader.exe "$@"
