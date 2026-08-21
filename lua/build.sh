#!/usr/bin/env bash
# Builds the whole worlddraw kit: the hook daemon, then the engine that
# registers with it. Both compile against daemon/worlddraw_abi.h, so they
# cannot disagree about the ABI; the kit md5 below is what identifies the
# pair, and deploy.sh ships them together.
#
# The engine links against LuaCore.dll, which exports the Lua 5.1 C API; the
# import library comes from Navigation/libLuaCore.a.
set -euo pipefail
cd "$(dirname "$0")"
LUA_INC=${LUA_INC:-/usr/include/lua5.1}
LUA_IMPLIB=${LUA_IMPLIB:-../../Navigation/libLuaCore.a}

# The vtable indices the daemon patches are generated from the SDK header.
# Verify before anything is built: a drifted index patches an unrelated
# method, which corrupts the render thread's stack on every call.
bash ../tools/gen_slots/build.sh > /dev/null
echo "slots  : verified against d3d8.h"

bash ../daemon/build.sh | sed 's/^/daemon : /'

flags=(-std=c++17 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Wpedantic -I"$LUA_INC")

# Compile first so the object can be inspected. Nothing of ours may run at
# image teardown: a static destructor in a mapped image runs at process exit
# whatever DllMain does. The check belongs on the object -- the linked DLL
# carries the CRT's own atexit machinery, which is not ours and never fires
# for us.
i686-w64-mingw32-g++ "${flags[@]}" -c -o worlddraw.o worlddraw.cpp
symbols=$(i686-w64-mingw32-nm worlddraw.o)
if grep -qE '(^| )U .*(atexit|__cxa_atexit)' <<< "$symbols"; then
    echo "BUILD FAILED: the engine registers something to run at process exit"
    grep -E '(^| )U .*(atexit|__cxa_atexit)' <<< "$symbols"
    exit 1
fi
echo "statics: nothing registered to run at image teardown"

# --no-insert-timestamp keeps the link byte-reproducible, so "the DLL we
# shipped is the DLL we built" is a checkable claim rather than a hope.
i686-w64-mingw32-g++ "${flags[@]}" \
    -shared -static -static-libgcc -static-libstdc++ \
    -Wl,--no-insert-timestamp \
    -o worlddraw.dll worlddraw.o "$LUA_IMPLIB" -lgdiplus
rm -f worlddraw.o

# grep -q closes the pipe early, which trips pipefail via SIGPIPE -- read the
# exports into a variable first.
exports=$(i686-w64-mingw32-objdump -p worlddraw.dll)
grep -q 'luaopen_worlddraw' <<< "$exports" \
    || { echo "BUILD FAILED: luaopen_worlddraw not exported"; exit 1; }

kit=$(cat worlddraw.dll worlddraw.lua ../daemon/worlddraw_daemon.dll | md5sum | cut -d' ' -f1)
echo "engine : $(pwd)/worlddraw.dll"
echo "export : luaopen_worlddraw"
echo "md5    : $(md5sum worlddraw.dll | cut -d' ' -f1)"
echo "kit    : $kit  (engine + worlddraw.lua + daemon)"
echo "NOT deployed. Run lua/deploy.sh -- it stages and renames, so a running"
echo "client keeps the images it has until the addon is loaded again."
