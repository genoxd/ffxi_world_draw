#!/usr/bin/env bash
# Deploys the worlddraw kit -- worlddraw.dll, worlddraw.lua and the hook
# daemon -- into every addon that already carries one.
#
# Deployment is by RENAME, never by copy. cp rewrites the file a running
# client has mapped, which corrupts it silently on a Linux filesystem;
# rename(2) never touches the mapped inode, so a client that is running keeps
# the images it already has and picks the new ones up the next time the addon
# loads. That also removes the old prompt asking whether the addon was
# unloaded -- with several clients running, nobody can answer it honestly.
#
# The daemon is the exception to "reload and you have the new build": it pins
# itself for the life of the client, so a client that has already loaded a
# worlddraw addon keeps the daemon it pinned until it is closed.
set -euo pipefail
cd "$(dirname "$0")"

addons_dir=../../addons
files=(worlddraw.dll worlddraw.lua ../daemon/worlddraw_daemon.dll)

for f in "${files[@]}"; do
    [ -f "$f" ] || { echo "missing build artifact: $f  (run lua/build.sh)" >&2; exit 1; }
done

targets=()
if [ $# -gt 0 ]; then
    for addon in "$@"; do
        target="$addons_dir/$addon/libs"
        [ -d "$target" ] || { echo "no such addon libs folder: $target" >&2; exit 1; }
        targets+=("$target")
    done
else
    # Every addon already carrying the kit. Deploying to only some of them
    # leaves mixed daemons in one client, where whichever pins first wins and
    # the others refuse to draw until the client restarts.
    while IFS= read -r dll; do
        targets+=("$(dirname "$dll")")
    done < <(find "$addons_dir" -mindepth 3 -maxdepth 3 -path '*/libs/worlddraw.dll' | sort)
fi

[ ${#targets[@]} -gt 0 ] || { echo "no addons carry worlddraw.dll; name one explicitly" >&2; exit 1; }

# Stage everything first, verify every staged byte, and only then rename. A
# failure part way through leaves nothing but .new files behind.
staged=()
cleanup() { for s in "${staged[@]:-}"; do rm -f "$s"; done; }
trap cleanup EXIT

for target in "${targets[@]}"; do
    for f in "${files[@]}"; do
        name=$(basename "$f")
        cp "$f" "$target/$name.new"
        staged+=("$target/$name.new")
        cmp "$f" "$target/$name.new"
    done
done

for target in "${targets[@]}"; do
    for f in "${files[@]}"; do
        name=$(basename "$f")
        mv "$target/$name.new" "$target/$name"
    done
done
staged=()
trap - EXIT

echo "deployed to ${#targets[@]} addon(s):"
for target in "${targets[@]}"; do
    echo "  ${target#$addons_dir/}"
done
md5sum worlddraw.dll worlddraw.lua ../daemon/worlddraw_daemon.dll | sed 's/^/  /'
echo "reload an addon to pick up the engine; the daemon needs a client restart"
