# example — the C++ plugin path

A complete, minimal Windower 4 plugin built on `ffxi_world_draw.h`: it draws a
camera-facing marker, a ring, a solid depth-writing cube, and the same image as
a world-sized sprite, a fixed-pixel sprite and a fixed-facing panel. `//example
hide` and `//example show` toggle drawing. The whole plugin is one `.cpp` file
and one override, `OnWorldDraw`.

**It is already built.** `example.dll` and `worlddraw_daemon.dll` are committed
in this folder. Copy **both** into `<windower>/plugins/`, put `happy_dog.jpg`
beside them if you want the picture rather than the built-in checker fallback,
and `//load example`. No compiler needed.

There are two files because the daemon owns the six device vtable slots and the
plugin registers observer handlers with it. That is also why this plugin can
sit in the same client as worlddraw Lua addons: exactly one image in the
process hooks the device, and it is the daemon.

```sh
bash example/build.sh     # builds the daemon, then the plugin, and stages both
```

See the [repository README](../README.md) for the full plugin API, and
[`daemon/README.md`](../daemon/README.md) for what the daemon is doing
underneath.
