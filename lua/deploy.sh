#!/usr/bin/env bash
# Copies the built module into an addon's libs/ folder.
#
# ONLY run this while the addon is UNLOADED. A DLL that Lua has require'd is
# mapped into the client exactly like a plugin is; writing over it through the
# Linux filesystem succeeds silently and corrupts the running image. Windows
# would refuse the write -- we get no such protection here.
#
#   //lua unload <addon>     then    lua/deploy.sh <addon>     then    //lua load <addon>
set -euo pipefail
cd "$(dirname "$0")"

addon=${1:-}
if [ -z "$addon" ]; then
    echo "usage: deploy.sh <addon-name>   (addon must be UNLOADED first)" >&2
    exit 2
fi

target="../../addons/$addon/libs"
[ -d "$target" ] || { echo "no such addon libs folder: $target" >&2; exit 1; }

read -r -p "Is '$addon' unloaded in game? Copying onto a loaded DLL crashes the client. [y/N] " reply
case "$reply" in
    y|Y) ;;
    *) echo "aborted"; exit 1 ;;
esac

cp worlddraw.dll "$target/worlddraw.dll"
cmp worlddraw.dll "$target/worlddraw.dll"
echo "deployed to $addon: $(md5sum "$target/worlddraw.dll" | cut -d' ' -f1)"
