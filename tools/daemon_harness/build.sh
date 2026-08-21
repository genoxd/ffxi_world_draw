#!/usr/bin/env bash
# Builds the daemon, three fake-engine images and the driver, then runs the
# driver under wine. Exits non-zero when a check fails, so it can gate a change.
#
# The daemon comes from daemon/build.sh -- the shipping artifact, not a copy
# compiled with different flags -- and is placed in two directories under one
# basename, which the loader treats as two separate images. That is the election
# this cannot test any other way. The engines get three directories for the same
# reason.
set -euo pipefail
cd "$(dirname "$0")"

FLAGS=(-std=c++17 -O2 -Wall -Wextra -Wpedantic)

../../daemon/build.sh > /dev/null
mkdir -p build/a build/b build/c
cp ../../daemon/worlddraw_daemon.dll build/a/worlddraw_daemon.dll
cp ../../daemon/worlddraw_daemon.dll build/b/worlddraw_daemon.dll
echo "daemon : $(md5sum ../../daemon/worlddraw_daemon.dll | cut -d' ' -f1) into build/a and build/b"

i686-w64-mingw32-g++ "${FLAGS[@]}" \
    -shared -static -static-libgcc -static-libstdc++ \
    -o build/a/fake_engine.dll fake_engine.cpp fake_engine.def
cp build/a/fake_engine.dll build/b/fake_engine.dll
cp build/a/fake_engine.dll build/c/fake_engine.dll

i686-w64-mingw32-g++ "${FLAGS[@]}" \
    -static -static-libgcc -static-libstdc++ \
    -o build/daemon_harness.exe daemon_harness.cpp

echo "built  : $(pwd)/build/daemon_harness.exe"
WINEDEBUG="${WINEDEBUG:--all}" wine build/daemon_harness.exe "$@"
