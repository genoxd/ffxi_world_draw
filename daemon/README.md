# worlddraw_daemon

The part of worlddraw that owns the Direct3D hooks.

FFXI's device is hooked by patching six entries in its vtable. Overlays such as
Discord or ReShade patch the same entries, and they chain: whoever hooks second
calls whoever hooked first. That means an image holding a hook can never safely
unload, because someone else may still be calling into it.

So the hooks live here, in a small DLL that loads once and stays for the life of
the client. The engine (`worlddraw.dll`) registers with it and can be replaced
freely; the daemon never unloads.

Addon authors do not use this directly -- see the main README. This file is for
someone working on worlddraw itself.

## Using it

One export:

```c
const WdDaemonApi* __stdcall wd_daemon_acquire(uint32_t min_abi);
```

Call it after `LoadLibrary`, never from `DllMain`. It returns NULL if the
resident daemon is older than `min_abi`, or if setup failed. The returned
struct registers a handler set, asks for the hooks to be installed, and reports
whether anything has displaced them.

Handlers are observers: the daemon calls them, then forwards to the real method
itself. They must not call into Lua, allocate, block, or take the loader lock.

## Building

```sh
bash daemon/build.sh                  # builds worlddraw_daemon.dll
bash tools/gen_slots/build.sh         # checks the vtable indices against d3d8.h
bash tools/daemon_harness/build.sh    # the test suite
```

The build fails on a static initialiser, an exit-time destructor, or a second
export. The vtable indices are generated from the SDK header, never hand-typed;
`gen_slots generate` rewrites them.

## Shipping it

`worlddraw_daemon.dll` ships beside `worlddraw.dll` in an addon's `libs/`
folder. Several addons may each carry a copy: the first to load wins and the
rest use it.

Replacing it needs the client closed -- it is mapped for the whole session, so
unloading an addon is not enough. The engine has no such constraint.
