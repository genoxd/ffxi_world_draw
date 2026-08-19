#!/usr/bin/env bash
# Builds the Lua module. Links against LuaCore.dll, which exports the Lua 5.1
# C API; the import library comes from Navigation/libLuaCore.a.
set -euo pipefail
cd "$(dirname "$0")"
LUA_INC=${LUA_INC:-/usr/include/lua5.1}
LUA_IMPLIB=${LUA_IMPLIB:-../../Navigation/libLuaCore.a}

i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    -I"$LUA_INC" \
    -shared -static -static-libgcc -static-libstdc++ \
    -o worlddraw.dll worlddraw.cpp "$LUA_IMPLIB" -lgdiplus

# grep -q closes the pipe early, which trips pipefail via SIGPIPE -- read the
# exports into a variable first.
exports=$(i686-w64-mingw32-objdump -p worlddraw.dll)
grep -q 'luaopen_worlddraw' <<< "$exports" \
    || { echo "BUILD FAILED: luaopen_worlddraw not exported"; exit 1; }
echo "built  : $(pwd)/worlddraw.dll"
echo "export : luaopen_worlddraw"
echo "md5    : $(md5sum worlddraw.dll | cut -d' ' -f1)"
echo "NOT deployed. Unload the addon, then: lua/deploy.sh <addon>"
