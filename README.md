# ffxi_world_draw

Enables drawing of 3D geometry inside Final Fantasy XI's world from a Windower 4
plugin. This renders in world space so it respects the depth buffer.

Single header. Include it, override one function, emit triangles.

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

Drop `ffxi_world_draw.h` and `WindowerPlugin.h` next to your source, build a
32-bit DLL, put it in Windower's `plugins` folder, and `//load myplugin`.

A complete working plugin is in [`example/`](example/), with a build script.

## Building

The plugin must be **32-bit**. On Linux:

```sh
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ \
    -o myplugin.dll myplugin.cpp exports.def
```

With `exports.def`:

```
LIBRARY "myplugin"
EXPORTS
    GetInterfaceVersion
    CreateInstance
```

On Windows, build a 32-bit DLL with MSVC. Nothing beyond the Windows and
Direct3D 8 SDK headers is required.

## Coordinates and units

Positions use the same convention as Windower's Lua API, so anything you read
from `windower.ffxi.get_mob_by_target('me')` can be passed straight in.

| axis | meaning |
|---|---|
| `x` | east / west |
| `y` | north / south |
| `z` | height — **negative is up** |

Distances are **yalms**. A Tarutaru is roughly 2 yalms tall, player collision is about 1 yalm.

```cpp
draw.Pillar(x, y, z, 0.15f, 3.0f, color);   // stands on z, rises 3 yalms
draw.Ring(x, y, z - 0.5f, 50.0f, 0.25f, c);  // ring floating half a yalm up
```

## Colors

`DWORD` in `0xAARRGGBB` order — alpha, red, green, blue.

```cpp
0xFF00FFFF   // opaque cyan
0x80FF0000   // half-transparent red
0xFFFFAA00   // opaque amber
```

Alpha blends against whatever is behind it. Colors are interpolated across a
triangle, so giving vertices different colors produces a gradient.

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
drawing with an invalid one simply draws nothing.

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
// facing you give it, so you can walk around it. Facing is in radians --
// 0 looks east (+x), increasing counter-clockwise.
draw.Panel(x, y, z, 2.0f, 2.0f, 0.0f, tex);

// Or place the corners yourself; set u/v on each vertex when projecting.
draw.Project(x, y, z, 0xFFFFFFFF, corner, /*u*/ 0.0f, /*v*/ 1.0f);
draw.TexturedQuad(a, b, c, d, tex);
```

Vertex color multiplies the image: `0xFFFFFFFF` leaves it alone, `0x80FFFFFF`
makes it half transparent, and a color tints it. That is how you fade or
color a sprite without touching the image.

`Sprite` and `ScreenSprite` turn to face the camera; `Panel` does not. All
three are centred on the point you give them.

`SetTexture(id)` binds one for everything drawn after it, `SetTexture(0)` goes
back to untextured. `ReleaseTexture(id)` frees one early; everything is freed
for you when the plugin unloads. `TextureSize(id, w, h)` reads the dimensions.

Textures are single-level, so a texture seen edge-on or far away may shimmer
slightly as you move. You should set a distance limit for what you're rendering.

## Per-frame work

`OnWorldDraw` runs on the render thread. Keep it to geometry — no file reading,
no network, no blocking. Do that work in `OnFrame`, which runs once per frame
away from drawing, and store what `OnWorldDraw` needs.

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
| `OnError(const char*)` | something went wrong; does nothing unless you override it |

`SetDrawEnabled(false)` stops drawing without unloading; `DrawEnabled()` reads
it back. Drawing is on by default.

## License

0BSD. Do whatever you want with it; no credit needed. See [LICENSE](LICENSE).
