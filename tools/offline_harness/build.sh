#!/usr/bin/env bash
# Builds and runs the offline harness: the CPU half of lua/worlddraw.cpp
# exercised without a graphics device. It #includes the module itself, so what
# is checked is the shipping code rather than a copy of it, and the Lua C API
# is stubbed only so the module's entry points link.
#
# The harness runs six times, each in a process of its own: the behavioural
# pass, the two daemon refusals and the three device-behaviour answers, all of
# which are sticky for the life of an image by design.
#
# It then gates the demo's copy of the kit against the kit, because a copy
# drifts.
#
# It also gates at the symbol level what the shipping image must and must not
# carry: the module's Lua entry point, the four handlers the daemon calls and
# the functions that load and register with it, and nothing queued to run at
# process exit -- static destructors run then whatever DllMain does, and there
# is nothing this image can safely do at that point.
#
# Exits non-zero when a check fails, so it can gate a change.
set -euo pipefail
cd "$(dirname "$0")"
LUA_INC=${LUA_INC:-/usr/include/lua5.1}
mkdir -p build

FLAGS=(-std=c++17 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Wpedantic -I"$LUA_INC")

i686-w64-mingw32-g++ "${FLAGS[@]}" \
    -static -static-libgcc -static-libstdc++ \
    -o build/worlddraw_harness.exe worlddraw_harness.cpp -lgdiplus
echo "built: $(pwd)/build/worlddraw_harness.exe"

# ---- the module's own object, for the symbol checks below -----------------
# An object rather than a linked image: what matters is what this module
# emits, not what the C++ runtime brings with it. The preprocessed text goes
# beside it, because an object only carries what is used -- an inlined-away
# function is absent from one and present in the other.
i686-w64-mingw32-g++ "${FLAGS[@]}" -c -o build/worlddraw.o ../../lua/worlddraw.cpp
i686-w64-mingw32-g++ "${FLAGS[@]}" -E ../../lua/worlddraw.cpp > build/worlddraw.i

# ---- THE BLOCKER'S GATE --------------------------------------------------
# No device method may be reachable from an l_* entry point. FFXI's device is
# created without D3DCREATE_MULTITHREADED and its hooks run on a different OS
# thread from Windower's Lua, so one that is reachable is an access violation
# inside d3d8.dll on a real client, and no lock of ours can fix it -- the
# GAME's render thread calls the same device and cannot be made to take one.
#
# This is the static half: a reachability walk over the preprocessed text of
# the two files this library is. The behavioural half is the thread gate
# inside worlddraw_harness.cpp, which catches a call however it got there.
# Neither replaces the other, and both fail the build.
python3 gate_device_calls.py build/worlddraw.i

symbols=$(i686-w64-mingw32-nm -C build/worlddraw.o)
failed=0

# The Lua entry point Windower's LuaCore looks for. Defined, not referenced: a
# module that does not export it loads as nothing at all.
if ! grep -qE '(^| )T .*luaopen_worlddraw' <<< "$symbols"; then
    echo "CHECK FAILED: luaopen_worlddraw is not defined by the module"
    failed=1
fi

# How this library hooks: the four handlers the daemon calls, the functions
# that find it, load it and register with it, the self-reference that keeps
# this image mapped while a registration stands, and the stomp poll.
for name in daemon_pre_reset daemon_post_reset daemon_pre_set_render_target daemon_pre_draw \
        EnsureDaemon attach_to_daemon detach_from_daemon self_reference_ check_hook_slots \
        stomp_reported_; do
    if ! grep -q -- "$name" build/worlddraw.i; then
        echo "CHECK FAILED: the module is missing $name"
        failed=1
    fi
done

# The pass state is the engine's, one copy per image, and the daemon holds
# none of it.
for name in hook_armed_ hook_drawing_ capture_pass_transforms pass_transforms_valid_; do
    if ! grep -q -- "$name" build/worlddraw.i; then
        echo "CHECK FAILED: the module has lost $name"
        failed=1
    fi
done

# The pieces the fixes put in, each of which is silently undoable by deleting
# one line: the one-shot device-behaviour report, the mesh finaliser, the two
# error accessors that keep the two kinds of message apart, the release of
# everything the image owns on the last close, and the deferred-work list that
# keeps every one of those releases off the main thread -- its queue, its
# drain, and the two things that stage rather than upload.
for name in report_device_behavior device_behavior_reported_ l_mesh_gc \
        player_error engineering_error release_all_textures \
        push_deferred drain_deferred deferred_head_ realize_texture \
        stage_draw_data drain_pending_uploads; do
    if ! grep -q -- "$name" build/worlddraw.i; then
        echo "CHECK FAILED: the module is missing $name"
        failed=1
    fi
done

# And the one that must NOT be there. This image takes a reference on itself
# and never gives it back: the release would have to happen on a path that is
# also a Lua __gc, where dropping the last reference unmaps the code that is
# running and the return lands in nothing. A FreeLibrary anywhere in the
# shipping text is that bug coming back.
if grep -q '::FreeLibrary' build/worlddraw.i; then
    echo "CHECK FAILED: the module can FreeLibrary -- it must never unmap itself"
    failed=1
fi
if grep -q 'release_self_reference' build/worlddraw.i; then
    echo "CHECK FAILED: release_self_reference is back; see take_self_reference"
    failed=1
fi

# Nothing of ours may run at process exit: a static destructor in a mapped
# image runs then whatever DllMain does. The check belongs on the object --
# the linked image carries the CRT's own atexit machinery, which is not ours
# and never fires for us.
if grep -qE '(^| )U .*(atexit|__cxa_atexit)' <<< "$(i686-w64-mingw32-nm build/worlddraw.o)"; then
    echo "CHECK FAILED: the module registers something to run at process exit"
    i686-w64-mingw32-nm build/worlddraw.o | grep -E '(^| )U .*(atexit|__cxa_atexit)'
    failed=1
fi

if [ "$failed" != "0" ]; then
    echo "FAIL   : the symbol checks did not hold"
    exit 1
fi
echo "symbols: luaopen_worlddraw is defined, the daemon handlers and the"
echo "         registration path are present, and nothing runs at process exit"

export WINEDEBUG="${WINEDEBUG:--all}"
echo
echo "--- the engine's CPU half ---"
wine build/worlddraw_harness.exe
echo
echo "--- daemon missing beside the image ---"
wine build/worlddraw_harness.exe daemon-missing
echo
echo "--- the daemon refuses the registration ---"
wine build/worlddraw_harness.exe daemon-full

# The one-shot device-behaviour report -- the diagnostic that asks whether the
# device carries D3DCREATE_MULTITHREADED. One answer per image by design, so
# one process per answer.
echo
echo "--- the device reports its behaviour flags: not multithreaded ---"
wine build/worlddraw_harness.exe device-flags
echo
echo "--- the device reports its behaviour flags: multithreaded ---"
wine build/worlddraw_harness.exe device-flags-mt
echo
echo "--- the device will not say what it was created with ---"
wine build/worlddraw_harness.exe device-flags-refused

# The Lua half, on the same terms: the real lua/worlddraw.lua over a fake
# windower and a fake native module, so what is checked is the file that ships.
echo
echo "--- lua/worlddraw.lua over a fake windower ---"
luac5.1 -p ../../lua/worlddraw.lua
echo "syntax : lua/worlddraw.lua compiles"
lua5.1 lua_test.lua

# ---- the demo carries the kit, byte for byte -----------------------------
# lua/worlddrawdemo ships a copy of the three files it is a demo OF, and a
# copy drifts: worlddrawdemo/libs/worlddraw.lua was 313 bytes adrift of its
# master before this gate existed, which makes the demo a demo of something
# nobody has. Deployment is by copy, so identity is the whole of the contract.
echo
demo=../../lua/worlddrawdemo/libs
drift=0
check_copy() {
    if [ ! -f "$1" ]; then
        echo "CHECK FAILED: the master $1 is not built. Run lua/build.sh."
        drift=1
    elif [ ! -f "$2" ]; then
        echo "CHECK FAILED: $2 is missing; copy it from $1"
        drift=1
    elif ! cmp -s "$1" "$2"; then
        echo "CHECK FAILED: $2 has drifted from $1"
        echo "              cp $1 $2"
        drift=1
    fi
}
check_copy ../../lua/worlddraw.lua "$demo/worlddraw.lua"
check_copy ../../lua/worlddraw.dll "$demo/worlddraw.dll"
check_copy ../../daemon/worlddraw_daemon.dll "$demo/worlddraw_daemon.dll"
if [ "$drift" != "0" ]; then
    echo "FAIL   : the demo is not carrying the kit it demonstrates"
    exit 1
fi
echo "demo   : worlddrawdemo/libs is byte-identical to the kit it demonstrates"
