# ffxi_world_draw

Draw your own 3D geometry inside Final Fantasy XI's world, from Windower 4.

It renders in world space, so what you draw is *in* the world: it respects the
depth buffer, hides behind terrain, and moves with the camera the way anything
else in the scene does. Rings on the ground, markers over mobs, a path drawn
along the floor, a model standing in a field.

There are two ways in, and **you only need to read one of them**:

| you are writing | read |
|---|---|
| a **Lua addon** | [Lua addons](#lua-addons) — copy three files in, `require`, describe geometry |
| a **C++ plugin** | [C++ plugins](#c-plugins) — include one header, override one function |

## Coordinates, units and colors

The same for both paths.

Positions use Windower's convention, so anything you read from
`windower.ffxi.get_mob_by_target('me')` can be passed straight in.

| axis | meaning |
|---|---|
| `x` | east / west |
| `y` | north / south |
| `z` | height — **negative is up** |

Distances are **yalms**. A Tarutaru is roughly 2 yalms tall; player collision
is about 1 yalm. `facing` is in radians in the ground plane: 0 looks east
(`+x`), increasing counter-clockwise.

Colors are `0xAARRGGBB` — alpha, red, green, blue:

```
0xFF00FFFF   opaque cyan
0x80FF0000   half-transparent red
0xFFFFAA00   opaque amber
```

Alpha blends against whatever is behind it. Color is interpolated across a
triangle, so giving vertices different colors gives you a gradient. On a
texture, the color multiplies the image: `0xFFFFFFFF` leaves it alone,
`0x80FFFFFF` makes it half transparent, and anything else tints it. Where a
color is optional it defaults to `0xFFFFFFFF`.

---

# Lua addons

No C++ to write and nothing to compile. Your addon includes one file and
describes what it wants drawn.

## Install

Three files go in your addon's own `libs/` folder. They are prebuilt in this
repository — take them from
[`lua/worlddrawdemo/libs/`](lua/worlddrawdemo/libs) and copy them as they are:

```
<windower>/addons/myaddon/
├── myaddon.lua
└── libs/
    ├── worlddraw.lua              the file you require
    ├── worlddraw.dll              the engine
    └── worlddraw_daemon.dll       the hook daemon
```

All three, together, in **your** addon's `libs/` — not in Windower's shared
`addons/libs/`. `worlddraw.lua` finds `worlddraw.dll`
beside itself, and the engine finds the daemon beside *it*.

Several addons each shipping their own copy is handled and not a
conflict: see [How it works](#how-it-works).

Then, in your addon:

```lua
local wd = require('libs.worlddraw')
local d  = wd.new('myaddon')
```

`wd.new(name)` takes a name used only in diagnostics. The handle owns its own
geometry, meshes and textures, so two addons drawing at once cannot touch each
other's.

## Drawing something

```lua
local wd = require('libs.worlddraw')
local d  = wd.new('myaddon')

d:begin()
d:ring(x, y, z, 10.0, 0.25, 0xFFFFAA00)
d:pillar(x, y, z, 0.15, 3.0, 0xFF00FFFF)
d:commit()
```

That is the whole thing. `commit()` publishes the description and it is drawn
every frame until you replace it.

## What you get

On the module:

| call | does |
|---|---|
| `wd.new(name)` | a new handle |
| `wd.version()` | version string, e.g. `worlddraw 0.3, daemon abi 1 build 1.1.0` — paste it into a bug report |

Shapes, all on the handle, all between a `begin()` and a `commit()`:

| call | draws |
|---|---|
| `d:pillar(x, y, z, width, height [, color])` | an upright bar, facing the camera |
| `d:ring(x, y, z, radius, thickness [, color [, segments]])` | a horizontal circle drawn as an upright band |
| `d:line(x1,y1,z1, x2,y2,z2, width [, color])` | a bar between two points, facing the camera |
| `d:panel(x, y, z, width, height, facing, texture [, color])` | a picture with a fixed facing |
| `d:sprite(x, y, z, width, height, texture [, color])` | a picture that turns to face the camera |
| `d:triangle(x1,y1,z1, x2,y2,z2, x3,y3,z3 [, color])` | one triangle |

`ring` defaults to 128 segments; pass more for a smoother circle.

And the rest of the handle:

| call | does |
|---|---|
| `d:begin()` | start describing |
| `d:commit()` | publish it; drawn every frame until replaced |
| `d:clear()` | draw nothing |
| `d:mesh()` | a new mesh — see [Meshes](#meshes) |
| `d:load_texture(path)` | a texture id, or nil |
| `d:last_error()` | why the last call failed, or nil |
| `d:player_draw_position()` | where your model is drawn: x, y, z — or nil |
| `d:close()` | release early; unload is already handled for you |

## The description is kept

`commit()` publishes a description that is drawn every frame until you replace
it, so **static geometry costs nothing per frame** — describe it once, at
load, and forget it. Rebuild only what moves:

```lua
windower.register_event('prerender', function()
    local x, y, z = d:player_draw_position()
    if x then
        d:begin()
        d:ring(x, y, z, 50.0, 0.25, 0xFFFFAA00)
        d:commit()
    end
end)
```

Anchor to `d:player_draw_position()` if you need it to move with you rather than to
`windower.ffxi.get_mob_by_target('me')`. The latter is the *logical* position,
which runs about 0.6 yalms ahead of your on-screen model while you move, so
anything placed with it sits visibly in front of you. It returns nil when it
cannot read the position, so keep `get_mob_by_target` as the fallback.

## Meshes

Re-describing a model every frame is exactly what this exists to avoid.
Describe it once in model space, build it, and then move it with one call a
frame:

```lua
local m = d:mesh()
m:tri(-0.5, -0.5, 0.0,   0, 0,      -- x, y, z, u, v per corner
       0.5, -0.5, 0.0,   0, 0,
       0.0,  0.0, -1.0,  0, 0,   0xFFFF4040)
m:build()

windower.register_event('prerender', function()
    m:at(x, y, z, facing, scale)
end)
```

| call | does |
|---|---|
| `m:tri(x1,y1,z1,u1,v1, x2,y2,z2,u2,v2, x3,y3,z3,u3,v3 [, color])` | one triangle, in model space |
| `m:mark(x, y, z, width, height [, color])` | a camera-facing marker inside the mesh (two triangles) |
| `m:vertices(packed [, color])` | a whole mesh in one crossing |
| `m:texture(id)` | put a loaded texture on the mesh |
| `m:build()` | freeze it and upload it |
| `m:at(x, y, z [, facing [, scale]])` | place it, and make it visible |
| `m:show(bool)` | hide or show it |
| `m:free()` | release it |

Coordinates in `tri` and `mark` are model space, origin-centred; `at()`
supplies the world placement. Moving 500 triangles costs one call, not 500.
Markers and triangles mix freely in one mesh, and a marker keeps facing the
camera even inside a rotated mesh.

`m:build()` freezes the mesh, so describe it fully first — staging after
`build()`, or `at()` before it, raises a Lua error. There is no limit on
vertices per mesh or meshes per handle; both grow as you use them.

Meshes draw with depth writing on and back-face culling off, so a closed model
hides its own far faces and a two-sided one works without a winding rule. They
draw before the flat overlays, so solids and overlays occlude each other
correctly in both directions.

`m:vertices(packed [, color])` uploads a whole mesh in one crossing, for when
per-triangle calls are the wrong shape: little-endian float32, 15 per triangle
— x, y, z, u, v for each of the three corners, 60 bytes. The optional color
applies to every vertex in the call.

`m:texture(id)` puts a loaded texture on the whole mesh, with the u, v pair on
each vertex choosing the part of the image that corner takes. Marker quads
carry uv 0, so they draw untextured even in a textured mesh.

## What the library does for you

- **It closes its handles when your addon unloads.**
- **It ticks itself every frame.**
- **It says so when it cannot draw.** The engine records why and stays quiet;
  `worlddraw.lua` polls and puts player-facing failures in chat, prefixed with
  the name you passed to `wd.new`.

Nothing is printed while things work. `d:close()` is still there if you want
to release something early, and closing one handle leaves every other addon's
untouched.

## When something is wrong

Four messages reach the player..

```
worlddraw can't draw: a file is missing.
Copy this addon's folder again from where you downloaded it.
Missing: <the exact path it looked at>
```

```
worlddraw can't draw: an older copy of its shared file is loaded.
Copy this addon's libs\worlddraw_daemon.dll over the one at:
  <the addon that won the election>
Then restart FFXI.
```

```
worlddraw can't draw: it failed to start.
Restart FFXI. If it happens again, please report it.
```

```
worlddraw stopped drawing: another program took over the graphics.
Restart FFXI, and load your addons before starting overlays like Discord or
ReShade.
```

A `details:` line may follow the third one, carrying the technical string. It
is printed on purpose: it is what makes a bug report useful, so ask for it
along with `wd.version()`.

An addon whose library cannot draw still **runs**. The handle is returned, the
calls succeed, nothing is drawn, and the reason is on screen.

## The example addon

[`lua/worlddrawdemo`](lua/worlddrawdemo) is a complete, ready-to-run addon: a
ring, a post, spokes, a textured picture and a mesh that spins in place, all
occluded by the world.

**Copy the folder into `Windower4/addons/` and load it.** Its `libs/` is
already populated — there is nothing to build and nothing else to copy.

```
//lua load worlddrawdemo
//wdd here      place it where you are standing
//wdd follow    make it follow you
//wdd spin      turn the mesh in place
//wdd clear     remove it
```

Read [`worlddrawdemo.lua`](lua/worlddrawdemo/worlddrawdemo.lua) next: it is
about 170 lines, and it is the whole Lua surface in use.

### Updating the DLLs later

`lua/deploy.sh` is this repository's own tool for that. It builds nothing, and
it deploys to every addon under `../../addons` that already carries
`libs/worlddraw.dll`, staging each file, verifying every staged byte with
`cmp`, and only then renaming them all:

```sh
bash lua/build.sh                 # rebuild engine + daemon
bash lua/deploy.sh                # every addon carrying the kit
bash lua/deploy.sh worlddrawdemo  # or just the ones you name
```

Reloading the addon picks up a new engine. A new **daemon** needs the client
restarted — see [How it works](#how-it-works).

---

# C++ plugins

A Windower 4 plugin, in one header. Include it, override one function, emit
geometry.

## Install the prebuilt example

[`example/`](example/) is built and committed. Copy **both DLLs** into
`<windower>/plugins/`, with `happy_dog.jpg` beside them, and `//load example`:

```
<windower>/plugins/
├── example.dll
├── worlddraw_daemon.dll
└── happy_dog.jpg
```

Two files: the plugin, and the daemon it draws through — see
[Which files ship](#which-files-ship) below.

## Quick start

```cpp
#include "ffxi_world_draw.h"

class MyPlugin final : public ffxi::WorldDrawPlugin {
public:
    const char* __stdcall GetPluginName() override   { return "myplugin"; }
    const char* __stdcall GetPluginAuthor() override { return "you"; }

private:
    void OnWorldDraw(ffxi::WorldDraw& draw) override {
        // A 3 yalm tall marker at a spot in the world.
        draw.Pillar(-118.0f, 266.0f, 0.9f, 0.15f, 3.0f, 0xFF00FFFF);
    }
};

std::uint32_t GetInterfaceVersion() { return WINDOWER_INTERFACE_VERSION; }
PluginBase* CreateInstance()        { return new MyPlugin(); }
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
```

With `exports.def`:

```
LIBRARY "myplugin"
EXPORTS
    GetInterfaceVersion
    CreateInstance
```

## Building

The plugin must be **32-bit**. Nothing beyond the Windows and Direct3D 8 SDK
headers is required; on Windows, build a 32-bit DLL with MSVC.

Drop `ffxi_world_draw.h`, `WindowerPlugin.h` and `daemon/worlddraw_abi.h` —
keeping the `daemon/` folder name, the header includes it by that path — next
to your source:

```sh
i686-w64-mingw32-g++ -std=c++17 -O2 \
    -shared -static -static-libgcc -static-libstdc++ \
    -o myplugin.dll myplugin.cpp exports.def
```

Add `-lgdiplus` if you use `LoadTexture` (see [Textures](#textures)).

## Which files ship

Two, both in `<windower>/plugins/`:

```
myplugin.dll
worlddraw_daemon.dll        the prebuilt copy from daemon/
```

The daemon owns the six device vtable slots; your plugin registers observer
handlers with it. The hooking image has to outlive your plugin, which is why
it is a separate file: foreign overlays chain through those same slots, saving
whatever address they find there, and an image whose address one of them saved
can never unmap without killing the client. The daemon takes that on and stays
small; your plugin stays replaceable behind it.
[How it works](#how-it-works) is the longer account.

The daemon is looked for **beside your DLL**, by that exact name. There is no
search path and no fallback: without it the plugin loads, records the failure,
and draws nothing. `example/build.sh` stages the daemon beside its output so
both files are ready to copy.

## Drawing

`OnWorldDraw` is called once per frame. Everything is triangles; the helpers
build them for you.

```cpp
void OnWorldDraw(ffxi::WorldDraw& draw) override {
    // A vertical bar, always turned to face the camera.
    draw.Pillar(x, y, z, width, height, color);

    // A horizontal circle, drawn as an upright band `thickness` yalms tall.
    draw.Ring(x, y, z, radius, thickness, color);
    draw.Ring(x, y, z, radius, thickness, color, 256);   // smoother

    // A bar between two points, facing the camera.
    draw.Line(x1, y1, z1, x2, y2, z2, width, color);
}
```

`Ring`'s segment count defaults to 128 and is not capped: it is a count, and
the batch flushes as it fills, so a finer ring costs frames, never geometry.

For anything else, project world points yourself and emit the triangles:

```cpp
ffxi::Vertex a, b, c;
if (draw.Project(x1, y1, z1, 0xFFFF0000, a)
    && draw.Project(x2, y2, z2, 0xFF00FF00, b)
    && draw.Project(x3, y3, z3, 0xFF0000FF, c)) {
    draw.Triangle(a, b, c);
}
```

`Project` returns `false` when a point is behind the camera or far off screen.
Check it and skip that shape — a partly projected shape will look wrong.

`draw.Quad(a, b, c, d)` emits two triangles. `draw.CameraRightX()` and
`CameraRightY()` give the ground-plane direction to the camera's right, if you
want to build your own camera-facing shapes. `draw.Viewport()` gives the
viewport being drawn to.

There is no limit on how much you emit — geometry is sent in batches as you
go, so draw as many shapes as you need. `draw.Flush()` sends the pending batch
early, but it happens for you when a batch fills, when state changes, and at
the end of the frame, so you should not need it.

## Solid objects

By default shapes are flat overlays: they don't write depth and are visible
from both sides, which is what you want for rings, bars and markers. Solid
geometry needs both, or its own back faces paint over its front ones.

```cpp
draw.SetSolid(true);
// ... emit the triangles of your mesh ...
draw.SetSolid(false);
```

`SetSolid` is shorthand for `SetDepthWrite(true)` and `SetCulling(true)`; both
are available separately. Faces are culled counter-clockwise, so wind your
triangles so the outside is what you want to see.

Only solid geometry writes depth, so if solid objects and flat overlays
overlap, emit the solid ones first.

## Textures

Two ways to get a texture. Both give you a `TextureId`; `0` means none, and
drawing with an invalid one simply draws nothing. The texture pool grows on
demand — there is no maximum.

```cpp
// Pixels you already have: 32-bit BGRA, top row first. No dependencies.
TextureId tex = CreateTexture(pixels, width, height);
```

```cpp
// Or load an image file. Requires the switch and the library:
//   #define FFXI_WORLD_DRAW_IMAGE_LOADING   (before the include)
//   ... -lgdiplus                           (when linking)
TextureId tex = LoadTexture("plugins/settings/myplugin/icon.png");
```

`LoadTexture` handles png, jpg, bmp, gif and tiff. Load textures in `OnLoad`
or `OnFrame` — never while drawing, since decoding an image is slow.

Drawing with one:

```cpp
// A square that turns to face you, sized in yalms, so it shrinks with distance.
draw.Sprite(x, y, z, 1.0f, 1.0f, tex);

// The same, sized in pixels, so it stays readable however far away it is.
draw.ScreenSprite(x, y, z, 48.0f, 48.0f, tex);

// A standing rectangle that does NOT turn with the camera: it keeps the
// facing you give it, so you can walk around it.
draw.Panel(x, y, z, 2.0f, 2.0f, 0.0f, tex);

// Or place the corners yourself; set u/v on each vertex when projecting.
draw.Project(x, y, z, 0xFFFFFFFF, corner, /*u*/ 0.0f, /*v*/ 1.0f);
draw.TexturedQuad(a, b, c, d, tex);
```

`Sprite` and `ScreenSprite` turn to face the camera; `Panel` does not. All
three are centred on the point you give them.

`SetTexture(id)` binds one for everything drawn after it, `SetTexture(0)` goes
back to untextured. `ReleaseTexture(id)` frees one early; everything is freed
for you when the plugin unloads. `TextureSize(id, w, h)` reads the dimensions.

Textures are single-level, so a texture seen edge-on or far away may shimmer
slightly as you move. You should set a distance limit for what you're rendering.

## Per-frame work

`OnWorldDraw` runs on the render thread; `OnFrame` runs where Windower
dispatches from. These are two different OS threads, so state you share
between them needs your own locking. Keep `OnWorldDraw` to geometry: no file
reading, no network, no blocking. Do that work in `OnFrame`, which runs once
per frame away from drawing, and store what `OnWorldDraw` needs.

```cpp
void OnFrame() override {
    positions_ = ReadMyPositionsFromSomewhere();
}

void OnWorldDraw(ffxi::WorldDraw& draw) override {
    for (const auto& p : positions_) {
        draw.Pillar(p.x, p.y, p.z, 0.15f, 3.0f, 0xFF00FFFF);
    }
}
```

Other overrides available:

| method | when |
|---|---|
| `OnLoad()` | plugin loaded |
| `OnUnload()` | plugin unloading |
| `OnFrame()` | once per frame, before drawing |
| `OnError(const char*)` | something went wrong |

**`OnError` does nothing unless you override it.** The Lua front-end prints
these for the player; a plugin has no such front-end, so a plugin that does
not override `OnError` will fail silently — including the case where the
daemon is missing beside it. Override it and put the message somewhere you
will see:

```cpp
void OnError(const char* message) override {
    // Lines beginning "worlddraw " are for the player; everything else is
    // an engineering string ("texture: ", "gpu: ", "hook: ", "draw: ",
    // "scan: ") and is worth logging but not showing.
    std::fprintf(stderr, "[myplugin] %s\n", message);
}
```

`SetDrawEnabled(false)` stops drawing without unloading; `DrawEnabled()` reads
it back. Drawing is on by default. `DaemonAbi()` and `DaemonBuild()` report
the resident daemon for a support string.

## The example plugin

[`example/`](example/) is the reference: a marker, a ring, a solid
depth-writing cube, and one image drawn three ways. One `.cpp` file, one
override, a build script, and a prebuilt DLL so you can see it working before
you compile anything. See
[`example/README.md`](example/README.md).

---

# How it works

You do not need this to use the library.

worlddraw ships as two DLLs. `worlddraw_daemon.dll` owns the Direct3D hooks and
stays loaded for the whole session, because overlays like Discord and ReShade
hook the same place and chain through each other -- an image holding a hook
cannot safely unload. `worlddraw.dll` does the drawing and registers with it,
so it can be replaced by reloading an addon.

Geometry you describe is uploaded to the GPU and drawn from there, so a
description costs nothing per frame once committed and nothing is capped at a
fixed size.

## Repository layout

```
ffxi_world_draw.h        the C++ library header
WindowerPlugin.h         Windower's plugin interface
daemon/                  the hook daemon, its ABI, and its own README
lua/                     the engine (worlddraw.cpp) and worlddraw.lua
lua/worlddrawdemo/       the example addon, ready to copy and load
example/                 the example plugin, prebuilt
tools/                   offline harnesses and generators (see below)
```

The DLLs in this tree are committed deliberately: installing an addon or a
plugin should never require a compiler. To rebuild them:

```sh
bash lua/build.sh                     # daemon + engine, with their build gates
bash example/build.sh                 # the example plugin and its daemon
bash tools/gen_slots/build.sh         # prove the vtable indices against d3d8.h
bash tools/daemon_harness/build.sh    # the daemon across real DLL boundaries
bash tools/offline_harness/build.sh   # the engine's CPU half
bash tools/verify_shader/build.sh     # the embedded shader tokens
```

## License

0BSD. Do whatever you want with it; no credit needed. See [LICENSE](LICENSE).
