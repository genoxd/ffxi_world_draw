#pragma once
//
// ffxi_world_draw - draw your own 3D geometry inside Final Fantasy XI's world
// from a Windower 4 plugin.
//
// Derive from ffxi::WorldDrawPlugin, override OnWorldDraw, and emit geometry in
// world coordinates. See README.md. Licensed 0BSD; see LICENSE.
//
#include "WindowerPlugin.h"

// Define FFXI_WORLD_DRAW_IMAGE_LOADING before including this header to get
// LoadTexture(path) for png/jpg/bmp/gif/tiff. It needs GDI+, so link gdiplus:
//   ... -o myplugin.dll myplugin.cpp exports.def -lgdiplus
// Without it the header pulls in no extra dependencies and you supply pixels
// to CreateTexture yourself.
#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
#include <objidl.h>
#include <gdiplus.h>

// Behind macro names so the offline harness can replace and count them; a
// shipping build reaches GDI+ directly. Startup and shutdown are a pair.
#ifndef FFXI_WORLD_DRAW_GDIPLUS_STARTUP
#define FFXI_WORLD_DRAW_GDIPLUS_STARTUP(token, input) \
    Gdiplus::GdiplusStartup((token), (input), nullptr)
#endif
#ifndef FFXI_WORLD_DRAW_GDIPLUS_SHUTDOWN
#define FFXI_WORLD_DRAW_GDIPLUS_SHUTDOWN(token) Gdiplus::GdiplusShutdown(token)
#endif
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

// Drawing goes through worlddraw_daemon.dll, which owns the six device vtable
// slots for the life of the process and calls the four handlers this image
// registers. The daemon must sit beside this image; there is no fallback and
// nothing to configure.
#include "daemon/worlddraw_abi.h"

// Behind macro names so the offline harness can replace and count them; a
// shipping build defines none of these and reaches Win32 directly.
// FFXI_WORLD_DRAW_FREE_LIBRARY must stay unreached -- see take_self_reference
// -- and the seam exists so the harness can prove the call is never made.
#ifndef FFXI_WORLD_DRAW_LOAD_LIBRARY_W
#define FFXI_WORLD_DRAW_LOAD_LIBRARY_W(path) ::LoadLibraryW(path)
#endif
#ifndef FFXI_WORLD_DRAW_FREE_LIBRARY
#define FFXI_WORLD_DRAW_FREE_LIBRARY(module) ::FreeLibrary(module)
#endif
#ifndef FFXI_WORLD_DRAW_GET_PROC_ADDRESS
#define FFXI_WORLD_DRAW_GET_PROC_ADDRESS(module, name) ::GetProcAddress(module, name)
#endif

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

namespace ffxi {

#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
// Records which thread each D3D hook site is entered on. Diagnostic builds
// only; the shipping image never defines this macro.
//
// record() runs inside the render path: it must not allocate, open anything or
// take a lock. A row is never released, which is what makes reading the table
// without a lock safe. Whatever it measures licenses no removal of
// synchronization -- one machine on one day is not the tools a player loads.
namespace thread_probe {

enum Site : LONG {
    site_pre_reset = 1,
    site_post_reset = 2,
    site_pre_set_render_target = 4,
    site_pre_draw = 8,
    site_lua_tick = 16,
};

struct SiteName {
    LONG bit;
    const char* name;
};

constexpr SiteName site_names_[] = {
    {site_pre_reset, "pre_reset"},
    {site_post_reset, "post_reset"},
    {site_pre_set_render_target, "pre_set_render_target"},
    {site_pre_draw, "pre_draw"},
    {site_lua_tick, "lua_tick"},
};

// Everything the daemon calls from the render path.
constexpr LONG hook_sites_ = site_pre_reset | site_post_reset
    | site_pre_set_render_target | site_pre_draw;

// Rows in the table below. Fixed because record() may not allocate. A sample
// that finds no free row is counted, and the report turns that into an
// inconclusive verdict rather than a quiet wrong answer.
constexpr int max_threads_ = 8;

struct Observation {
    volatile LONG id;      // thread id; zero means the row is free
    volatile LONG count;
    volatile LONG sites;
};

// Zero-initialised static storage, no constructor and no destructor: nothing
// here may run at image load or at process exit.
inline Observation observations_[max_threads_] {};
inline volatile LONG unfitted_ = 0;

inline void record(LONG site) {
    const LONG id = static_cast<LONG>(::GetCurrentThreadId());

    for (int i = 0; i < max_threads_; ++i) {
        Observation& row = observations_[i];
        if (row.id != id) {
            // Compare-exchange rather than a store, so two threads arriving
            // at once cannot share a row; losing the race moves to the next.
            if (row.id != 0 || ::InterlockedCompareExchange(&row.id, id, 0) != 0) {
                continue;
            }
        }

        ::InterlockedIncrement(&row.count);
        if ((row.sites & site) == 0) {
            ::InterlockedOr(&row.sites, site);
        }
        return;
    }

    ::InterlockedIncrement(&unfitted_);
}

inline void append(char* out, std::size_t size, const char* text) {
    const std::size_t used = std::strlen(out);
    if (used + 1 < size) {
        std::snprintf(out + used, size - used, "%s", text);
    }
}

// Called from the Lua thread, never from a handler.
inline void report(char* out, std::size_t size) {
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';

    char line[256] {};
    LONG lua_id = 0;
    int lua_threads = 0;
    int hook_threads = 0;
    int rows = 0;

    append(out, size, "worlddraw render-thread probe (diagnostic build; counts are since"
        " this image loaded)\n");

    for (int i = 0; i < max_threads_; ++i) {
        const LONG id = observations_[i].id;
        if (id == 0) {
            continue;
        }
        ++rows;

        const LONG sites = observations_[i].sites;
        if ((sites & site_lua_tick) != 0) {
            ++lua_threads;
            if (lua_id == 0) {
                lua_id = id;
            }
        }
        if ((sites & hook_sites_) != 0) {
            ++hook_threads;
        }

        const unsigned long samples = static_cast<unsigned long>(observations_[i].count);
        std::snprintf(line, sizeof(line), "  thread 0x%08lX  %lu sample%s  sites:",
            static_cast<unsigned long>(id), samples, samples == 1 ? "" : "s");
        append(out, size, line);

        for (std::size_t s = 0; s < sizeof(site_names_) / sizeof(site_names_[0]); ++s) {
            if ((sites & site_names_[s].bit) != 0) {
                append(out, size, " ");
                append(out, size, site_names_[s].name);
            }
        }
        append(out, size, "\n");
    }

    if (rows == 0) {
        append(out, size, "  (nothing recorded yet)\n");
    }

    std::snprintf(line, sizeof(line),
        "  samples that found no free row: %lu   (the table holds %d distinct threads)\n",
        static_cast<unsigned long>(unfitted_), max_threads_);
    append(out, size, line);

    if (lua_threads == 0) {
        append(out, size, "  lua tick thread: none seen yet\n");
    } else {
        std::snprintf(line, sizeof(line), "  lua tick thread: 0x%08lX%s\n",
            static_cast<unsigned long>(lua_id),
            lua_threads > 1 ? "  (AND OTHERS -- see the rows above)" : "");
        append(out, size, line);
    }

    // A row with hook bits and no tick bit, rather than a comparison against
    // one chosen id, which would be wrong the moment there were two of them.
    bool hooks_off_lua = false;
    for (int i = 0; i < max_threads_; ++i) {
        const LONG sites = observations_[i].sites;
        if ((sites & hook_sites_) != 0 && (sites & site_lua_tick) == 0) {
            hooks_off_lua = true;
        }
    }

    const char* verdict;
    if (lua_threads == 0) {
        verdict = "  VERDICT: inconclusive -- no lua tick sampled yet.\n";
    } else if (hook_threads == 0) {
        verdict = "  VERDICT: inconclusive -- no hook site sampled yet; the world has not"
            " drawn through the daemon.\n";
    } else if (hooks_off_lua) {
        verdict = "  VERDICT: NOT SINGLE-THREADED -- a hook site ran on a thread the lua tick"
            " never used. The hooks and Windower's Lua are NOT one thread here.\n";
    } else if (hook_threads > 1 || lua_threads > 1) {
        verdict = "  VERDICT: NOT SINGLE-THREADED -- the sites above span more than one"
            " thread.\n";
    } else if (unfitted_ != 0) {
        verdict = "  VERDICT: inconclusive -- the table overflowed, so a thread may have"
            " gone unrecorded.\n";
    } else {
        verdict = "  VERDICT: consistent with single-threaded -- every hook site ran on the"
            " lua tick thread.\n";
    }
    append(out, size, verdict);

    append(out, size, "  This measures THIS client on THIS machine and licenses no removal"
        " of synchronization.\n");
}

}  // namespace thread_probe
#endif  // FFXI_WORLD_DRAW_THREAD_PROBE

// A screen-space vertex carrying the depth that places it in the world.
// Make these with WorldDraw::Project, then pass them to Triangle or Quad.
struct Vertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rhw = 1.0f;
    DWORD color = 0xFFFFFFFF;
    float u = 0.0f;
    float v = 0.0f;
};

// A vertex for the shader-driven pipeline. Nothing in it is screen-space: the
// vertex shader places it, so the anchor stays in world (or model) coordinates.
//
//   x, y, z    the anchor, in whatever axes the matrix loaded into c0-c3 maps
//   color      diffuse, multiplied with the texture exactly as Vertex is
//   u, v       texcoord0
//   ox, oy     world-unit offsets along the ground-plane camera-right (c4) and
//              along up (c5), so a billboard turns with the camera without the
//              CPU rebuilding it
//   px, py     screen-pixel offsets applied after the projection, so a screen
//              sprite keeps its size however far away it is
//
// A plain triangle leaves all four offsets at zero. A plain aggregate with no
// member initialisers on purpose: producers fill every field.
//
// The line shader reads these same 40 bytes through its own declaration, where
// (ox, oy, px, py) are (half width, direction x, direction y, direction z).
// One vertex format, two declarations -- both shaders draw out of one buffer.
struct GpuVertex {
    float x, y, z;
    DWORD color;
    float u, v;
    float ox, oy;
    float px, py;
};

// The vertex declarations below describe exactly this layout to the device.
static_assert(sizeof(GpuVertex) == 40,
    "GpuVertex is float3 + D3DCOLOR + float2 + float4 = 40 bytes");
static_assert(offsetof(GpuVertex, x) == 0, "GpuVertex: anchor first");
static_assert(offsetof(GpuVertex, y) == 4, "GpuVertex: anchor packed");
static_assert(offsetof(GpuVertex, z) == 8, "GpuVertex: anchor packed");
static_assert(offsetof(GpuVertex, color) == 12, "GpuVertex: colour after the anchor");
static_assert(offsetof(GpuVertex, u) == 16, "GpuVertex: uv after the colour");
static_assert(offsetof(GpuVertex, v) == 20, "GpuVertex: uv packed");
static_assert(offsetof(GpuVertex, ox) == 24, "GpuVertex: world offsets after the uv");
static_assert(offsetof(GpuVertex, oy) == 28, "GpuVertex: world offsets packed");
static_assert(offsetof(GpuVertex, px) == 32, "GpuVertex: pixel offsets last");
static_assert(offsetof(GpuVertex, py) == 36, "GpuVertex: pixel offsets packed");

// Returned by CreateTexture and LoadTexture. Zero means "no texture": drawing
// with it is untextured, which is also the default.
typedef int TextureId;

class WorldDrawPlugin;

// Handed to OnWorldDraw. Anything emitted through it is placed in the world.
class WorldDraw {
public:
    // World point -> screen vertex. False when the point is behind the camera
    // or well off screen; skip whatever geometry needed it.
    bool Project(float x, float y, float z, DWORD color, Vertex& out,
                 float u = 0.0f, float v = 0.0f) const;

    void Triangle(const Vertex& a, const Vertex& b, const Vertex& c);
    void Quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d);

    // A vertical bar at (x, y, z) rising `height` yalms, `width` yalms wide,
    // turned to face the camera.
    bool Pillar(float x, float y, float z, float width, float height, DWORD color);

    // A horizontal circle centred on (x, y, z), drawn as an upright band
    // `thickness` yalms tall. `segments` is however many you ask for: it is a
    // count, so anything under one is raised to one, and nothing else is
    // capped -- the batch flushes as it fills, so a finer ring costs frames,
    // never geometry.
    bool Ring(float x, float y, float z, float radius, float thickness,
              DWORD color, int segments = 128);

    // A camera-facing bar between two world points, `width` yalms thick.
    bool Line(float x1, float y1, float z1, float x2, float y2, float z2,
              float width, DWORD color);

    // Ground-plane camera-right, if you want to build your own billboards.
    float CameraRightX() const { return right_x_; }
    float CameraRightY() const { return right_y_; }

    const D3DVIEWPORT8& Viewport() const { return viewport_; }

    // Draw with a texture. 0 goes back to untextured. Vertex color multiplies
    // the image, so 0xFFFFFFFF leaves it untouched and anything else tints or
    // fades it. Changing texture sends the pending batch.
    void SetTexture(TextureId id);

    // Solid geometry writes depth and hides its own back faces, so a closed
    // object looks right. Flat overlays (the default) do neither, so they stay
    // visible from both sides and never occlude each other.
    void SetSolid(bool solid);
    void SetDepthWrite(bool enabled);
    void SetCulling(bool enabled);

    // A camera-facing textured rectangle centred on the point, sized in yalms,
    // so it shrinks with distance like real geometry.
    bool Sprite(float x, float y, float z, float width, float height,
                TextureId texture, DWORD color = 0xFFFFFFFF);

    // The same, but sized in screen pixels: stays readable at any distance.
    bool ScreenSprite(float x, float y, float z, float width, float height,
                      TextureId texture, DWORD color = 0xFFFFFFFF);

    // A standing rectangle with a fixed facing: it does not turn with the
    // camera. Centred on the point, sized in yalms. `facing` is in radians and
    // is the direction the picture looks towards -- 0 faces east (+x) and the
    // angle increases counter-clockwise.
    bool Panel(float x, float y, float z, float width, float height, float facing,
               TextureId texture, DWORD color = 0xFFFFFFFF);

    // Four corners you projected yourself, with UVs already on the vertices.
    void TexturedQuad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d,
                      TextureId texture);

    // Send whatever has been emitted so far. Called for you when the batch
    // fills and again at the end of the frame; you never need to call it.
    void Flush();

private:
    friend class WorldDrawPlugin;
    WorldDrawPlugin* owner_ = nullptr;
    IDirect3DDevice8* device_ = nullptr;
    Vertex* buffer_ = nullptr;
    int capacity_ = 0;
    int count_ = 0;
    float right_x_ = 1.0f;
    float right_y_ = 0.0f;
    D3DVIEWPORT8 viewport_ {};
    TextureId texture_ = 0;
    bool depth_write_ = false;
    bool culling_ = false;
};

// The vertex declaration for GpuVertex on stream 0. A programmable shader binds
// by register number, so the names below say only what this library puts in
// each -- they are not fixed-function semantics, and the D3DVSDE_ constants
// would give other registers entirely.
inline constexpr DWORD gpu_shader_declaration[] = {
    D3DVSD_STREAM(0),
    D3DVSD_REG(0, D3DVSDT_FLOAT3),    // v0 <- x, y, z         (position)
    D3DVSD_REG(1, D3DVSDT_D3DCOLOR),  // v1 <- color           (diffuse)
    D3DVSD_REG(2, D3DVSDT_FLOAT2),    // v2 <- u, v            (texcoord0)
    D3DVSD_REG(3, D3DVSDT_FLOAT4),    // v3 <- ox, oy, px, py  (texcoord1)
    D3DVSD_END(),
};

// The vertex shader, as assembled tokens because D3D8 takes nothing else.
// tools/verify_shader assembles the listing below and byte-compares it with
// the tokens; every line of it must stay verbatim. The listing it encodes:
//
//     vs.1.1
//     ; r0 = anchor + up*oy; the width offset joins in clip space, below
//     mad r0.xyz, c5.xyz, v3.y, v0.xyz
//     mov r0.w, c7.x                      ; 1.0
//     dp4 r1.x, r0, c0                    ; transform: c0-c3 are the COLUMNS
//     dp4 r1.y, r0, c1                    ;   of the row-vector-convention
//     dp4 r1.z, r0, c2                    ;   matrix (v*M), see LoadGpuConstants
//     dp4 r1.w, r0, c3
//     mul r2, c9, v3.x                    ; the exact width offset, in clip
//     max r3.x, v3.x, -v3.x               ; |ox|
//     mul r3.x, r3.x, c11.x               ; its half width in pixels at w = 1
//     mul r3.y, c11.y, r1.w               ; the floor's half width, same units
//     sge r3.z, r3.x, r3.y                ; 1 when the exact width is enough
//     sge r4.x, v3.x, c7.y                ; which side of the quad this is
//     sge r4.y, -v3.x, c7.y
//     add r3.w, r4.x, -r4.y               ; +1, -1, and exactly 0 when ox is 0
//     mul r4.xy, c10.xy, r1.w             ; the floored offset, in clip units
//     mul r4.xy, r4.xy, r3.w              ; ... on this vertex's side
//     add r5.xy, r2.xy, -r4.xy
//     mad r2.xy, r5.xy, r3.z, r4.xy       ; pick one; z stays exact
//     mul r2.w, r2.w, r3.z                ; a floored quad keeps the anchor w
//     add r1, r1, r2
//     mul r5.xy, v3.zw, c6.xy             ; pixel offsets to clip scale
//     mad r1.xy, r5.xy, r1.w, r1.xy       ; perspective-correct screen shift
//     mov oPos, r1
//     mov oD0, v1
//     mov oT0, v2
//
// c7 holds (1, 0, 0, 0) from the CPU rather than a def token, so the constant
// block is one upload and the shader carries no data section.
//
// The width offset is applied after the transform, which is where its size on
// screen is known: that is what lets a quad narrower than min_projected_width_
// pixels be widened to exactly that. vs_1_1 has no branch, so the select is
// arithmetic. A floored quad keeps the anchor's w and drops the exact offset's,
// which is what makes the widening exact. A vertex with ox of 0 is never
// widened. Deliberate approximation: the direction the floor stretches along
// comes from c9.xy alone, not from this vertex's own perspective divide.
//
// The swizzles are the assembler's own -- a source read under a partial write
// mask repeats its last component -- which is why these tokens come from an
// assembler and are never written by eye.
// >>> BEGIN GENERATED gpu_shader_function -- do not hand-edit these tokens.
// They are what tools/verify_shader assembles the listing above into:
//   tools/verify_shader/build.sh generate   rewrites this span
//   tools/verify_shader/build.sh            proves it still matches
inline constexpr DWORD gpu_shader_function[] = {
    0xFFFE0101,  // vs.1.1
    // mad r0.xyz, c5.xyz, v3.y, v0.xyz
    0x00000004, 0x80070000, 0xA0A40005, 0x90550003, 0x90A40000,
    // mov r0.w, c7.x
    0x00000001, 0x80080000, 0xA0000007,
    // dp4 r1.x, r0, c0
    0x00000009, 0x80010001, 0x80E40000, 0xA0E40000,
    // dp4 r1.y, r0, c1
    0x00000009, 0x80020001, 0x80E40000, 0xA0E40001,
    // dp4 r1.z, r0, c2
    0x00000009, 0x80040001, 0x80E40000, 0xA0E40002,
    // dp4 r1.w, r0, c3
    0x00000009, 0x80080001, 0x80E40000, 0xA0E40003,
    // mul r2, c9, v3.x
    0x00000005, 0x800F0002, 0xA0E40009, 0x90000003,
    // max r3.x, v3.x, -v3.x
    0x0000000B, 0x80010003, 0x90000003, 0x91000003,
    // mul r3.x, r3.x, c11.x
    0x00000005, 0x80010003, 0x80000003, 0xA000000B,
    // mul r3.y, c11.y, r1.w
    0x00000005, 0x80020003, 0xA055000B, 0x80FF0001,
    // sge r3.z, r3.x, r3.y
    0x0000000D, 0x80040003, 0x80000003, 0x80550003,
    // sge r4.x, v3.x, c7.y
    0x0000000D, 0x80010004, 0x90000003, 0xA0550007,
    // sge r4.y, -v3.x, c7.y
    0x0000000D, 0x80020004, 0x91000003, 0xA0550007,
    // add r3.w, r4.x, -r4.y
    0x00000002, 0x80080003, 0x80000004, 0x81550004,
    // mul r4.xy, c10.xy, r1.w
    0x00000005, 0x80030004, 0xA054000A, 0x80FF0001,
    // mul r4.xy, r4.xy, r3.w
    0x00000005, 0x80030004, 0x80540004, 0x80FF0003,
    // add r5.xy, r2.xy, -r4.xy
    0x00000002, 0x80030005, 0x80540002, 0x81540004,
    // mad r2.xy, r5.xy, r3.z, r4.xy
    0x00000004, 0x80030002, 0x80540005, 0x80AA0003, 0x80540004,
    // mul r2.w, r2.w, r3.z
    0x00000005, 0x80080002, 0x80FF0002, 0x80AA0003,
    // add r1, r1, r2
    0x00000002, 0x800F0001, 0x80E40001, 0x80E40002,
    // mul r5.xy, v3.zw, c6.xy
    0x00000005, 0x80030005, 0x90FE0003, 0xA0540006,
    // mad r1.xy, r5.xy, r1.w, r1.xy
    0x00000004, 0x80030001, 0x80540005, 0x80FF0001, 0x80540001,
    // mov oPos, r1
    0x00000001, 0xC00F0000, 0x80E40001,
    // mov oD0, v1
    0x00000001, 0xD00F0000, 0x90E40001,
    // mov oT0, v2
    0x00000001, 0xE00F0000, 0x90E40002,
    0x0000FFFF,  // end
};
// <<< END GENERATED gpu_shader_function

// Which of the two shaders a draw runs. The vertex buffer is the same either
// way; what changes is how the last 16 bytes of each vertex are read.
enum GpuShader {
    GpuShaderBillboard = 0,
    GpuShaderLine = 1,
};

// The line shader's declaration over that same GpuVertex layout: the first 24
// bytes read as above, the last 16 as half width plus a unit direction. A D3D8
// declaration walks the stream in order, so this re-reads the same bytes and is
// not a second vertex format.
inline constexpr DWORD gpu_line_declaration[] = {
    D3DVSD_STREAM(0),
    D3DVSD_REG(0, D3DVSDT_FLOAT3),    // v0 <- x, y, z           (position)
    D3DVSD_REG(1, D3DVSDT_D3DCOLOR),  // v1 <- color             (diffuse)
    D3DVSD_REG(2, D3DVSDT_FLOAT2),    // v2 <- u, v              (texcoord0)
    D3DVSD_REG(3, D3DVSDT_FLOAT1),    // v3 <- ox = half width   (texcoord1)
    D3DVSD_REG(4, D3DVSDT_FLOAT3),    // v4 <- oy, px, py = unit direction
    D3DVSD_END(),                     //                         (texcoord2)
};

// The line shader, again as assembled tokens, verified the same way. The
// listing it encodes, verbatim:
//
//     vs.1.1
//     ; a camera-facing line: widen across the view direction and the line
//     mul r0.xyz, c8.yzx, v4.zxy          ; r0 = cross(view forward, direction)
//     mad r0.xyz, -c8.zxy, v4.yzx, r0.xyz
//     dp3 r1.x, r0, r0                    ; normalise it
//     rsq r1.x, r1.x
//     mul r0.xyz, r0.xyz, r1.x
//     mad r0.xyz, r0.xyz, v3.x, v0.xyz    ; anchor + width direction * v3.x
//     mov r0.w, c7.x                      ; 1.0
//     dp4 r1.x, r0, c0                    ; transform: c0-c3 are the COLUMNS
//     dp4 r1.y, r0, c1                    ;   of the row-vector-convention
//     dp4 r1.z, r0, c2                    ;   matrix (v*M), see LoadGpuConstants
//     dp4 r1.w, r0, c3
//     mov oPos, r1
//     mov oD0, v1
//     mov oT0, v2
//
// c8 is the camera forward in the same axes as the anchors. A line running
// straight at the camera crosses to zero and its vertices come out NaN --
// deliberately unguarded, because such a line is edge-on and covers no pixels
// either way. There is no pixel-offset mad here: this shader's v3 is one float
// and the other three are the direction.
// >>> BEGIN GENERATED gpu_line_function -- do not hand-edit these tokens.
// They are what tools/verify_shader assembles the listing above into:
//   tools/verify_shader/build.sh generate   rewrites this span
//   tools/verify_shader/build.sh            proves it still matches
inline constexpr DWORD gpu_line_function[] = {
    0xFFFE0101,  // vs.1.1
    // mul r0.xyz, c8.yzx, v4.zxy
    0x00000005, 0x80070000, 0xA0090008, 0x90520004,
    // mad r0.xyz, -c8.zxy, v4.yzx, r0.xyz
    0x00000004, 0x80070000, 0xA1520008, 0x90090004, 0x80A40000,
    // dp3 r1.x, r0, r0
    0x00000008, 0x80010001, 0x80E40000, 0x80E40000,
    // rsq r1.x, r1.x
    0x00000007, 0x80010001, 0x80000001,
    // mul r0.xyz, r0.xyz, r1.x
    0x00000005, 0x80070000, 0x80A40000, 0x80000001,
    // mad r0.xyz, r0.xyz, v3.x, v0.xyz
    0x00000004, 0x80070000, 0x80A40000, 0x90000003, 0x90A40000,
    // mov r0.w, c7.x
    0x00000001, 0x80080000, 0xA0000007,
    // dp4 r1.x, r0, c0
    0x00000009, 0x80010001, 0x80E40000, 0xA0E40000,
    // dp4 r1.y, r0, c1
    0x00000009, 0x80020001, 0x80E40000, 0xA0E40001,
    // dp4 r1.z, r0, c2
    0x00000009, 0x80040001, 0x80E40000, 0xA0E40002,
    // dp4 r1.w, r0, c3
    0x00000009, 0x80080001, 0x80E40000, 0xA0E40003,
    // mov oPos, r1
    0x00000001, 0xC00F0000, 0x80E40001,
    // mov oD0, v1
    0x00000001, 0xD00F0000, 0x90E40001,
    // mov oT0, v2
    0x00000001, 0xE00F0000, 0x90E40002,
    0x0000FFFF,  // end
};
// <<< END GENERATED gpu_line_function

// The constants behind the billboard shader's minimum-width floor, worked out
// once per constant upload. A free function so the offline harness calls
// exactly this.
//
//   m               the transform the anchors go through: view-projection, or
//                   world * view-projection for a mesh
//   right3          the direction one unit of ox offsets along, in the same
//                   axes as the anchors
//   pixel_scale_x/y clip units per screen pixel, c6
//   half_floor      half the narrowest the quad may appear, in pixels
//
//   width_clip[4] -> c9   (right3, 0) * m: the clip-space delta of one unit of
//                         ox, the transform being affine in the offset
//   floor_clip[2] -> c10  the same direction, scaled so a clip delta of
//                         c10.xy * w lands half_floor pixels from the anchor
//                         whatever w is
//   floor_test[2] -> c11  (x) the pixel length of c9.xy at w = 1, and
//                         (y) half_floor to compare it with
//
// c9.xy of zero length means the offset points straight at the camera and there
// is no direction to stretch along; c11.y then loads 0, which leaves the exact
// offset alone for every vertex.
inline void gpu_width_floor_constants(const D3DMATRIX& m, const float* right3,
    float pixel_scale_x, float pixel_scale_y, float half_floor,
    float* width_clip, float* floor_clip, float* floor_test) {
    for (int column = 0; column < 4; ++column) {
        width_clip[column] = right3
            ? right3[0] * m.m[0][column] + right3[1] * m.m[1][column]
                + right3[2] * m.m[2][column]
            : 0.0f;
    }

    // The same delta in pixels, at w = 1.
    const float pixels_x = pixel_scale_x != 0.0f ? width_clip[0] / pixel_scale_x : 0.0f;
    const float pixels_y = pixel_scale_y != 0.0f ? width_clip[1] / pixel_scale_y : 0.0f;
    const float pixel_length = std::sqrt(pixels_x * pixels_x + pixels_y * pixels_y);

    floor_clip[0] = 0.0f;
    floor_clip[1] = 0.0f;
    floor_test[0] = 0.0f;
    floor_test[1] = 0.0f;
    if (pixel_length > 1.0e-8f) {
        floor_clip[0] = width_clip[0] / pixel_length * half_floor;
        floor_clip[1] = width_clip[1] / pixel_length * half_floor;
        floor_test[0] = pixel_length;
        floor_test[1] = half_floor;
    }
}

class WorldDrawPlugin : public PluginBase {
private:
    // The thread rule, which everything below it exists to keep.
    //
    // No device method may be called from the main thread. The daemon's thunks
    // run on the game's render thread; a front-end's calls run on the main one.
    // FFXI creates its device without D3DCREATE_MULTITHREADED, so the D3D8
    // runtime takes no lock and two threads inside one device is undefined.
    //
    // A lock of ours cannot fix it: the game's render thread calls the same
    // device knowing nothing of us and cannot be made to take one.
    //
    // A release is a device call too, so teardown queues its releases here for
    // the render thread. The list is the image's, not an instance's, so it
    // outlives every handle and registration. The cost is accepted: work queued
    // after the last consumer closes waits until something registers again, and
    // is held to process exit if nothing ever does.
    enum DeferredKind {
        deferred_texture,   // an IDirect3DTexture8 and the pixels it was made from
        deferred_buffer,    // an IDirect3DVertexBuffer8
        deferred_shader,    // up to two vertex shader handles, and the device that owns them
        deferred_gdiplus,   // a GDI+ token
    };

    struct DeferredWork {
        DeferredWork* next = nullptr;
        DeferredKind kind = deferred_buffer;
        IDirect3DTexture8* texture = nullptr;
        IDirect3DVertexBuffer8* buffer = nullptr;
        IDirect3DDevice8* device = nullptr;
        DWORD shader_a = 0;
        DWORD shader_b = 0;
        std::uint8_t* pixels = nullptr;
        ULONG_PTR token = 0;
    };

    struct Texture {
        // Made by the render thread, from `pixels`, the first time the id is
        // drawn with. Until then the id draws untextured.
        IDirect3DTexture8* texture = nullptr;
        // The 32-bit BGRA copy CreateTexture took, `width` * 4 bytes a row,
        // owned by this entry until the render thread has consumed it.
        std::uint8_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        // Claimed, whatever state the two pointers are in: this alone says the
        // entry is somebody's, so claim_texture cannot hand it out twice.
        bool used = false;
        // The device refused. A property of the device, so it is never retried.
        bool failed = false;
    };

    // The texture pool: a chain of blocks, each twice the size of the one
    // before it, an id being an index into the sequence they form.
    //
    // Chained, never reallocated, because the render thread walks this pool
    // without a lock. Adding a block must move no existing entry and free
    // nothing, and is published by one pointer store into a block that is
    // already complete.
    struct TextureBlock {
        TextureBlock* next = nullptr;
        int first = 0;      // the pool index this block's entries[0] carries
        int capacity = 0;
        Texture* entries = nullptr;
    };

    // The first block's size, not a limit: each block doubles the last.
    static constexpr int initial_textures_ = 64;

public:
    // An overflow guard, so the size arithmetic in CreateGpuBuffer cannot wrap.
    // Public because a front-end tessellating into a buffer has to refuse a
    // size before it wraps rather than after.
    static constexpr UINT max_gpu_vertices_ =
        static_cast<UINT>(0xFFFFFFFFu / sizeof(GpuVertex));

    // The longest text this library ever hands OnError, and therefore the size
    // a front-end's own copy of it needs: a sentence plus one MAX_PATH path in
    // UTF-8.
    static constexpr std::size_t max_message_ = MAX_PATH * 3 + 512;

    void __stdcall Load(PluginManager* manager) override {
        plugin_manager_ = manager;
        AcquireDevice();
        OnLoad();
    }

    // Windower's own teardown for a plugin. Runs on the main thread, so every
    // release below queues rather than calling the device.
    void __stdcall Unload() override {
        OnUnload();
        detach_from_daemon();
        defer_dynamic_vb();
        defer_gpu_shader();
        release_all_textures();
    }

    // Never refuses. The plugin can always be unloaded and its DLL replaced,
    // whatever else is loaded, in any order.
    bool __stdcall IgnoreUnload() override {
        return false;
    }

    void __stdcall PostRender() override {
        frame_transforms_valid_ = false;

        if (!draw_enabled_) {
            return;
        }

        OnFrame();

        // The game can destroy its device and make another, so it is
        // re-acquired by identity once a frame. Nothing here is a device call:
        // AcquireDevice reads the game's renderer global and check_hook_slots
        // asks the daemon what is in the vtables it retained.
        refresh_device();
        check_hook_slots();

        // Normally a no-op: setup happens at Open(), but a front-end that
        // opened before the device existed gets another chance here.
        if (!hook_installed_ && !hook_install_failed_ && EnsureDevice()) {
            attach_to_daemon();
        }
    }

protected:
    // Emit your geometry here. Called once per frame while the world is drawn.
    // Runs on the render thread: keep it cheap and never block.
    virtual void OnWorldDraw(WorldDraw& draw) = 0;

    // Optional. OnFrame runs at the frame boundary and is where per-frame work
    // belongs -- reading files, updating whatever OnWorldDraw will consume.
    virtual void OnLoad() {}
    virtual void OnUnload() {}
    virtual void OnFrame() {}

    // Optional. Gets a short message when something goes wrong. Does nothing
    // by default; override to log it however you like.
    virtual void OnError(const char* message) { (void)message; }

    // The same channel from a front-end's side. A front-end that fails at
    // something this header cannot see -- a commit that found no device, a
    // mesh that could not be built -- reports it here rather than inventing a
    // channel of its own, and the message contract below applies to it
    // unchanged: "worlddraw " in front means a player sees it.
    void ReportError(const char* message) { report_error(message); }

    void SetDrawEnabled(bool enabled) { draw_enabled_ = enabled; }

    // Paired by front-ends several consumers can open at once. The registration
    // goes only when the last consumer has gone.
    void Open() {
        // No lock needed: per image, and both front-ends open and close on the
        // game's main thread.
        ++open_count_;

        // Setup here rather than per frame, so a caller need not have ticked
        // before any of this is usable.
        if (EnsureDevice() && !hook_installed_ && !hook_install_failed_) {
            attach_to_daemon();
        }
    }

    // The mirror of Open, and the only teardown a Lua front-end ever reaches:
    // nothing calls Unload() for a module loaded through package.loadlib, so
    // everything this image owns must be released here.
    //
    // The order is load-bearing: detach_from_daemon() first, because
    // unregister_set drains and only past that drain is the render thread
    // certainly out of BindGpuTexture; release_all_textures() second, because
    // it frees the TextureBlock chain that reader was walking.
    //
    // Nothing here calls the device. Both steps queue their releases.
    void Close() {
        const int remaining = open_count_ > 0 ? --open_count_ : 0;
        if (remaining == 0) {
            detach_from_daemon();
            release_all_textures();
        }
    }

    int OpenCount() const { return open_count_; }

    // Unregister and go inert. The hooks stay: they are the daemon's, and
    // nothing in this image can take them out.
    void Shutdown() { detach_from_daemon(); }

    // The resident daemon, for a version string a support question can use.
    // Zero and nullptr until one has been acquired.
    std::uint32_t DaemonAbi() const {
        return daemon_api_ ? daemon_api_->abi_version : 0;
    }

    const char* DaemonBuild() const {
        return daemon_api_ && daemon_api_->build_id ? daemon_api_->build_id() : nullptr;
    }

    bool DrawEnabled() const { return draw_enabled_; }

    // Make a texture from pixels you already have: 32-bit BGRA, `width` * 4
    // bytes per row, top row first. Returns 0 on failure, and the only failures
    // are memory ones.
    //
    // No device is touched and none is needed: the pixels are copied and the id
    // handed back at once, and the IDirect3DTexture8 is made by the render
    // thread on the first draw that wants it (see realize_texture). Until then
    // the id draws untextured.
    TextureId CreateTexture(const void* pixels, int width, int height) {
        if (!pixels || width <= 0 || height <= 0) {
            return 0;
        }

        // A copy this image owns until the render thread has consumed it,
        // refused before the size arithmetic can wrap.
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
        if (row_bytes / 4 != static_cast<std::size_t>(width)
            || static_cast<std::size_t>(height) > SIZE_MAX / row_bytes) {
            report_error("texture: out of memory");
            return 0;
        }

        const std::size_t bytes = row_bytes * static_cast<std::size_t>(height);
        std::uint8_t* const copy = new (std::nothrow) std::uint8_t[bytes];
        if (!copy) {
            report_error("texture: out of memory");
            return 0;
        }
        std::memcpy(copy, pixels, bytes);

        TextureId id = 0;
        Texture* entry = claim_texture(id);
        if (!entry) {
            delete[] copy;
            report_error("texture: out of memory");
            return 0;
        }

        // `pixels` is written last, after the barrier: it publishes the entry,
        // so a render thread that reaches it never sees an unwritten size.
        entry->width = width;
        entry->height = height;
        entry->failed = false;
        MemoryBarrier();
        entry->pixels = copy;
        return id;
    }

    // Gives the entry back. The texture and the pixel copy are queued, because
    // the render thread may be reading either this instant; `used` is cleared
    // last, after the barrier, so claim_texture cannot re-issue this entry
    // until both pointers are off it.
    void ReleaseTexture(TextureId id) {
        Texture* entry = texture_entry(id);
        if (!entry || !entry->used) {
            return;
        }

        defer_texture(entry->texture, entry->pixels);
        entry->texture = nullptr;
        entry->pixels = nullptr;
        entry->width = 0;
        entry->height = 0;
        entry->failed = false;
        MemoryBarrier();
        entry->used = false;
    }

    // The size the id was made with, whether or not the render thread has made
    // the texture yet: a property of the image handed over, not of the device.
    bool TextureSize(TextureId id, int& width, int& height) const {
        const Texture* entry = texture_entry(id);
        if (!entry || (!entry->texture && !entry->pixels)) {
            return false;
        }
        width = entry->width;
        height = entry->height;
        return true;
    }

#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
    // Load an image file (png, jpg, bmp, gif, tiff). Requires linking gdiplus.
    // Never call this while drawing -- decoding is slow.
    //
    // All CPU work: nothing here touches a Direct3D interface, so the decode
    // may run on the caller's thread and needs no device.
    TextureId LoadTexture(const char* path) {
        if (!path) {
            return 0;
        }

        if (!gdiplus_token_) {
            Gdiplus::GdiplusStartupInput input;
            if (FFXI_WORLD_DRAW_GDIPLUS_STARTUP(&gdiplus_token_, &input) != Gdiplus::Ok) {
                report_error("texture: GDI+ startup failed");
                gdiplus_token_ = 0;
                return 0;
            }
        }

        WCHAR wide[MAX_PATH] {};
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH) == 0) {
            report_error("texture: path could not be converted");
            return 0;
        }

        Gdiplus::Bitmap bitmap(wide);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            report_error("texture: image could not be read");
            return 0;
        }

        const int width = static_cast<int>(bitmap.GetWidth());
        const int height = static_cast<int>(bitmap.GetHeight());
        if (width <= 0 || height <= 0) {
            return 0;
        }

        Gdiplus::Rect area(0, 0, width, height);
        Gdiplus::BitmapData data {};
        if (bitmap.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data)
                != Gdiplus::Ok) {
            report_error("texture: image lock failed");
            return 0;
        }

        TextureId id = 0;
        if (data.Stride == width * 4) {
            id = CreateTexture(data.Scan0, width, height);
        } else {
            // Repack to tightly packed rows first.
            std::uint8_t* packed = static_cast<std::uint8_t*>(
                HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(width) * height * 4));
            if (packed) {
                for (int row = 0; row < height; ++row) {
                    std::memcpy(packed + static_cast<std::size_t>(row) * width * 4,
                        static_cast<const std::uint8_t*>(data.Scan0)
                            + static_cast<std::size_t>(row) * data.Stride,
                        static_cast<std::size_t>(width) * 4);
                }
                id = CreateTexture(packed, width, height);
                HeapFree(GetProcessHeap(), 0, packed);
            }
        }

        bitmap.UnlockBits(&data);
        return id;
    }
#endif

    // Vertices held before the batch is sent and the buffer reused. Not a limit
    // on how much you can draw: Triangle flushes a full buffer and carries on.
    // It bounds only how often one DrawPrimitiveUP goes out.
    static constexpr int vertex_batch_ = 8190;

    // Reading the game's own memory. Protected so a front-end can resolve what
    // it needs without carrying a second copy of the scanner.
    template<typename T>
    bool read_memory_raw(std::uintptr_t address, T& value) const {
        SIZE_T bytes_read = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &value, sizeof(T), &bytes_read) &&
            bytes_read == sizeof(T);
    }

    template<typename T>
    bool read_memory(std::uintptr_t address, T& value) const {
        if (!is_readable_range(address, sizeof(T))) {
            return false;
        }

        return read_memory_raw(address, value);
    }

    bool get_module_image(const char* name, std::uintptr_t& image_base, std::size_t& image_size) const {
        image_base = 0;
        image_size = 0;

        HMODULE module = GetModuleHandleA(name);
        if (!module) {
            return false;
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
        IMAGE_DOS_HEADER dos_header {};
        if (!read_memory(base, dos_header) || dos_header.e_magic != IMAGE_DOS_SIGNATURE
            // The PE header's own limits: a negative or absurd e_lfanew is
            // not a header this is willing to walk.
            || dos_header.e_lfanew <= 0 || dos_header.e_lfanew > 0x10000) {
            return false;
        }

        IMAGE_NT_HEADERS32 nt_headers {};
        if (!read_memory(base + static_cast<std::uintptr_t>(dos_header.e_lfanew), nt_headers)
            || nt_headers.Signature != IMAGE_NT_SIGNATURE
            || nt_headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return false;
        }

        const std::size_t size_of_image = static_cast<std::size_t>(nt_headers.OptionalHeader.SizeOfImage);
        // Smaller than one page, or half the 32-bit user address space: a
        // SizeOfImage outside that is not a module, whatever the header says.
        if (size_of_image < 0x1000 || size_of_image > 0x20000000) {
            return false;
        }

        image_base = base;
        image_size = size_of_image;
        return true;
    }

    std::uintptr_t scan_module(std::uintptr_t image_base, std::size_t image_size,
        const std::uint8_t* pattern, const char* mask, std::size_t length) {
        const std::uintptr_t image_end = image_base + image_size;
        std::uintptr_t cursor = image_base;

        while (cursor < image_end) {
            MEMORY_BASIC_INFORMATION mbi {};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) {
                return 0;
            }

            const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (region_end <= cursor) {
                return 0;
            }

            if (mbi.State != MEM_COMMIT || !is_readable_page(mbi.Protect)) {
                cursor = region_end;
                continue;
            }
            std::uintptr_t span_end = region_end;
            while (span_end < image_end) {
                MEMORY_BASIC_INFORMATION next {};
                if (!VirtualQuery(reinterpret_cast<const void*>(span_end), &next, sizeof(next))) {
                    break;
                }

                const std::uintptr_t next_end = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
                if (next_end <= span_end || next.State != MEM_COMMIT || !is_readable_page(next.Protect)) {
                    break;
                }

                span_end = next_end;
            }

            const std::uintptr_t scan_end = span_end < image_end ? span_end : image_end;
            const std::uintptr_t hit = scan_span(cursor, scan_end, pattern, mask, length);
            if (hit) {
                return hit;
            }

            cursor = span_end;
        }

        return 0;
    }

    // Machinery for a front-end that describes geometry once and lets the GPU
    // place it every frame. The screen-space path above neither uses it nor is
    // disturbed by it.
    //
    // The order a consumer must follow inside OnWorldDraw:
    //
    //   1. EnsureGpuPipeline(device)      once, at the top; false means draw
    //                                     nothing through this path
    //   2. LoadGpuConstants(device, ...)  once per transform, a mesh's world
    //                                     matrix premultiplied into the one
    //                                     passed in
    //   3. DrawGpuRange(device, vb, ...)  once per range, repeated freely
    //   4. EndGpuDraws(device)            once, before any screen-space batch
    //                                     follows in the same composite
    //
    // Every one of them is a device method, so every one is render thread only
    // -- and so are CreateGpuBuffer and UpdateGpuBuffer. A front-end stages
    // vertices on whatever thread it is on and the render thread uploads them
    // at the top of its own composite, so the geometry appears one frame later.
    //
    // begin_draw_state and end_draw_state save and restore the shader handle,
    // its constants and stream 0, so a composite leaves the pass as it found it.

    // Render thread only.
    //
    // Made on first use and kept for the life of the device. A failure is a
    // property of the device, so it is reported once and never retried. D3D8
    // vertex shader handles survive Reset, so only teardown releases them.
    bool EnsureGpuPipeline(IDirect3DDevice8* device) {
        if (gpu_shader_ && gpu_line_shader_ && gpu_shader_device_ == device) {
            return true;
        }
        if (!device) {
            return false;
        }

        // Handles from a device the game destroyed are dropped, never deleted:
        // this library holds no reference on a device, so it cannot know the old
        // one is still alive, and DeleteVertexShader on a device that has gone
        // is certainly wrong. What leaks dies with the device that owned it.
        if (gpu_shader_device_ && gpu_shader_device_ != device) {
            gpu_shader_ = 0;
            gpu_line_shader_ = 0;
            gpu_shader_device_ = nullptr;
            gpu_shader_failed_ = false;

            // Said once. A wrapper handing the composite a different device
            // from the one buffers are built on would rebuild every frame, and
            // this line is the only thing that tells the two cases apart.
            if (!gpu_device_change_reported_) {
                gpu_device_change_reported_ = true;
                report_error("gpu: the graphics device changed; the shaders were dropped "
                    "and rebuilt on the new one");
            }
        }

        if (gpu_shader_failed_) {
            return false;
        }

        // Both or neither: a consumer that got one and not the other would
        // have half a pipeline and no way to say which half.
        DWORD billboard = 0;
        DWORD line = 0;
        if (FAILED(device->CreateVertexShader(gpu_shader_declaration, gpu_shader_function,
                &billboard, 0)) || billboard == 0
            || FAILED(device->CreateVertexShader(gpu_line_declaration, gpu_line_function,
                &line, 0)) || line == 0) {
            if (billboard) {
                device->DeleteVertexShader(billboard);
            }
            if (line) {
                device->DeleteVertexShader(line);
            }
            gpu_shader_ = 0;
            gpu_line_shader_ = 0;
            gpu_shader_device_ = nullptr;
            gpu_shader_failed_ = true;
            report_error("gpu: vertex shader could not be created");
            return false;
        }

        gpu_shader_ = billboard;
        gpu_line_shader_ = line;
        gpu_shader_device_ = device;
        return true;
    }

    // Loads c0-c8 in one upload.
    //
    //   c0-c3  the columns of m, never the rows: this library composes in the
    //          row-vector convention (out = v * M) and a dp4 is a dot product,
    //          so loading the rows would transpose the transform.
    //   c4     (right3[0..2], 0): the ground-plane camera-right the ox offsets
    //          ride along, in the caller's axes. Direct3D axes put its two
    //          ground components in c4.x and c4.z, not c4.x and c4.y.
    //   c5     (up3[0..2], 0): the up the oy offsets rise along. A null pointer
    //          loads zero, which flattens oy rather than corrupting the
    //          transform.
    //   c6     (pixel_scale_x, pixel_scale_y, 0, 0): clip units per screen
    //          pixel, negative on y because clip space rises where the screen
    //          falls.
    //   c7     (1, 0, 0, 0): the literals. c7.x is the w the shader writes into
    //          the anchor before the transform.
    //   c8     (forward3[0..2], 0): the camera forward the line shader crosses
    //          with each line's direction. Zero when none is passed, which makes
    //          every line quad degenerate rather than putting it somewhere wrong.
    //   c9-c11 the minimum-width floor, worked out from the three above by
    //          gpu_width_floor_constants. Neither shader reads c4 directly; it
    //          is an input to c9 and c10.
    void LoadGpuConstants(IDirect3DDevice8* device, const D3DMATRIX& m,
        float right_x, float right_y, const float* up3,
        float pixel_scale_x, float pixel_scale_y) {
        const float right3[3] = {right_x, right_y, 0.0f};
        LoadGpuConstants(device, m, right3, up3, nullptr, pixel_scale_x, pixel_scale_y);
    }

    // The same upload for a caller whose axes need three components in c4 --
    // Direct3D axes put the camera right's ground components in x and z, not in
    // x and y -- and which has a camera forward for c8.
    void LoadGpuConstants(IDirect3DDevice8* device, const D3DMATRIX& m,
        const float* right3, const float* up3, const float* forward3,
        float pixel_scale_x, float pixel_scale_y) {
        if (!device) {
            return;
        }

        float constants[gpu_constant_count_][4] {};
        for (int column = 0; column < 4; ++column) {
            constants[column][0] = m.m[0][column];
            constants[column][1] = m.m[1][column];
            constants[column][2] = m.m[2][column];
            constants[column][3] = m.m[3][column];
        }

        for (int i = 0; i < 3; ++i) {
            constants[4][i] = right3 ? right3[i] : 0.0f;
            constants[5][i] = up3 ? up3[i] : 0.0f;
            constants[8][i] = forward3 ? forward3[i] : 0.0f;
        }
        constants[6][0] = pixel_scale_x;
        constants[6][1] = pixel_scale_y;
        constants[7][0] = 1.0f;

        // The same constant and rule the screen-space path applies in
        // expand_pair; the two paths must floor identically.
        gpu_width_floor_constants(m, right3, pixel_scale_x, pixel_scale_y,
            min_projected_width_ * 0.5f, constants[9], constants[10], constants[11]);

        device->SetVertexShaderConstant(0, constants, gpu_constant_count_);
    }

    // Render thread only.
    //
    // A managed buffer holding `count` vertices. D3DPOOL_MANAGED survives Reset
    // without help from here, and FVF 0 because the declaration describes the
    // layout instead. Returns nullptr on any failure, silently.
    IDirect3DVertexBuffer8* CreateGpuBuffer(IDirect3DDevice8* device,
        const GpuVertex* vertices, UINT count) {
        if (!device || !vertices || count == 0 || count > max_gpu_vertices_) {
            return nullptr;
        }

        IDirect3DVertexBuffer8* buffer = nullptr;
        if (FAILED(device->CreateVertexBuffer(count * static_cast<UINT>(sizeof(GpuVertex)),
                D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &buffer))
            || !buffer) {
            return nullptr;
        }

        if (!UpdateGpuBuffer(buffer, vertices, count)) {
            buffer->Release();
            return nullptr;
        }

        return buffer;
    }

    // Render thread only.
    //
    // Rewrites an existing buffer from the front. `count` must fit what the
    // buffer was made to hold; a lock past its end returns false with nothing
    // left locked.
    bool UpdateGpuBuffer(IDirect3DVertexBuffer8* buffer, const GpuVertex* vertices, UINT count) {
        if (!buffer || !vertices || count == 0 || count > max_gpu_vertices_) {
            return false;
        }

        const UINT bytes = count * static_cast<UINT>(sizeof(GpuVertex));
        BYTE* mapped = nullptr;
        if (FAILED(buffer->Lock(0, bytes, &mapped, 0)) || !mapped) {
            return false;
        }

        std::memcpy(mapped, vertices, bytes);
        return SUCCEEDED(buffer->Unlock());
    }

    // One triangle-list range out of one buffer. The shader and the stream are
    // left where this put them; end_draw_state puts the pass back either way.
    bool DrawGpuRange(IDirect3DDevice8* device, IDirect3DVertexBuffer8* buffer,
        UINT start_vertex, UINT primitive_count, GpuShader shader = GpuShaderBillboard) {
        const DWORD handle = shader == GpuShaderLine ? gpu_line_shader_ : gpu_shader_;
        if (!device || !buffer || primitive_count == 0 || !handle) {
            return false;
        }

        if (FAILED(device->SetVertexShader(handle))
            || FAILED(device->SetStreamSource(0, buffer, static_cast<UINT>(sizeof(GpuVertex))))) {
            return false;
        }

        return SUCCEEDED(device->DrawPrimitive(D3DPT_TRIANGLELIST, start_vertex, primitive_count));
    }

    // Render thread only.
    //
    // Binds a pool texture and its stage states: modulate against the diffuse
    // colour when there is an image, take the diffuse alone when there is not.
    // Id 0, an empty slot and an id whose texture could not be made are all
    // untextured.
    //
    // And it is where a texture is made -- here, on the thread allowed to make
    // one, the first time somebody draws with the id.
    void BindGpuTexture(IDirect3DDevice8* device, TextureId id) {
        if (!device) {
            return;
        }

        IDirect3DTexture8* bound = realize_texture(device, id);
        device->SetTexture(0, bound);
        if (bound) {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        } else {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
    }

    // Hands the pass back to the fixed-function FVF the screen-space path draws
    // with. Must be called after the last DrawGpuRange of a composite: batches
    // that follow it are XYZRHW vertices, not GpuVertex.
    void EndGpuDraws(IDirect3DDevice8* device) {
        if (!device) {
            return;
        }

        device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    }

    // The device the current composite is drawing on, or nullptr outside one.
    // Valid for the length of OnWorldDraw and nowhere else.
    IDirect3DDevice8* FrameDevice() const { return dispatch_device_; }

    // The device to build on outside a composite, acquired on demand and null
    // when the game has not made one yet. Inside OnWorldDraw use FrameDevice:
    // that is the device the composite is actually running on.
    IDirect3DDevice8* GpuDevice() { return EnsureDevice() ? d3d_device_ : nullptr; }

    // The frame's view-projection, the same matrix Project uses, valid while
    // OnWorldDraw runs. Its world axes are the Direct3D ones -- x = EW,
    // y = raw game height, z = NS -- which is the order live_world_to_screen
    // feeds it. A caller keeping anchors in this library's (EW, NS, height)
    // argument order must fold that swap into the matrix it hands
    // LoadGpuConstants, along with any world matrix it is premultiplying.
    const D3DMATRIX& FrameViewProjection() const { return cached_view_projection_; }

    // Ground-plane camera-right for the current composite: x = EW, y = NS,
    // already normalised. The same pair WorldDraw::CameraRightX/Y report --
    // this is where a consumer that never touches a WorldDraw gets them.
    float FrameGroundRightX() const { return dispatch_right_x_; }
    float FrameGroundRightY() const { return dispatch_right_y_; }

    // The camera forward for the current composite: three floats in the
    // Direct3D axes FrameViewProjection is in -- x = EW, y = raw game height,
    // z = NS -- normalised, valid while OnWorldDraw runs. It is column 2 of the
    // captured view matrix, this library composing in the row-vector convention.
    const float* FrameViewForward() const { return dispatch_forward_; }

private:
    friend class WorldDraw;

    void report_error(const char* message) { OnError(message); }

    // A singly-linked list, pushed by whatever thread gives a resource back and
    // emptied whole by the render thread. Lock-free, not guarded: it is reached
    // from inside the render path, which may not wait on a lock the main thread
    // holds. Nothing ever pops a single node, so there is no ABA to have, and
    // the drain sees reverse order, which nothing here depends on.
    //
    // One list per image, so the next render-thread visit of any plugin in the
    // image drains it.
    inline static DeferredWork* volatile deferred_head_ = nullptr;
    // Said once: repeating it per texture would bury everything else.
    inline static bool deferred_lost_reported_ = false;

    // False when the node could not be allocated. The resource is then leaked
    // deliberately: releasing it here is the crash this list exists to avoid.
    bool push_deferred(const DeferredWork& work) {
        DeferredWork* const node = new (std::nothrow) DeferredWork(work);
        if (!node) {
            if (!deferred_lost_reported_) {
                deferred_lost_reported_ = true;
                report_error("gpu: out of memory queueing a release; "
                    "a graphics resource was leaked");
            }
            return false;
        }

        for (;;) {
            DeferredWork* const head = deferred_head_;
            node->next = head;
            if (InterlockedCompareExchangePointer(
                    reinterpret_cast<void* volatile*>(&deferred_head_), node, head) == head) {
                return true;
            }
        }
    }

    void defer_texture(IDirect3DTexture8* texture, std::uint8_t* pixels) {
        if (!texture && !pixels) {
            return;
        }
        DeferredWork work;
        work.kind = deferred_texture;
        work.texture = texture;
        work.pixels = pixels;
        push_deferred(work);
    }

    void defer_shader(IDirect3DDevice8* device, DWORD billboard, DWORD line) {
        if (!device || (billboard == 0 && line == 0)) {
            return;
        }
        DeferredWork work;
        work.kind = deferred_shader;
        work.device = device;
        work.shader_a = billboard;
        work.shader_b = line;
        push_deferred(work);
    }

    void defer_gdiplus(ULONG_PTR token) {
        if (token == 0) {
            return;
        }
        DeferredWork work;
        work.kind = deferred_gdiplus;
        work.token = token;
        push_deferred(work);
    }

protected:
    // A vertex buffer a front-end made through CreateGpuBuffer and is giving
    // back. A front-end has no render thread of its own to release one on.
    void DeferVertexBufferRelease(IDirect3DVertexBuffer8* buffer) {
        if (!buffer) {
            return;
        }
        DeferredWork work;
        work.kind = deferred_buffer;
        work.buffer = buffer;
        push_deferred(work);
    }

private:
    // Render thread only. Takes the whole list and performs it. Called only
    // from the two handlers that are certainly outside a composite of ours: the
    // render-target change, and the reset, which must not find a
    // D3DPOOL_DEFAULT resource of ours still alive.
    static void drain_deferred() {
        if (!deferred_head_) {
            return;
        }

        DeferredWork* node = static_cast<DeferredWork*>(InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&deferred_head_), nullptr));
        while (node) {
            DeferredWork* const next = node->next;
            switch (node->kind) {
            case deferred_texture:
                if (node->texture) {
                    node->texture->Release();
                }
                delete[] node->pixels;
                break;
            case deferred_buffer:
                if (node->buffer) {
                    node->buffer->Release();
                }
                break;
            case deferred_shader:
                if (node->shader_a) {
                    node->device->DeleteVertexShader(node->shader_a);
                }
                if (node->shader_b) {
                    node->device->DeleteVertexShader(node->shader_b);
                }
                break;
            case deferred_gdiplus:
#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
                FFXI_WORLD_DRAW_GDIPLUS_SHUTDOWN(node->token);
#endif
                break;
            }
            delete node;
            node = next;
        }
    }

    // The message contract, which lua/worlddraw.lua parses on. Every message
    // this library produces must take one of exactly two shapes:
    //
    //   Player-facing begins with "worlddraw ", plus the "details:" line that
    //   belongs to one of them. Only these are put in front of a player.
    //
    //   Engineering begins with a subsystem prefix -- "texture: ", "gpu: ",
    //   "hook: ", "draw: ", "scan: ". Retrievable through the handle's
    //   last_error and never chatted.

    // A setup failure. The action lines come first and are the same whatever
    // failed; `details:` is the only line that differs.
    void report_setup_failure(const char* details) {
        char message[max_message_] {};
        std::snprintf(message, sizeof(message), "%s\ndetails: %s",
            setup_failed_message_, details ? details : "hook: no detail recorded");
        report_error(message);
    }

    // An id is the pool index plus one, and a block owns a known run of them.
    // Growth never moves an entry, so an id resolves to the same Texture for
    // the life of the plugin.
    const Texture* texture_entry(TextureId id) const {
        if (id <= 0) {
            return nullptr;
        }

        const int index = id - 1;
        for (const TextureBlock* block = texture_blocks_; block; block = block->next) {
            if (index < block->first + block->capacity) {
                return &block->entries[index - block->first];
            }
        }
        return nullptr;
    }

    Texture* texture_entry(TextureId id) {
        return const_cast<Texture*>(
            static_cast<const WorldDrawPlugin*>(this)->texture_entry(id));
    }

    // Render thread only. The texture an id names, made here if this is the
    // first draw that wanted it -- this being the thread that may call the
    // device. A failure is a property of the device, so the entry is marked and
    // never retried, and the id goes on drawing untextured.
    IDirect3DTexture8* realize_texture(IDirect3DDevice8* device, TextureId id) {
        Texture* entry = texture_entry(id);
        if (!entry) {
            return nullptr;
        }
        if (entry->texture) {
            return entry->texture;
        }
        if (!entry->pixels || entry->failed || !device) {
            return nullptr;
        }

        IDirect3DTexture8* texture = nullptr;
        if (FAILED(device->CreateTexture(static_cast<UINT>(entry->width),
                static_cast<UINT>(entry->height), 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                &texture))
            || !texture) {
            entry->failed = true;
            release_pending_pixels(*entry);
            report_error("texture: creation failed");
            return nullptr;
        }

        D3DLOCKED_RECT locked {};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
            texture->Release();
            entry->failed = true;
            release_pending_pixels(*entry);
            report_error("texture: lock failed");
            return nullptr;
        }

        const std::size_t row_bytes = static_cast<std::size_t>(entry->width) * 4;
        const std::uint8_t* const source = entry->pixels;
        std::uint8_t* const destination = static_cast<std::uint8_t*>(locked.pBits);
        for (int row = 0; row < entry->height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * locked.Pitch,
                source + static_cast<std::size_t>(row) * row_bytes, row_bytes);
        }
        texture->UnlockRect(0);

        // The texture is published before the pixels are taken away, so the
        // entry never looks free to claim_texture in between.
        entry->texture = texture;
        MemoryBarrier();
        release_pending_pixels(*entry);
        return texture;
    }

    // Render thread only, and only for an entry this thread has just finished
    // with: freed outright rather than queued, this being the draining thread.
    void release_pending_pixels(Texture& entry) {
        std::uint8_t* const pixels = entry.pixels;
        entry.pixels = nullptr;
        delete[] pixels;
    }

    // A free entry and the id that names it, appending a block when every entry
    // is taken. Never called from a handler, and never concurrently with itself.
    //
    // Free is `used`, never "has no texture": an entry may legitimately hold
    // pixels and no texture, or neither, while an id still names it. `used` is
    // set here, before the caller has filled anything in.
    Texture* claim_texture(TextureId& id) {
        TextureBlock* last = nullptr;
        for (TextureBlock* block = texture_blocks_; block; block = block->next) {
            for (int i = 0; i < block->capacity; ++i) {
                if (!block->entries[i].used) {
                    block->entries[i].used = true;
                    id = block->first + i + 1;
                    return &block->entries[i];
                }
            }
            last = block;
        }

        TextureBlock* block = append_texture_block(last);
        if (!block) {
            return nullptr;
        }

        block->entries[0].used = true;
        id = block->first + 1;
        return &block->entries[0];
    }

    // The next block, twice the size of the one it follows. The only ceiling is
    // arithmetic: a TextureId is an int.
    TextureBlock* append_texture_block(TextureBlock* last) {
        const int first = last ? last->first + last->capacity : 0;
        int capacity = initial_textures_;
        if (last) {
            if (last->capacity > INT_MAX / 2) {
                return nullptr;
            }
            capacity = last->capacity * 2;
        }
        if (first > INT_MAX - capacity) {
            return nullptr;
        }

        TextureBlock* block = new (std::nothrow) TextureBlock();
        if (!block) {
            return nullptr;
        }

        Texture* entries = new (std::nothrow) Texture[capacity];
        if (!entries) {
            delete block;
            return nullptr;
        }

        block->next = nullptr;
        block->first = first;
        block->capacity = capacity;
        block->entries = entries;

        // Published last, by one pointer store into an already complete block;
        // the barrier is what makes "complete" true for the render thread too.
        MemoryBarrier();
        if (last) {
            last->next = block;
        } else {
            texture_blocks_ = block;
        }
        return block;
    }

    // Teardown only, from both teardowns: Unload() and Close(). Both must run
    // it past the drain in detach_from_daemon -- that drain is the only thing
    // that makes freeing the blocks safe, since growing them frees nothing.
    void release_all_textures() {
        TextureBlock* block = texture_blocks_;
        texture_blocks_ = nullptr;
        while (block) {
            for (int i = 0; i < block->capacity; ++i) {
                // The blocks go now, past the drain; the textures and pixel
                // copies are queued, the nodes carrying the raw pointers so
                // this storage can go while they wait.
                defer_texture(block->entries[i].texture, block->entries[i].pixels);
                block->entries[i].texture = nullptr;
                block->entries[i].pixels = nullptr;
                block->entries[i].used = false;
            }

            TextureBlock* const next = block->next;
            delete[] block->entries;
            delete block;
            block = next;
        }

#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
        // Queued like the rest, and the token dropped here: the next
        // LoadTexture starts GDI+ afresh whether or not the shutdown has run.
        if (gdiplus_token_) {
            defer_gdiplus(gdiplus_token_);
            gdiplus_token_ = 0;
        }
#endif
    }

    // The device is fetched on demand, so callers are not required to have
    // ticked first before creating textures.
    bool EnsureDevice() {
        if (!d3d_device_) {
            ensure_resolution();
            AcquireDevice();
        }
        return d3d_device_ != nullptr;
    }

    void AcquireDevice() {
        void* device = plugin_manager_ ? plugin_manager_->GetDirect3D8Device() : nullptr;
        if (!device) {
            device = DeviceFromGame();
        }
        if (device) {
            d3d_device_ = static_cast<IDirect3DDevice8*>(device);
        }
    }

    // Re-acquire by identity, not only when the cached pointer is null: this
    // image outlives a device the game destroyed and remade. Nothing here
    // releases anything, and AcquireDevice keeps what it has when it cannot get
    // one, so a frame taken between two devices does not clear the pointer.
    void refresh_device() {
        AcquireDevice();
    }

    // Render thread only: GetCreationParameters is a device method like any
    // other. Called from daemon_pre_set_render_target, the first render-thread
    // moment of a frame.
    //
    // Asks once whether this device was created with D3DCREATE_MULTITHREADED,
    // which is what decides whether the thread rule above is merely prudent or
    // load-bearing. Sticky: the answer is a property of the device.
    //
    // An engineering string, prefixed "gpu: ", so it never reaches chat.
    void report_device_behavior() {
        if (device_behavior_reported_ || !d3d_device_) {
            return;
        }
        device_behavior_reported_ = true;

        D3DDEVICE_CREATION_PARAMETERS parameters {};
        if (FAILED(d3d_device_->GetCreationParameters(&parameters))) {
            report_error("gpu: device behavior flags unavailable");
            return;
        }

        char message[max_message_] {};
        std::snprintf(message, sizeof(message),
            "gpu: device behavior flags 0x%08X, multithreaded=%s",
            static_cast<unsigned>(parameters.BehaviorFlags),
            (parameters.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0 ? "yes" : "no");
        report_error(message);
    }

    // Without a PluginManager to ask, take the device the game itself is using.
    void* DeviceFromGame() {
        if (renderer_global_ == 0) {
            return nullptr;
        }

        std::uint32_t renderer = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer_global_), renderer) || !renderer) {
            return nullptr;
        }

        std::uint32_t device = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer) + renderer_device_offset_, device)
            || !device) {
            return nullptr;
        }

        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(device));
    }

    void DispatchWorldDraw(IDirect3DDevice8* device) {
        if (!draw_enabled_ || !device || !refresh_projection_matrices(device)) {
            return;
        }

        D3DVIEWPORT8 viewport {};
        if (FAILED(device->GetViewport(&viewport))) {
            return;
        }

        if (!begin_draw_state(device)) {
            return;
        }

        float right_x = cached_view_.m[0][0];
        float right_y = cached_view_.m[2][0];
        const float right_length = std::sqrt(right_x * right_x + right_y * right_y);
        if (right_length < 1.0e-4f) {
            right_x = 1.0f;
            right_y = 0.0f;
        } else {
            right_x /= right_length;
            right_y /= right_length;
        }

        WorldDraw draw;
        draw.owner_ = this;
        draw.device_ = device;
        draw.buffer_ = vertices_;
        draw.capacity_ = vertex_batch_;
        draw.count_ = 0;
        draw.right_x_ = right_x;
        draw.right_y_ = right_y;
        draw.viewport_ = viewport;

        // Cleared straight after the callback, so nothing outside a composite
        // can read a stale device.
        dispatch_device_ = device;
        dispatch_right_x_ = right_x;
        dispatch_right_y_ = right_y;

        float forward_x = cached_view_.m[0][2];
        float forward_y = cached_view_.m[1][2];
        float forward_z = cached_view_.m[2][2];
        const float forward_length =
            std::sqrt(forward_x * forward_x + forward_y * forward_y + forward_z * forward_z);
        if (forward_length < 1.0e-4f) {
            forward_x = 0.0f;
            forward_y = 0.0f;
            forward_z = 1.0f;
        } else {
            forward_x /= forward_length;
            forward_y /= forward_length;
            forward_z /= forward_length;
        }
        dispatch_forward_[0] = forward_x;
        dispatch_forward_[1] = forward_y;
        dispatch_forward_[2] = forward_z;

        OnWorldDraw(draw);
        draw.Flush();

        dispatch_device_ = nullptr;

        end_draw_state(device);
    }

    // Applied when a batch is sent, so each batch carries the state it was
    // emitted under.
    void apply_batch_state(IDirect3DDevice8* device, TextureId texture,
                           bool depth_write, bool culling) {
        BindGpuTexture(device, texture);

        device->SetRenderState(D3DRS_ZWRITEENABLE, depth_write ? TRUE : FALSE);
        device->SetRenderState(D3DRS_CULLMODE, culling ? D3DCULL_CCW : D3DCULL_NONE);
    }

    void submit_batch(IDirect3DDevice8* device, const Vertex* vertices, int count) {
        if (count < 3) {
            return;
        }

        submit_vertices(device, D3DPT_TRIANGLELIST,
            static_cast<UINT>(count / 3), vertices, sizeof(Vertex));
    }

    Vertex vertices_[vertex_batch_] {};

    // One dynamic buffer written as a discard/no-overwrite ring. Everything
    // that touches these runs on the render thread, so no lock guards them.
    // Teardown is the exception, and reaches them only past the daemon's drain
    // inside unregister_set.
    IDirect3DVertexBuffer8* dynamic_vb_ = nullptr;
    UINT dynamic_vb_offset_ = 0;  // in vertices, not bytes
    bool dynamic_vb_failed_ = false;

    // A D3D8 vertex shader handle survives Reset, so only teardown releases
    // these. The device that made them is kept so the delete goes back to that
    // device and not to whatever the plugin has picked up since.
    DWORD gpu_shader_ = 0;
    DWORD gpu_line_shader_ = 0;
    IDirect3DDevice8* gpu_shader_device_ = nullptr;
    bool gpu_shader_failed_ = false;
    bool gpu_device_change_reported_ = false;

    bool draw_enabled_ = true;
    inline static int open_count_ = 0;


    static void capture_pass_transforms(IDirect3DDevice8* device) {
        if (pass_transforms_valid_ || !device) {
            return;
        }

        if (FAILED(device->GetTransform(D3DTS_VIEW, &pass_view_))
            || FAILED(device->GetTransform(D3DTS_PROJECTION, &pass_projection_))) {
            return;
        }

        pass_transforms_valid_ = true;
    }

    // The handlers the daemon calls. `user` is the instance that registered.
    // The order of the lines below is load-bearing: the guard sequence and the
    // point at which the captured transforms are dropped are what make the
    // world pass detectable. Handlers never alter an argument and never
    // suppress the original.

    static void __stdcall daemon_pre_reset(void* user, IDirect3DDevice8*,
        D3DPRESENT_PARAMETERS*) {
#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
        thread_probe::record(thread_probe::site_pre_reset);
#endif
        WorldDrawPlugin* const owner = static_cast<WorldDrawPlugin*>(user);
        hook_armed_ = false;

        // Also drained here, not only at the render-target change: a queued
        // D3DPOOL_DEFAULT buffer no frame has taken yet would fail the Reset.
        drain_deferred();

        // Nothing in D3DPOOL_DEFAULT may be alive across a Reset or the Reset
        // itself fails, so the dynamic vertex buffer goes here. Nothing of the
        // shader-driven pipeline belongs here: its buffers are D3DPOOL_MANAGED
        // and its shader handles survive Reset.
        if (owner) {
            owner->release_dynamic_vb();
        }
    }

    static void __stdcall daemon_post_reset(void* user, IDirect3DDevice8*, HRESULT result) {
#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
        thread_probe::record(thread_probe::site_post_reset);
#endif
        WorldDrawPlugin* const owner = static_cast<WorldDrawPlugin*>(user);
        if (SUCCEEDED(result)) {
            hook_armed_ = true;
        }

        // For the other case: a device the game replaced rather than reset.
        if (owner) {
            owner->refresh_device();
        }
    }

    static void __stdcall daemon_pre_set_render_target(void* user, IDirect3DDevice8* device,
        IDirect3DSurface8*, IDirect3DSurface8*) {
#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
        thread_probe::record(thread_probe::site_pre_set_render_target);
#endif
        WorldDrawPlugin* const owner = static_cast<WorldDrawPlugin*>(user);

        // The render-thread entry point for everything a main thread may not
        // do. Runs at every render-target change, which is certainly not inside
        // a composite of ours.
        drain_deferred();
        if (owner) {
            owner->report_device_behavior();
        }

        const bool qualifies = pass_transforms_valid_;
        if (owner && hook_armed_ && !hook_drawing_ && qualifies
            && owner->is_world_pass(device)) {
            hook_drawing_ = true;
            owner->DispatchWorldDraw(device);
            hook_drawing_ = false;
        }

        pass_transforms_valid_ = false;
    }

    static void __stdcall daemon_pre_draw(void* user, IDirect3DDevice8* device) {
#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
        thread_probe::record(thread_probe::site_pre_draw);
#endif
        // All four draw slots arrive here and none reads a draw argument, so
        // there is one handler; the captured pass belongs to the image, not to
        // a handle, so `user` goes unread.
        (void)user;
        if (!hook_drawing_) {
            capture_pass_transforms(device);
        }
    }

    // A foreign hooker landing on top of the daemon's thunks takes the pass
    // with it, and this is the only way to say why. Polled rarely, because
    // check_slots walks every retained vtable under the daemon's lock, and
    // reported once, because the condition never clears.
    //
    // Nothing is repaired and the daemon never re-chains: a foreign hook that
    // cleanly departed and one still installed beneath us are the same bytes,
    // so no recovery can be correct.
    void check_hook_slots() {
        if (!hook_installed_ || stomp_reported_ || !daemon_api_ || !daemon_api_->check_slots) {
            return;
        }

        if (++stomp_poll_frames_ < stomp_poll_interval_) {
            return;
        }
        stomp_poll_frames_ = 0;

        if (daemon_api_->check_slots() == 0) {
            return;
        }

        stomp_reported_ = true;
        report_error("worlddraw stopped drawing: another program took over the graphics.\n"
            "Restart FFXI, and load your addons before starting overlays like Discord or "
            "ReShade.");
    }

    // This image's own module, and its full path when one is asked for.
    // Resolved from an address inside the image, never by module name: every
    // addon ships its own copy of this basename and GetModuleHandleA would hand
    // back an arbitrary one. UNCHANGED_REFCOUNT is not optional -- without it
    // each call leaks a reference and the image can never be replaced.
    static HMODULE self_module(WCHAR* path, DWORD length) {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                &module_anchor_, &self)
            || !self) {
            return nullptr;
        }

        if (path) {
            const DWORD copied = GetModuleFileNameW(self, path, length);
            if (copied == 0 || copied >= length) {
                return nullptr;
            }
        }

        return self;
    }

    // This image's directory with `name` appended, which is where the daemon
    // has to be: it ships inside the same addon folder, never in a shared one.
    static bool path_beside_self(const WCHAR* name, WCHAR* out, DWORD length) {
        if (!self_module(out, length)) {
            return false;
        }

        DWORD cut = 0;
        for (DWORD i = 0; out[i] != L'\0'; ++i) {
            if (out[i] == L'\\' || out[i] == L'/') {
                cut = i + 1;
            }
        }

        DWORD i = 0;
        for (; name[i] != L'\0'; ++i) {
            if (cut + i + 1 >= length) {
                return false;
            }
            out[cut + i] = name[i];
        }
        out[cut + i] = L'\0';
        return true;
    }

    // The election's record, out of the pid-scoped mapping the daemon published
    // it in, for the two fields a refusal message needs. The record's prefix
    // through winner_build is frozen, so a record written by a daemon of any
    // age is readable that far in.
    static bool read_daemon_record(WdDaemonRecord& out) {
        char name[WD_NAME_MAX] {};
        std::snprintf(name, sizeof(name), WD_MAPPING_NAME_FORMAT,
            static_cast<unsigned>(GetCurrentProcessId()));

        HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
        if (!mapping) {
            return false;
        }

        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, WD_MAPPING_BYTES);
        if (!view) {
            CloseHandle(mapping);
            return false;
        }

        const WdDaemonRecord* const record = static_cast<const WdDaemonRecord*>(view);
        bool ok = false;
        if (record->magic == WD_MAGIC
            && record->record_size >= offsetof(WdDaemonRecord, api)) {
            const std::size_t copied = record->record_size < sizeof(out)
                ? record->record_size : sizeof(out);
            std::memcpy(&out, record, copied);
            out.winner_path[MAX_PATH - 1] = '\0';
            out.winner_build[WD_BUILD_MAX - 1] = '\0';
            ok = true;
        }

        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return ok;
    }

    // The file is not beside this image, or what is there is not the daemon.
    // The message must name the exact path looked for: putting a file there is
    // the only thing a player can do about it.
    void report_daemon_missing(const WCHAR* path) {
        char narrow[MAX_PATH * 3 + 1] {};
        if (WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr)
                == 0) {
            narrow[0] = '\0';
        }

        char message[max_message_] {};
        std::snprintf(message, sizeof(message),
            "worlddraw can't draw: a file is missing.\n"
            "Copy this addon's folder again from where you downloaded it.\n"
            "Missing: %s", narrow);
        report_error(message);
    }

    // acquire() refuses for exactly two reasons and the published record says
    // which: a resident daemon older than this engine needs, or an image that
    // could not pin itself and published nothing at all. Only the first has an
    // answer a player can carry out.
    void report_daemon_refusal() {
        WdDaemonRecord record {};
        if (read_daemon_record(record) && record.abi_version < WD_DAEMON_ABI) {
            char message[max_message_] {};
            std::snprintf(message, sizeof(message),
                "worlddraw can't draw: an older copy of its shared file is loaded.\n"
                "Copy this addon's libs\\worlddraw_daemon.dll over the one at:\n"
                "  %s\n"
                "Then restart FFXI.", record.winner_path);
            report_error(message);
            return;
        }

        report_setup_failure("hook: the daemon could not pin itself");
    }

    // Load the daemon shipped beside this image and take its API. Every failure
    // here is final: the flag stays set, the reason is reported once, and no
    // frame asks again. There is nothing to fall back to.
    bool EnsureDaemon() {
        if (daemon_api_) {
            return true;
        }
        if (daemon_failed_) {
            return false;
        }
        daemon_failed_ = true;

        WCHAR path[MAX_PATH] {};
        if (!path_beside_self(L"worlddraw_daemon.dll", path, MAX_PATH)) {
            // No path was worked out, so there is no file to name: this is
            // this image failing to find itself, not a missing daemon.
            report_setup_failure("hook: this image could not resolve its own path");
            return false;
        }

        const HMODULE module = FFXI_WORLD_DRAW_LOAD_LIBRARY_W(path);
        if (!module) {
            report_daemon_missing(path);
            return false;
        }

        // Through void (*)(): the cast that keeps the compiler quiet about
        // turning GetProcAddress's one function type into this one.
        const WdDaemonAcquire acquire = reinterpret_cast<WdDaemonAcquire>(
            reinterpret_cast<void (*)()>(
                FFXI_WORLD_DRAW_GET_PROC_ADDRESS(module, WD_DAEMON_ACQUIRE_NAME)));
        if (!acquire) {
            // A file of that name without the export is not the daemon, and
            // as far as a player is concerned it is missing.
            report_daemon_missing(path);
            return false;
        }

        // Never from DllMain: the election takes a lock and may LoadLibrary,
        // which deadlocks under the loader lock. This runs from Open, on the
        // game's main thread, after LoadLibraryW has returned.
        //
        // The test inside is a range, not equality, so addons on different
        // release cadences can share one client.
        const WdDaemonApi* const api = acquire(WD_DAEMON_ABI);
        if (!api || api->size < sizeof(WdDaemonApi) || !api->ensure_hooks
            || !api->register_set || !api->unregister_set) {
            report_daemon_refusal();
            return false;
        }

        daemon_api_ = api;
        daemon_failed_ = false;
        return true;
    }

    void fill_handler_set() {
        std::memset(&handler_set_, 0, sizeof(handler_set_));
        handler_set_.abi_version = WD_DAEMON_ABI;
        handler_set_.size = static_cast<std::uint32_t>(sizeof(WdHandlerSet));
        handler_set_.user = this;
        handler_set_.pre_reset = &daemon_pre_reset;
        handler_set_.post_reset = &daemon_post_reset;
        handler_set_.pre_set_render_target = &daemon_pre_set_render_target;
        handler_set_.pre_draw = &daemon_pre_draw;
    }

    // One reference on this image, ever, and it is never given back. It exists
    // so a registration can never outlive the code it points into.
    //
    // The release cannot be made safe, so there is none: the only place one
    // could happen is a teardown path, which in a Lua front-end is also the
    // handle's __gc, and if ours is the last reference by then FreeLibrary
    // unmaps the image out from under the instruction pointer.
    //
    // The costs are accepted: once attached, this image stays mapped for the
    // life of the client, its process-wide statics survive an addon reload
    // (detach_from_daemon must leave every one of them in its unattached
    // state), and a mapped image cannot be overwritten in place -- which is why
    // lua/deploy.sh deploys by rename rather than by copy.
    static bool take_self_reference() {
        if (self_reference_) {
            return true;
        }

        WCHAR path[MAX_PATH] {};
        if (!self_module(path, MAX_PATH)) {
            return false;
        }

        self_reference_ = FFXI_WORLD_DRAW_LOAD_LIBRARY_W(path);
        return self_reference_ != nullptr;
    }

    // Where this image goes live: the daemon, the device, the six slots, the
    // self-reference and the registration, in that order because each needs the
    // one before it. A failure here is final unless it is the absent device.
    bool attach_to_daemon() {
        if (hook_installed_) {
            return true;
        }

        if (!EnsureDaemon()) {
            hook_install_failed_ = true;
            return false;
        }

        if (!EnsureDevice()) {
            return false;
        }

        if (!daemon_api_->ensure_hooks(d3d_device_)) {
            report_setup_failure(
                "hook: the daemon patched nothing, this device is not ours to hook");
            hook_install_failed_ = true;
            return false;
        }

        fill_handler_set();

        // Taken before a handler of ours can be called, so a registered set can
        // never point into an unmapped image.
        if (!take_self_reference()) {
            report_setup_failure(
                "hook: this image could not reference itself, refusing to register");
            hook_install_failed_ = true;
            return false;
        }

        // A 1-based slot index; 0 is a refusal, which nothing an addon does can
        // provoke -- the daemon's table of registrations grows on demand.
        if (!daemon_api_->register_set(&handler_set_)) {
            // The reference stays, as it does on every path.
            report_setup_failure("hook: the daemon refused the registration");
            hook_install_failed_ = true;
            return false;
        }

        hook_owner_ = this;
        hook_installed_ = true;
        hook_install_failed_ = false;
        hook_armed_ = true;
        return true;
    }

    // The mirror. unregister_set drains: once it returns, no thunk is inside a
    // handler of this image, which is what makes the releases below safe.
    // Never call it while a front-end holds its render-side lock -- the drain
    // waits on handlers that take it.
    void detach_from_daemon() {
        if (!hook_installed_) {
            return;
        }

        hook_armed_ = false;
        WorldDrawPlugin* const owner = hook_owner_;
        hook_owner_ = nullptr;
        hook_installed_ = false;

        if (daemon_api_ && owner) {
            daemon_api_->unregister_set(&owner->handler_set_);
        }

        // Past the drain nothing can still be drawing, so the device resources
        // go -- queued, this being the main thread. The registering instance is
        // not always the one taking the registration out, so both are cleared.
        if (owner) {
            owner->defer_dynamic_vb();
            owner->defer_gpu_shader();
        }
        defer_dynamic_vb();
        defer_gpu_shader();

        // The self-reference is not dropped here, or anywhere: this is reached
        // from a Lua handle's __gc. See take_self_reference.
    }

    bool is_world_pass(IDirect3DDevice8* device) {
        if (!device) {
            return false;
        }
        if (!pass_transforms_valid_ || std::fabs(pass_projection_.m[3][3]) > 1.0e-4f) {
            return false;
        }

        const float translation = std::fabs(pass_view_.m[3][0])
            + std::fabs(pass_view_.m[3][1]) + std::fabs(pass_view_.m[3][2]);
        if (translation <= 1.0e-3f) {
            return false;
        }
        const std::uintptr_t scene_depth = scene_depth_surface();
        return scene_depth != 0
            && reinterpret_cast<std::uintptr_t>(depth_surface_of(device)) == scene_depth;
    }
    std::uintptr_t scene_depth_surface() const {
        if (renderer_global_ == 0) {
            return 0;
        }

        std::uint32_t renderer = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer_global_), renderer) || renderer == 0) {
            return 0;
        }

        std::uint32_t depth = 0;
        if (!read_memory(static_cast<std::uintptr_t>(renderer) + renderer_scene_depth_offset_, depth)) {
            return 0;
        }

        return depth;
    }
    static const void* depth_surface_of(IDirect3DDevice8* device) {
        IDirect3DSurface8* depth = nullptr;
        if (!device || FAILED(device->GetDepthStencilSurface(&depth)) || !depth) {
            return nullptr;
        }
        const void* identity = depth;
        depth->Release();
        return identity;
    }

    void expand_pair(float& minus_x, float& minus_y, float& plus_x, float& plus_y,
        float normal_x, float normal_y) const {
        const float delta_x = plus_x - minus_x;
        const float delta_y = plus_y - minus_y;
        const float distance = std::sqrt(delta_x * delta_x + delta_y * delta_y);
        if (distance >= min_projected_width_) {
            return;
        }

        const float center_x = (minus_x + plus_x) * 0.5f;
        const float center_y = (minus_y + plus_y) * 0.5f;
        const float half_target = min_projected_width_ * 0.5f;

        if (distance > 1.0e-3f) {
            const float scale = half_target / distance;
            minus_x = center_x - delta_x * scale;
            minus_y = center_y - delta_y * scale;
            plus_x = center_x + delta_x * scale;
            plus_y = center_y + delta_y * scale;
            return;
        }

        minus_x = center_x - normal_x * half_target;
        minus_y = center_y - normal_y * half_target;
        plus_x = center_x + normal_x * half_target;
        plus_y = center_y + normal_y * half_target;
    }

    bool refresh_projection_matrices(IDirect3DDevice8* device) {
        projection_matrices_valid_ = false;

        if (!device) {
            return false;
        }

        if (!pass_transforms_valid_) {
            return false;
        }
        if (!frame_transforms_valid_) {
            frame_view_ = pass_view_;
            frame_projection_ = pass_projection_;
            frame_transforms_valid_ = true;
        }

        cached_view_ = frame_view_;
        cached_projection_ = frame_projection_;
        if (cached_projection_.m[3][3] != 0.0f) {
            return false;
        }

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                cached_view_projection_.m[row][column] =
                    cached_view_.m[row][0] * cached_projection_.m[0][column]
                    + cached_view_.m[row][1] * cached_projection_.m[1][column]
                    + cached_view_.m[row][2] * cached_projection_.m[2][column]
                    + cached_view_.m[row][3] * cached_projection_.m[3][column];
            }
        }

        projection_matrices_valid_ = true;
        return true;
    }

    bool live_world_to_screen(float lua_x, float lua_y, float lua_z, const D3DVIEWPORT8& viewport,
        float& screen_x, float& screen_y, float& depth, float& rhw) {
        if (!projection_matrices_valid_) {
            return false;
        }

        const D3DMATRIX& view_projection = cached_view_projection_;
        const float world_x = lua_x;
        const float world_y = lua_z;
        const float world_z = lua_y;

        const float clip_x = world_x * view_projection.m[0][0] + world_y * view_projection.m[1][0]
            + world_z * view_projection.m[2][0] + view_projection.m[3][0];
        const float clip_y = world_x * view_projection.m[0][1] + world_y * view_projection.m[1][1]
            + world_z * view_projection.m[2][1] + view_projection.m[3][1];
        const float clip_z = world_x * view_projection.m[0][2] + world_y * view_projection.m[1][2]
            + world_z * view_projection.m[2][2] + view_projection.m[3][2];
        const float clip_w = world_x * view_projection.m[0][3] + world_y * view_projection.m[1][3]
            + world_z * view_projection.m[2][3] + view_projection.m[3][3];

        if (std::fabs(clip_w) <= 0.0001f) {
            return false;
        }

        const float ndc_x = clip_x / clip_w;
        const float ndc_y = clip_y / clip_w;
        // A visibility test, not a limit: the margin is wide enough that a quad
        // with one corner off screen still projects whole.
        if (clip_w < 0.0f || ndc_x < -4.0f || ndc_x > 4.0f || ndc_y < -4.0f || ndc_y > 4.0f) {
            return false;
        }

        screen_x = static_cast<float>(viewport.X) + (ndc_x + 1.0f) * static_cast<float>(viewport.Width) * 0.5f;
        screen_y = static_cast<float>(viewport.Y) + (1.0f - ndc_y) * static_cast<float>(viewport.Height) * 0.5f;
        const float ndc_z = clip_z / clip_w;
        const float span = viewport.MaxZ - viewport.MinZ;
        depth = viewport.MinZ + ndc_z * span;
        if (depth < 0.0f) {
            depth = 0.0f;
        } else if (depth > 1.0f) {
            depth = 1.0f;
        }
        rhw = 1.0f / clip_w;
        return true;
    }
    bool begin_draw_state(IDirect3DDevice8* device) {
        if (draw_state_active_ || !device) {
            return false;
        }

        saved_texture_ = nullptr;
        saved_stream_ = nullptr;
        saved_stream_stride_ = 0;
        // GetVertexShader reports an FVF code or a shader handle and
        // SetVertexShader takes either, so this one field covers both paths.
        device->GetVertexShader(&saved_shader_);

        // Whether the read worked is remembered: writing zeroes back to a
        // device that would not report them is worse than restoring nothing.
        saved_constants_valid_ = SUCCEEDED(
            device->GetVertexShaderConstant(0, saved_constants_, gpu_constant_count_));

        device->GetRenderState(D3DRS_ALPHABLENDENABLE, &saved_alpha_);
        device->GetRenderState(D3DRS_SRCBLEND, &saved_src_);
        device->GetRenderState(D3DRS_DESTBLEND, &saved_dest_);
        device->GetRenderState(D3DRS_ZENABLE, &saved_z_);
        device->GetRenderState(D3DRS_ZWRITEENABLE, &saved_zwrite_);
        device->GetRenderState(D3DRS_ZFUNC, &saved_zfunc_);
        device->GetRenderState(D3DRS_ZBIAS, &saved_zbias_);
        device->GetRenderState(D3DRS_LIGHTING, &saved_lighting_);
        device->GetRenderState(D3DRS_CULLMODE, &saved_cull_);
        device->GetTexture(0, &saved_texture_);
        device->GetRenderState(D3DRS_ALPHATESTENABLE, &saved_alphatest_);
        device->GetRenderState(D3DRS_FOGENABLE, &saved_fog_);
        device->GetRenderState(D3DRS_STENCILENABLE, &saved_stencil_);
        device->GetTextureStageState(0, D3DTSS_COLOROP, &saved_colorop_);
        device->GetTextureStageState(0, D3DTSS_COLORARG1, &saved_colorarg1_);
        device->GetTextureStageState(0, D3DTSS_COLORARG2, &saved_colorarg2_);
        device->GetTextureStageState(0, D3DTSS_ALPHAOP, &saved_alphaop_);
        device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &saved_alphaarg1_);
        device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &saved_alphaarg2_);
        device->GetTextureStageState(1, D3DTSS_COLOROP, &saved_stage1_colorop_);

        // GetStreamSource AddRefs what it hands back, and that reference is
        // ours to drop -- end_draw_state does.
        device->GetStreamSource(0, &saved_stream_, &saved_stream_stride_);

        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

        draw_state_active_ = true;
        return true;
    }

    void end_draw_state(IDirect3DDevice8* device) {
        if (!draw_state_active_ || !device) {
            return;
        }

        device->SetTexture(0, saved_texture_);
        if (saved_texture_) {
            saved_texture_->Release();
            saved_texture_ = nullptr;
        }
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, saved_alpha_);
        device->SetRenderState(D3DRS_SRCBLEND, saved_src_);
        device->SetRenderState(D3DRS_DESTBLEND, saved_dest_);
        device->SetRenderState(D3DRS_ZENABLE, saved_z_);
        device->SetRenderState(D3DRS_ZWRITEENABLE, saved_zwrite_);
        device->SetRenderState(D3DRS_ZFUNC, saved_zfunc_);
        device->SetRenderState(D3DRS_ZBIAS, saved_zbias_);
        device->SetRenderState(D3DRS_LIGHTING, saved_lighting_);
        device->SetRenderState(D3DRS_CULLMODE, saved_cull_);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, saved_alphatest_);
        device->SetRenderState(D3DRS_FOGENABLE, saved_fog_);
        device->SetRenderState(D3DRS_STENCILENABLE, saved_stencil_);
        device->SetTextureStageState(0, D3DTSS_COLOROP, saved_colorop_);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, saved_colorarg1_);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, saved_colorarg2_);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, saved_alphaop_);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, saved_alphaarg1_);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, saved_alphaarg2_);
        device->SetTextureStageState(1, D3DTSS_COLOROP, saved_stage1_colorop_);

        // A null saved stream is the ordinary case, and setting it back
        // restores exactly what was there.
        device->SetStreamSource(0, saved_stream_, saved_stream_stride_);
        if (saved_stream_) {
            saved_stream_->Release();
            saved_stream_ = nullptr;
        }
        device->SetVertexShader(saved_shader_);
        if (saved_constants_valid_) {
            device->SetVertexShaderConstant(0, saved_constants_, gpu_constant_count_);
            saved_constants_valid_ = false;
        }
        draw_state_active_ = false;
    }

    // Made on first use and kept for the life of the device. A failure is a
    // property of the device, so it is reported once and DrawPrimitiveUP is
    // used from then on, never retried per frame. Reset clears the flag.
    bool ensure_dynamic_vb(IDirect3DDevice8* device) {
        if (dynamic_vb_) {
            return true;
        }
        if (dynamic_vb_failed_ || !device) {
            return false;
        }

        IDirect3DVertexBuffer8* vertex_buffer = nullptr;
        if (FAILED(device->CreateVertexBuffer(
                static_cast<UINT>(static_cast<std::size_t>(vertex_batch_) * sizeof(Vertex)),
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                D3DPOOL_DEFAULT, &vertex_buffer))
            || !vertex_buffer) {
            dynamic_vb_ = nullptr;
            dynamic_vb_offset_ = 0;
            dynamic_vb_failed_ = true;
            report_error("draw: dynamic vertex buffer unavailable, drawing through DrawPrimitiveUP");
            return false;
        }

        dynamic_vb_ = vertex_buffer;
        dynamic_vb_offset_ = 0;
        return true;
    }

    // Render thread only, and the one release in this file that may not be
    // deferred: the buffer is D3DPOOL_DEFAULT and has to be gone before the
    // Reset. Its only caller, daemon_pre_reset, is on the render thread.
    void release_dynamic_vb() {
        if (dynamic_vb_) {
            dynamic_vb_->Release();
            dynamic_vb_ = nullptr;
        }
        dynamic_vb_offset_ = 0;
        dynamic_vb_failed_ = false;
    }

    // The same from a teardown path, which is on the main thread, so the
    // Release is queued. drain_deferred runs at the top of daemon_pre_reset as
    // well, so a Reset arriving first still finds this buffer gone.
    void defer_dynamic_vb() {
        if (dynamic_vb_) {
            DeferVertexBufferRelease(dynamic_vb_);
            dynamic_vb_ = nullptr;
        }
        dynamic_vb_offset_ = 0;
        dynamic_vb_failed_ = false;
    }

    // The mirror of defer_dynamic_vb, from the same teardown paths. Not called
    // from the Reset handler: the buffer has to go there and a shader handle
    // does not. DeleteVertexShader is a device method and these paths are on
    // the main thread, so the handles and their device are queued.
    void defer_gpu_shader() {
        defer_shader(gpu_shader_device_, gpu_shader_, gpu_line_shader_);
        gpu_shader_ = 0;
        gpu_line_shader_ = 0;
        gpu_shader_device_ = nullptr;
        gpu_shader_failed_ = false;
    }

    // True when the batch went out through the ring. False means nothing was
    // drawn and nothing was left locked, and the caller falls through to
    // DrawPrimitiveUP.
    bool submit_through_dynamic_vb(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive_type,
        UINT primitive_count, const Vertex* vertices, UINT stride) {
        // The ring holds one FVF at one stride, and only a triangle list turns
        // a primitive count into a vertex count on its own.
        if (primitive_type != D3DPT_TRIANGLELIST || stride != sizeof(Vertex)) {
            return false;
        }

        // A guard, not a live case. Tested against primitive_count so the
        // multiply below cannot wrap.
        const UINT capacity = static_cast<UINT>(vertex_batch_);
        if (primitive_count > capacity / 3u) {
            return false;
        }

        if (!ensure_dynamic_vb(device)) {
            return false;
        }

        const UINT vertex_count = primitive_count * 3u;
        const UINT vertex_bytes = static_cast<UINT>(sizeof(Vertex));
        const UINT bytes = vertex_count * vertex_bytes;

        // No room left: discard the whole buffer rather than stall on the
        // batches already queued this frame. Otherwise write in behind them,
        // promising not to disturb what they are still drawing from.
        BYTE* mapped = nullptr;
        if (dynamic_vb_offset_ + vertex_count > capacity) {
            if (FAILED(dynamic_vb_->Lock(0, bytes, &mapped, D3DLOCK_DISCARD)) || !mapped) {
                return false;
            }
            dynamic_vb_offset_ = 0;
        } else if (FAILED(dynamic_vb_->Lock(dynamic_vb_offset_ * vertex_bytes, bytes, &mapped,
                D3DLOCK_NOOVERWRITE)) || !mapped) {
            return false;
        }

        std::memcpy(mapped, vertices, bytes);
        if (FAILED(dynamic_vb_->Unlock())) {
            return false;
        }

        if (FAILED(device->SetStreamSource(0, dynamic_vb_, vertex_bytes))
            || FAILED(device->DrawPrimitive(primitive_type, dynamic_vb_offset_, primitive_count))) {
            return false;
        }

        dynamic_vb_offset_ += vertex_count;
        return true;
    }

    HRESULT submit_vertices(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive_type,
        UINT primitive_count, const Vertex* vertices, UINT stride) {
        if (!device || !draw_state_active_ || primitive_count == 0 || !vertices) {
            return E_INVALIDARG;
        }

        // The ring is only ever the faster road: whatever it cannot take, and
        // any failure inside it, drops through to the call below.
        if (submit_through_dynamic_vb(device, primitive_type, primitive_count, vertices, stride)) {
            return D3D_OK;
        }

        return device->DrawPrimitiveUP(primitive_type, primitive_count, vertices, stride);
    }

    bool is_readable_page(DWORD protect) const {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS)) {
            return false;
        }

        const DWORD base = protect & 0xFF;
        return base == PAGE_READONLY ||
            base == PAGE_READWRITE ||
            base == PAGE_WRITECOPY ||
            base == PAGE_EXECUTE_READ ||
            base == PAGE_EXECUTE_READWRITE ||
            base == PAGE_EXECUTE_WRITECOPY;
    }

    bool is_readable_range(std::uintptr_t address, std::size_t size) const {
        if (address == 0 || size == 0) {
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) {
            return false;
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t end = base + mbi.RegionSize;
        return mbi.State == MEM_COMMIT &&
            is_readable_page(mbi.Protect) &&
            address >= base &&
            address + size <= end;
    }
    bool is_readable_span(std::uintptr_t address, std::size_t size) const {
        if (address == 0 || size == 0) {
            return false;
        }

        const std::uintptr_t end = address + size;
        if (end < address) {
            return false;
        }

        std::uintptr_t cursor = address;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION mbi {};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) {
                return false;
            }

            const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (region_end <= cursor || mbi.State != MEM_COMMIT || !is_readable_page(mbi.Protect)) {
                return false;
            }

            cursor = region_end;
        }

        return true;
    }

    bool read_bytes(std::uintptr_t address, void* output, std::size_t size) const {
        if (!output || !is_readable_span(address, size)) {
            return false;
        }

        SIZE_T bytes_read = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, size, &bytes_read) &&
            bytes_read == size;
    }

    std::uintptr_t scan_span(std::uintptr_t span_begin, std::uintptr_t span_end,
        const std::uint8_t* pattern, const char* mask, std::size_t length) {
        if (length == 0 || length > scan_chunk_size_ || span_end <= span_begin
            || (span_end - span_begin) < length) {
            return 0;
        }

        std::uintptr_t position = span_begin;
        while (position + length <= span_end) {
            const std::size_t remaining = static_cast<std::size_t>(span_end - position);
            const std::size_t chunk = remaining < scan_chunk_size_ ? remaining : scan_chunk_size_;
            if (!read_bytes(position, scan_buffer_, chunk)) {
                return 0;
            }

            const std::size_t last_candidate = chunk - length;
            std::size_t cursor = 0;
            while (cursor <= last_candidate) {
                const void* found = std::memchr(scan_buffer_ + cursor, pattern[0],
                    last_candidate - cursor + 1);
                if (!found) {
                    break;
                }

                const std::size_t candidate =
                    static_cast<std::size_t>(static_cast<const std::uint8_t*>(found) - scan_buffer_);
                std::size_t offset = 1;
                for (; offset < length; ++offset) {
                    if (mask[offset] == 'x' && scan_buffer_[candidate + offset] != pattern[offset]) {
                        break;
                    }
                }

                if (offset == length) {
                    return position + candidate;
                }

                cursor = candidate + 1;
            }

            if (chunk < scan_chunk_size_) {
                break;
            }

            position += scan_chunk_size_ - (length - 1);
        }

        return 0;
    }

    void ensure_resolution() {
        if (resolution_attempted_) {
            return;
        }

        resolution_attempted_ = true;
        scan_resolved_ = false;
        renderer_global_ = 0;

        std::uintptr_t image_base = 0;
        std::size_t image_size = 0;
        if (!get_module_image("FFXiMain.dll", image_base, image_size)) {
            report_setup_failure("scan: FFXiMain.dll image unavailable");
            return;
        }

        const std::uintptr_t renderer_hit = scan_module(image_base, image_size,
            renderer_pattern_, renderer_mask_, sizeof(renderer_pattern_));
        std::uint32_t renderer_global = 0;
        if (renderer_hit) {
            read_memory(renderer_hit + 2, renderer_global);
        }
        renderer_global_ = renderer_global;
        scan_resolved_ = renderer_global_ != 0;

        // Without the renderer a front-end with no PluginManager never gets a
        // device at all. The three numbers behind the player-facing lines are
        // the only record of where the scan failed.
        if (!scan_resolved_) {
            char details[128] {};
            std::snprintf(details, sizeof(details),
                "scan: renderer not found (module=0x%08lX size=0x%08lX hit=0x%08lX)",
                static_cast<unsigned long>(image_base), static_cast<unsigned long>(image_size),
                static_cast<unsigned long>(renderer_hit));
            report_setup_failure(details);
        }
    }

    // What every setup failure with no better answer says. report_setup_failure
    // appends exactly one more line, `details:`, carrying the technical string.
    static constexpr const char* setup_failed_message_ =
        "worlddraw can't draw: it failed to start.\n"
        "Restart FFXI. If it happens again, please report it.";

    // The narrowest a bar may appear, in screen pixels, before both paths widen
    // it -- expand_pair on the CPU and the width floor in the shader. Below
    // this a hairline bar shimmers in and out instead of drawing every frame.
    static constexpr float min_projected_width_ = 1.5f;
    // c0-c11. begin_draw_state and end_draw_state read this same count, so
    // every constant is saved and put back.
    static constexpr DWORD gpu_constant_count_ = 12;
    // The window scan_span reads the game's image through. Not a limit on what
    // can be scanned, only on how long a pattern may be.
    static constexpr std::size_t scan_chunk_size_ = 0x10000;
    static constexpr std::uint8_t renderer_pattern_[12] = {
        0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x89, 0xB9, 0x94, 0x01, 0x00, 0x00};
    static constexpr char renderer_mask_[13] = "xx????xxxxxx";
    static constexpr std::uintptr_t renderer_scene_depth_offset_ = 0x1A4;
    static constexpr std::uintptr_t renderer_device_offset_ = 0x0C;
    // Which instance the registration belongs to, and the state of the pass it
    // sees. One copy per image, shared by every plugin built on this header
    // inside it: the daemon holds no pass state of its own.
    inline static WorldDrawPlugin* hook_owner_ = nullptr;
    inline static bool hook_installed_ = false;
    inline static bool hook_install_failed_ = false;
    inline static bool hook_armed_ = false;
    inline static bool hook_drawing_ = false;
    // The daemon this image acquired and the reference it holds on itself, per
    // image like the hook state above: one daemon, one registration.
    //
    // module_anchor_ is an address certain to be inside this image, for the
    // FROM_ADDRESS lookups. A data anchor, not a function's address: casting a
    // function pointer to a data pointer is not portable C++.
    inline static const char module_anchor_ = 'w';
    inline static const WdDaemonApi* daemon_api_ = nullptr;
    inline static bool daemon_failed_ = false;
    inline static HMODULE self_reference_ = nullptr;

    // Frames between two stomp polls, and whether one has been reported.
    // Frames rather than a clock: this counts ticks of the thing it watches.
    static constexpr unsigned stomp_poll_interval_ = 300;
    inline static unsigned stomp_poll_frames_ = 0;
    inline static bool stomp_reported_ = false;

    // One per image, not one per instance: the device belongs to the client.
    inline static bool device_behavior_reported_ = false;

    // Per instance, because its `user` is this instance. The daemon holds the
    // pointer while registered and never touches it again once unregister_set
    // has returned.
    WdHandlerSet handler_set_ {};
    inline static D3DMATRIX pass_view_ {};
    inline static D3DMATRIX pass_projection_ {};
    inline static bool pass_transforms_valid_ = false;
    inline static D3DMATRIX frame_view_ {};
    inline static D3DMATRIX frame_projection_ {};
    inline static bool frame_transforms_valid_ = false;


    IDirect3DDevice8* d3d_device_ = nullptr;
    D3DMATRIX cached_view_ {};
    D3DMATRIX cached_projection_ {};
    D3DMATRIX cached_view_projection_ {};
    bool projection_matrices_valid_ = false;

    // Set for the length of OnWorldDraw. The device goes back to nullptr after
    // the callback; the right vector is whatever the last composite used.
    IDirect3DDevice8* dispatch_device_ = nullptr;
    float dispatch_right_x_ = 1.0f;
    float dispatch_right_y_ = 0.0f;
    float dispatch_forward_[3] = {0.0f, 0.0f, 1.0f};

    DWORD saved_shader_ = 0;
    float saved_constants_[gpu_constant_count_][4] {};
    bool saved_constants_valid_ = false;
    DWORD saved_alpha_ = 0;
    DWORD saved_src_ = 0;
    DWORD saved_dest_ = 0;
    DWORD saved_z_ = 0;
    DWORD saved_zwrite_ = 0;
    DWORD saved_zfunc_ = 0;
    DWORD saved_zbias_ = 0;
    DWORD saved_lighting_ = 0;
    DWORD saved_cull_ = 0;
    DWORD saved_alphatest_ = 0;
    DWORD saved_fog_ = 0;
    DWORD saved_stencil_ = 0;
    DWORD saved_colorop_ = 0;
    DWORD saved_colorarg1_ = 0;
    DWORD saved_colorarg2_ = 0;
    DWORD saved_alphaop_ = 0;
    DWORD saved_alphaarg1_ = 0;
    DWORD saved_alphaarg2_ = 0;
    DWORD saved_stage1_colorop_ = 0;
    IDirect3DBaseTexture8* saved_texture_ = nullptr;
    IDirect3DVertexBuffer8* saved_stream_ = nullptr;
    UINT saved_stream_stride_ = 0;
    bool draw_state_active_ = false;
    bool resolution_attempted_ = false;
    bool scan_resolved_ = false;
    std::uint32_t renderer_global_ = 0;
    std::uint8_t scan_buffer_[scan_chunk_size_] {};

    TextureBlock* texture_blocks_ = nullptr;
#ifdef FFXI_WORLD_DRAW_IMAGE_LOADING
    ULONG_PTR gdiplus_token_ = 0;
#endif
};

inline bool WorldDraw::Project(float x, float y, float z, DWORD color, Vertex& out,
                               float u, float v) const {
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    float depth = 0.0f;
    float rhw = 1.0f;
    if (!owner_ || !owner_->live_world_to_screen(x, y, z, viewport_, screen_x, screen_y, depth, rhw)) {
        return false;
    }

    out.x = screen_x;
    out.y = screen_y;
    out.z = depth;
    out.rhw = rhw;
    out.color = color;
    out.u = u;
    out.v = v;
    return true;
}

inline void WorldDraw::Flush() {
    if (owner_ && device_ && count_ >= 3) {
        owner_->apply_batch_state(device_, texture_, depth_write_, culling_);
        owner_->submit_batch(device_, buffer_, count_);
    }
    count_ = 0;
}

inline void WorldDraw::SetTexture(TextureId id) {
    if (id != texture_) {
        Flush();
        texture_ = id;
    }
}

inline void WorldDraw::SetDepthWrite(bool enabled) {
    if (enabled != depth_write_) {
        Flush();
        depth_write_ = enabled;
    }
}

inline void WorldDraw::SetCulling(bool enabled) {
    if (enabled != culling_) {
        Flush();
        culling_ = enabled;
    }
}

inline void WorldDraw::SetSolid(bool solid) {
    SetDepthWrite(solid);
    SetCulling(solid);
}

inline void WorldDraw::TexturedQuad(const Vertex& a, const Vertex& b, const Vertex& c,
                                    const Vertex& d, TextureId texture) {
    SetTexture(texture);
    Quad(a, b, c, d);
}

inline bool WorldDraw::Sprite(float x, float y, float z, float width, float height,
                              TextureId texture, DWORD color) {
    const float half = width * 0.5f;
    const float offset_x = right_x_ * half;
    const float offset_y = right_y_ * half;

    // Centred on the point, like ScreenSprite. Negative height is up.
    const float half_height = height * 0.5f;
    const float bottom = z + half_height;
    const float top = z - half_height;

    Vertex a, b, c, d;
    if (!Project(x - offset_x, y - offset_y, bottom, color, a, 0.0f, 1.0f)
        || !Project(x + offset_x, y + offset_y, bottom, color, b, 1.0f, 1.0f)
        || !Project(x - offset_x, y - offset_y, top, color, c, 0.0f, 0.0f)
        || !Project(x + offset_x, y + offset_y, top, color, d, 1.0f, 0.0f)) {
        return false;
    }

    TexturedQuad(a, b, c, d, texture);
    return true;
}

inline bool WorldDraw::Panel(float x, float y, float z, float width, float height,
                             float facing, TextureId texture, DWORD color) {
    // The width runs across the way the panel faces.
    const float across_x = -std::sin(facing);
    const float across_y = std::cos(facing);
    const float half_width = width * 0.5f;
    const float offset_x = across_x * half_width;
    const float offset_y = across_y * half_width;

    const float half_height = height * 0.5f;
    const float bottom = z + half_height;
    const float top = z - half_height;

    Vertex a, b, c, d;
    if (!Project(x - offset_x, y - offset_y, bottom, color, a, 0.0f, 1.0f)
        || !Project(x + offset_x, y + offset_y, bottom, color, b, 1.0f, 1.0f)
        || !Project(x - offset_x, y - offset_y, top, color, c, 0.0f, 0.0f)
        || !Project(x + offset_x, y + offset_y, top, color, d, 1.0f, 0.0f)) {
        return false;
    }

    TexturedQuad(a, b, c, d, texture);
    return true;
}

inline bool WorldDraw::ScreenSprite(float x, float y, float z, float width, float height,
                                    TextureId texture, DWORD color) {
    Vertex centre;
    if (!Project(x, y, z, color, centre)) {
        return false;
    }

    const float half_w = width * 0.5f;
    const float half_h = height * 0.5f;

    Vertex a = centre, b = centre, c = centre, d = centre;
    a.x -= half_w; a.y += half_h; a.u = 0.0f; a.v = 1.0f;
    b.x += half_w; b.y += half_h; b.u = 1.0f; b.v = 1.0f;
    c.x -= half_w; c.y -= half_h; c.u = 0.0f; c.v = 0.0f;
    d.x += half_w; d.y -= half_h; d.u = 1.0f; d.v = 0.0f;

    TexturedQuad(a, b, c, d, texture);
    return true;
}

inline void WorldDraw::Triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    if (!buffer_) {
        return;
    }

    // Full batch: send it and carry on. There is no cap on how much geometry a
    // frame may contain.
    if (count_ + 3 > capacity_) {
        Flush();
    }

    buffer_[count_++] = a;
    buffer_[count_++] = b;
    buffer_[count_++] = c;
}

inline void WorldDraw::Quad(const Vertex& a, const Vertex& b, const Vertex& c, const Vertex& d) {
    Triangle(a, b, c);
    Triangle(c, b, d);
}

inline bool WorldDraw::Pillar(float x, float y, float z, float width, float height, DWORD color) {
    return Line(x, y, z, x, y, z - height, width, color);
}

inline bool WorldDraw::Line(float x1, float y1, float z1, float x2, float y2, float z2,
                            float width, DWORD color) {
    const float half = width * 0.5f;
    const float offset_x = right_x_ * half;
    const float offset_y = right_y_ * half;

    Vertex a, b, c, d;
    if (!Project(x1 - offset_x, y1 - offset_y, z1, color, a)
        || !Project(x1 + offset_x, y1 + offset_y, z1, color, b)
        || !Project(x2 - offset_x, y2 - offset_y, z2, color, c)
        || !Project(x2 + offset_x, y2 + offset_y, z2, color, d)) {
        return false;
    }

    if (owner_) {
        const float axis_x = c.x - a.x;
        const float axis_y = c.y - a.y;
        const float axis_length = std::sqrt(axis_x * axis_x + axis_y * axis_y);
        float normal_x = 1.0f;
        float normal_y = 0.0f;
        if (axis_length > 1.0e-3f) {
            normal_x = -axis_y / axis_length;
            normal_y = axis_x / axis_length;
        }
        owner_->expand_pair(a.x, a.y, b.x, b.y, normal_x, normal_y);
        owner_->expand_pair(c.x, c.y, d.x, d.y, normal_x, normal_y);
    }

    Quad(a, b, c, d);
    return true;
}

inline bool WorldDraw::Ring(float x, float y, float z, float radius, float thickness,
                            DWORD color, int segments) {
    // A count, and the angle below divides by it. Capped from above only so
    // `i <= segments` can terminate; the batch flushes as often as it has to,
    // so a ring is as fine as the caller asks for.
    if (segments < 1) {
        segments = 1;
    } else if (segments > INT_MAX - 1) {
        segments = INT_MAX - 1;
    }

    const float top = z - thickness;
    bool drew = false;
    Vertex previous_bottom, previous_top;
    bool have_previous = false;

    for (int i = 0; i <= segments; ++i) {
        const float angle = 6.28318530718f * static_cast<float>(i) / static_cast<float>(segments);
        const float point_x = x + radius * std::cos(angle);
        const float point_y = y + radius * std::sin(angle);

        Vertex bottom, band_top;
        const bool ok = Project(point_x, point_y, z, color, bottom)
            && Project(point_x, point_y, top, color, band_top);
        if (ok && have_previous) {
            Quad(previous_bottom, bottom, previous_top, band_top);
            drew = true;
        }

        previous_bottom = bottom;
        previous_top = band_top;
        have_previous = ok;
    }

    return drew;
}

}  // namespace ffxi
