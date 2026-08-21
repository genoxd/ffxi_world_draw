#!/usr/bin/env bash
# Builds worlddraw_daemon.dll, the immortal hook multiplexer.
#
# The daemon pins itself into the client and can never be unloaded or replaced
# without the player restarting the game, so this script gates the two things
# that cannot be fixed afterwards:
#
#   - exactly one export, undecorated, named wd_daemon_acquire (the engine
#     resolves it by name; a decorated name would be a silent no-draw);
#   - no static constructors and no atexit registrations in our object, because
#     anything registered there runs at process exit whatever DllMain does.
#
# The vtable indices this is built against are proven separately:
#   tools/gen_slots/build.sh
set -euo pipefail
cd "$(dirname "$0")"

FLAGS=(-std=c++17 -O2 -Wall -Wextra -Wpedantic)

i686-w64-mingw32-g++ "${FLAGS[@]}" -c -o worlddraw_daemon.o worlddraw_daemon.cpp

# No dynamic initialisers, no exit-time work. GCC names a translation unit's
# initialiser _GLOBAL__sub_I_*; a non-trivial destructor on a namespace-scope
# object shows up as a reference to atexit or __cxa_atexit.
symbols=$(i686-w64-mingw32-nm worlddraw_daemon.o)
if grep -q '_GLOBAL__sub_I' <<< "$symbols"; then
    echo "BUILD FAILED: the daemon has a static initialiser"
    grep '_GLOBAL__sub_I' <<< "$symbols"
    exit 1
fi
if grep -qE '(^| )U .*(atexit|__cxa_atexit)' <<< "$symbols"; then
    echo "BUILD FAILED: the daemon registers something to run at process exit"
    grep -E '(^| )U .*(atexit|__cxa_atexit)' <<< "$symbols"
    exit 1
fi

# --no-insert-timestamp makes the link reproducible: without it the PE header
# carries the build time and the md5 of an unchanged daemon differs every time,
# which would make "the deployed daemon is the daemon we tested" uncheckable.
i686-w64-mingw32-g++ "${FLAGS[@]}" \
    -shared -static -static-libgcc -static-libstdc++ -Wl,--no-insert-timestamp \
    -o worlddraw_daemon.dll worlddraw_daemon.o exports.def
rm -f worlddraw_daemon.o

# grep -q closes the pipe early, which trips pipefail via SIGPIPE -- read the
# exports into a variable first.
dump=$(i686-w64-mingw32-objdump -p worlddraw_daemon.dll)
exports=$(sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' <<< "$dump" | grep -oE '\[ *[0-9]+\] .*' | sed 's/.*\] //')
if ! grep -qx 'wd_daemon_acquire' <<< "$exports"; then
    echo "BUILD FAILED: wd_daemon_acquire is not exported"
    echo "$exports"
    exit 1
fi
count=$(grep -c . <<< "$exports")
if [ "$count" != "1" ]; then
    echo "BUILD FAILED: expected exactly one export, found $count"
    echo "$exports"
    exit 1
fi

echo "built  : $(pwd)/worlddraw_daemon.dll"
echo "export : wd_daemon_acquire (the only one)"
echo "statics: no dynamic initialisers, no exit-time registrations"
echo "md5    : $(md5sum worlddraw_daemon.dll | cut -d' ' -f1)"
echo "NOT deployed. The daemon ships in each addon's own libs/ folder."
