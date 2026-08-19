worlddraw.dll goes here.

Build it with ../../build.sh, then copy it into this folder while the addon
is UNLOADED in game. Overwriting it while the addon is loaded will crash the
client: Lua maps the DLL into the process, and writing over a mapped image
through a Linux filesystem succeeds silently and corrupts it.
