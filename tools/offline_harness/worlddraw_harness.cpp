// worlddraw_harness - exercises the CPU half of lua/worlddraw.cpp without a
// graphics device: tessellation, the Windower-to-Direct3D axis swap, range
// grouping, staging growth, mesh staging and the mesh placement matrices.
//
// It #includes the real worlddraw.cpp, so what is tested is the shipping code
// and not a copy of it. The Lua C API is stubbed below purely so the module's
// Lua entry points link; nothing here calls one.
//
//   ./build.sh      builds it and runs it under wine; non-zero if a check fails

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <csetjmp>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

// ---- Lua C API stubs ------------------------------------------------------
// Enough symbols for the module to link. Outside a call_entry below, every one
// of them aborts if it is ever called, so a test that reached Lua by accident
// fails loudly rather than quietly measuring a stub.
namespace {
void stub_called(const char* name) {
    std::printf("FAIL   : the harness called into the Lua stub %s\n", name);
    std::fflush(stdout);
    std::abort();
}
}  // namespace

// ---- a Lua state just real enough to call an entry point ------------------
//
// Most of what this harness checks is reachable underneath the Lua layer, and
// is checked there. Three of the questions it now has to answer are not:
//
//   does d:commit() with no device raise, or report and return false?
//   does m:build() with no device raise, or report and return false?
//   does a handle that has been closed still raise, as it must?
//
// "Raises" is not observable from below -- it is a longjmp out of luaL_error --
// so those are asked through the shipping entry points with a state behind
// them that answers the handful of calls they make. Nothing here interprets
// Lua: luaL_checkudata hands back the object the test set, luaL_error records
// and longjmps the way the real one does, and the pushes are recorded so a
// return value can be read.
//
// The longjmp crosses only frames of the entry points themselves, which is
// exactly what it crosses in the client -- LuaCore is C, and every one of
// these functions is written to be left that way.
namespace harness {

// ---- the thread gate ------------------------------------------------------
//
// The blocker this file exists to keep closed: FFXI's device is created
// without D3DCREATE_MULTITHREADED and its hooks run on a different OS thread
// from Windower's Lua, so a device method called from a Lua entry point is an
// access violation inside d3d8.dll on a real client. No lock can fix it -- the
// game's render thread calls the same device and cannot be made to take ours
// -- so the rule is absolute: no device method, ever, from an l_* entry point.
//
// Every method of every device-shaped object below announces itself here.
// call_entry -- which is how this harness invokes an l_* entry point, and the
// only way it does -- puts the harness in the Lua role for the length of the
// call, and MainThread does the same around the header calls a plugin makes on
// the same thread. Anything that reaches a device inside one of those windows
// is named and counted, and the run fails.
//
// It is a behavioural gate, so it catches a device call however it got there:
// through a helper, through a header member, through a path nobody thought of.
// tools/offline_harness/gate_device_calls.py is the static half of the same
// question, over the preprocessed text.
enum ThreadRole {
    role_render,   // the daemon's handlers, and the composite they reach
    role_main,     // Windower's Lua thread, and a plugin's own calls
};

ThreadRole role_ = role_render;
int main_thread_device_calls_ = 0;
const char* first_main_thread_call_ = nullptr;

struct MainThread {
    ThreadRole saved;
    MainThread() : saved(role_) { role_ = role_main; }
    ~MainThread() { role_ = saved; }
};

void note_device_call(const char* name) {
    if (role_ != role_main) {
        return;
    }
    if (!first_main_thread_call_) {
        first_main_thread_call_ = name;
    }
    ++main_thread_device_calls_;
    std::printf("FAIL   : %s reached the device from the main thread\n", name);
    std::fflush(stdout);
}

struct LuaFake {
    bool active = false;

    // What luaL_checkudata hands back, and what luaL_check*/opt* answer with.
    void* userdata = nullptr;
    const char* string_argument = "";
    lua_Number number_argument = 0.0;

    // What the call raised, if it raised.
    bool raised = false;
    char error[512] {};

    // What the call pushed.
    int pushed = 0;
    int boolean = -1;
    bool nil = false;
    bool has_string = false;
    char string[2048] {};
    bool has_number = false;
    lua_Number number = 0.0;

    // lua_newuserdata comes out of here, so a MeshRef or a HandleRef the
    // entry point made outlives the call and can be handed back to the next
    // one. Bump-allocated and never reused: a test that made a hundred of
    // them would still be nowhere near the end.
    unsigned char arena[8192] {};
    std::size_t used = 0;
    void* last_userdata = nullptr;

    // What luaopen_worlddraw registered, one entry per metatable it made. A
    // metatable's __gc is the difference between a userdata that is reclaimed
    // and one that is not, and it is set in one line whose deletion would be
    // silent everywhere else.
    struct Registered {
        const char* name;
        lua_CFunction gc;
        const luaL_Reg* methods;
    };
    Registered registered[4] {};
    int registered_count = 0;
    int registering = -1;
    lua_CFunction pending_function = nullptr;
};

LuaFake lua_ {};
jmp_buf lua_jump_;

}  // namespace harness

extern "C" {
void* luaL_checkudata(lua_State*, int, const char*) {
    if (!harness::lua_.active) {
        stub_called("luaL_checkudata");
    }
    return harness::lua_.userdata;
}

int luaL_error(lua_State*, const char* format, ...) {
    if (!harness::lua_.active) {
        stub_called("luaL_error");
    }
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(harness::lua_.error, sizeof(harness::lua_.error), format, arguments);
    va_end(arguments);
    harness::lua_.raised = true;
    std::longjmp(harness::lua_jump_, 1);
    return 0;
}

lua_Number luaL_checknumber(lua_State*, int) {
    if (!harness::lua_.active) {
        stub_called("luaL_checknumber");
    }
    return harness::lua_.number_argument;
}

lua_Integer luaL_checkinteger(lua_State*, int) {
    if (!harness::lua_.active) {
        stub_called("luaL_checkinteger");
    }
    return static_cast<lua_Integer>(harness::lua_.number_argument);
}

const char* luaL_checklstring(lua_State*, int, size_t* length) {
    if (!harness::lua_.active) {
        stub_called("luaL_checklstring");
    }
    if (length) {
        *length = std::strlen(harness::lua_.string_argument);
    }
    return harness::lua_.string_argument;
}

lua_Number luaL_optnumber(lua_State*, int, lua_Number fallback) {
    if (!harness::lua_.active) {
        stub_called("luaL_optnumber");
    }
    return fallback;
}

int lua_toboolean(lua_State*, int) { stub_called("lua_toboolean"); return 0; }

int lua_type(lua_State*, int) {
    if (!harness::lua_.active) {
        stub_called("lua_type");
    }
    return LUA_TNIL;
}

void* lua_newuserdata(lua_State*, size_t size) {
    if (!harness::lua_.active) {
        stub_called("lua_newuserdata");
    }
    if (harness::lua_.used + size > sizeof(harness::lua_.arena)) {
        std::printf("FAIL   : the harness Lua arena ran out\n");
        std::fflush(stdout);
        std::abort();
    }
    void* const block = harness::lua_.arena + harness::lua_.used;
    harness::lua_.used += size;
    harness::lua_.last_userdata = block;
    ++harness::lua_.pushed;
    return block;
}

int lua_setmetatable(lua_State*, int) {
    if (!harness::lua_.active) {
        stub_called("lua_setmetatable");
    }
    return 1;
}

void lua_pushinteger(lua_State*, lua_Integer value) {
    if (!harness::lua_.active) {
        stub_called("lua_pushinteger");
    }
    ++harness::lua_.pushed;
    harness::lua_.has_number = true;
    harness::lua_.number = static_cast<lua_Number>(value);
}

void lua_pushnumber(lua_State*, lua_Number value) {
    if (!harness::lua_.active) {
        stub_called("lua_pushnumber");
    }
    ++harness::lua_.pushed;
    harness::lua_.has_number = true;
    harness::lua_.number = value;
}

void lua_pushboolean(lua_State*, int value) {
    if (!harness::lua_.active) {
        stub_called("lua_pushboolean");
    }
    ++harness::lua_.pushed;
    harness::lua_.boolean = value;
}

void lua_pushnil(lua_State*) {
    if (!harness::lua_.active) {
        stub_called("lua_pushnil");
    }
    ++harness::lua_.pushed;
    harness::lua_.nil = true;
}

void lua_pushstring(lua_State*, const char* text) {
    if (!harness::lua_.active) {
        stub_called("lua_pushstring");
    }
    ++harness::lua_.pushed;
    harness::lua_.has_string = true;
    std::snprintf(harness::lua_.string, sizeof(harness::lua_.string), "%s",
        text ? text : "");
}

void lua_pushvalue(lua_State*, int) {
    if (!harness::lua_.active) {
        stub_called("lua_pushvalue");
    }
}

void lua_pushcclosure(lua_State*, lua_CFunction function, int) {
    // lua_pushcfunction is this with no upvalues, and register_type reaches it
    // for exactly one thing: the __gc it is about to set.
    if (!harness::lua_.active) {
        stub_called("lua_pushcclosure");
    }
    harness::lua_.pending_function = function;
}

void lua_setfield(lua_State*, int, const char* key) {
    if (!harness::lua_.active) {
        stub_called("lua_setfield");
    }
    if (key && std::strcmp(key, "__gc") == 0 && harness::lua_.registering >= 0) {
        harness::lua_.registered[harness::lua_.registering].gc =
            harness::lua_.pending_function;
    }
}

void lua_getfield(lua_State*, int, const char*) {
    // luaL_getmetatable is this, and l_new and l_mesh both reach it.
    if (!harness::lua_.active) {
        stub_called("lua_getfield");
    }
}

void lua_settop(lua_State*, int) {
    if (!harness::lua_.active) {
        stub_called("lua_settop");
    }
}

void lua_createtable(lua_State*, int, int) {
    if (!harness::lua_.active) {
        stub_called("lua_createtable");
    }
    // lua_newtable, which luaopen_worlddraw ends with: the module table is no
    // longer a metatable, so nothing further belongs to one.
    harness::lua_.registering = -1;
    ++harness::lua_.pushed;
}

int luaL_newmetatable(lua_State*, const char* name) {
    if (!harness::lua_.active) {
        stub_called("luaL_newmetatable");
    }
    if (harness::lua_.registered_count
            < static_cast<int>(sizeof(harness::lua_.registered)
                / sizeof(harness::lua_.registered[0]))) {
        harness::lua_.registering = harness::lua_.registered_count++;
        harness::lua_.registered[harness::lua_.registering].name = name;
        harness::lua_.registered[harness::lua_.registering].gc = nullptr;
        harness::lua_.registered[harness::lua_.registering].methods = nullptr;
    }
    harness::lua_.pending_function = nullptr;
    return 1;
}

void luaL_register(lua_State*, const char*, const luaL_Reg* methods) {
    if (!harness::lua_.active) {
        stub_called("luaL_register");
    }
    if (harness::lua_.registering >= 0) {
        harness::lua_.registered[harness::lua_.registering].methods = methods;
    }
}
}

// ---- module seams ---------------------------------------------------------
// The engine loads a daemon from beside its own image, takes one reference on
// that image and drops it again. None of that can happen in a
// test exe, so the header's three module calls are redirected here and
// counted: whether the self-reference balances is the point of the check, and
// it cannot be seen from outside the process any other way.
//
// Declared before the header is included, because a member function body can
// only see names that were visible when its class was defined.
namespace harness {
HMODULE load_library(const WCHAR* path);
BOOL free_library(HMODULE module);
FARPROC get_proc_address(HMODULE module, const char* name);
int gdiplus_startup(void* token, const void* input);
void gdiplus_shutdown(ULONG_PTR token);
}  // namespace harness

#define FFXI_WORLD_DRAW_LOAD_LIBRARY_W(path) harness::load_library(path)
#define FFXI_WORLD_DRAW_FREE_LIBRARY(module) harness::free_library(module)
#define FFXI_WORLD_DRAW_GET_PROC_ADDRESS(module, name) harness::get_proc_address(module, name)

// The GDI+ pair, counted. Both call the real thing -- what is measured is
// whether the library starts it once and gives it back when the last handle
// closes, not whether GDI+ works.
#define FFXI_WORLD_DRAW_GDIPLUS_STARTUP(token, input) harness::gdiplus_startup(token, input)
#define FFXI_WORLD_DRAW_GDIPLUS_SHUTDOWN(token) harness::gdiplus_shutdown(token)

#include "../../lua/worlddraw.cpp"

namespace {

int checks_run = 0;
int checks_failed = 0;

void report(bool ok, const char* what) {
    ++checks_run;
    if (!ok) {
        ++checks_failed;
        std::printf("FAIL   : %s\n", what);
    }
}

void check(bool ok, const char* what) { report(ok, what); }

void check_exact(float value, float expected, const char* what) {
    const bool ok = value == expected;
    if (!ok) {
        std::printf("         %s: got %.9g want %.9g\n", what, value, expected);
    }
    report(ok, what);
}

void check_near(float value, float expected, float tolerance, const char* what) {
    const bool ok = std::fabs(value - expected) <= tolerance;
    if (!ok) {
        std::printf("         %s: got %.9g want %.9g\n", what, value, expected);
    }
    report(ok, what);
}

void check_size(std::size_t value, std::size_t expected, const char* what) {
    const bool ok = value == expected;
    if (!ok) {
        std::printf("         %s: got %u want %u\n", what,
            static_cast<unsigned>(value), static_cast<unsigned>(expected));
    }
    report(ok, what);
}

// The anchor of a vertex, exactly.
void check_anchor(const ffxi::GpuVertex& v, float x, float y, float z, const char* what) {
    check_exact(v.x, x, what);
    check_exact(v.y, y, what);
    check_exact(v.z, z, what);
}

void check_offsets(const ffxi::GpuVertex& v, float ox, float oy, float px, float py,
    const char* what) {
    check_exact(v.ox, ox, what);
    check_exact(v.oy, oy, what);
    check_exact(v.px, px, what);
    check_exact(v.py, py, what);
}

Command make(Command::Kind kind, const float* values, int count, DWORD color = 0xFF102030,
    ffxi::TextureId texture = 0, int segments = 128) {
    Command command;
    command.kind = kind;
    for (int i = 0; i < count; ++i) {
        command.v[i] = values[i];
    }
    command.color = color;
    command.texture = texture;
    command.segments = segments;
    return command;
}

// ---- 1. vertex counts -----------------------------------------------------
void test_counts() {
    const float pillar[5] = {1.0f, 2.0f, 3.0f, 0.5f, 4.0f};
    const float ring[5] = {1.0f, 2.0f, 3.0f, 10.0f, 0.25f};
    const float line[7] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.1f};
    const float degenerate[7] = {5.0f, 6.0f, 7.0f, 5.0f, 6.0f, 7.0f, 0.1f};
    const float panel[6] = {1.0f, 2.0f, 3.0f, 2.0f, 2.0f, 0.0f};
    const float sprite[5] = {1.0f, 2.0f, 3.0f, 2.0f, 2.0f};
    const float triangle[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};

    check_size(command_vertices(make(Command::Pillar, pillar, 5)), 6, "pillar is 6 vertices");
    check_size(command_vertices(make(Command::Ring, ring, 5, 0xFF, 0, 16)), 96,
        "ring of 16 segments is 96 vertices");
    check_size(command_vertices(make(Command::Ring, ring, 5, 0xFF, 0, 2)), 12,
        "a two-segment ring is two segments: no chosen minimum raises it");
    check_size(command_vertices(make(Command::Ring, ring, 5, 0xFF, 0, 100000)), 600000,
        "a hundred thousand segments is a hundred thousand segments: no cap");
    check_size(command_vertices(make(Command::Ring, ring, 5, 0xFF, 0, 0)), 6,
        "zero segments is raised to one, because a count divides the circle");
    check_size(command_vertices(make(Command::Ring, ring, 5, 0xFF, 0, -12)), 6,
        "a negative segment count is raised to one as well");
    // The only ceiling left is the one that keeps the vertex count
    // expressible, which is where UINT arithmetic over 40-byte vertices stops.
    check_size(command_vertices(make(Command::Ring, ring, 5, 0xFF, 0,
            std::numeric_limits<int>::max())),
        static_cast<std::size_t>(max_vertices_ / 6) * 6,
        "the segment ceiling is arithmetic: as many as a vertex count can name");
    check_size(command_vertices(make(Command::Line, line, 7)), 6, "line is 6 vertices");
    check_size(command_vertices(make(Command::Line, degenerate, 7)), 0,
        "a zero-length line is skipped");
    check_size(command_vertices(make(Command::Panel, panel, 6)), 6, "panel is 6 vertices");
    check_size(command_vertices(make(Command::Sprite, sprite, 5)), 6, "sprite is 6 vertices");
    check_size(command_vertices(make(Command::Triangle, triangle, 9)), 3,
        "triangle is 3 vertices");

    check(command_shader(make(Command::Line, line, 7)) == ffxi::GpuShaderLine,
        "a line takes the line shader");
    check(command_shader(make(Command::Pillar, pillar, 5)) == ffxi::GpuShaderBillboard,
        "a pillar takes the billboard shader");
    check(command_texture(make(Command::Panel, panel, 6, 0xFF, 7)) == 7,
        "a panel carries its texture");
    check(command_texture(make(Command::Pillar, pillar, 5, 0xFF, 7)) == 0,
        "a pillar is untextured whatever the command holds");
}

// ---- 2. the axis swap and the billboard offsets ---------------------------
void test_pillar() {
    // Windower (10, 20, -5): east/west 10, north/south 20, five yalms up.
    const float values[5] = {10.0f, 20.0f, -5.0f, 2.0f, 3.0f};
    ffxi::GpuVertex out[6];
    tessellate(make(Command::Pillar, values, 5, 0xFF445566), out);

    // Direct3D (10, -5, 20), and the top three yalms further up: -8.
    check_anchor(out[0], 10.0f, -5.0f, 20.0f, "pillar bottom anchor is (x, z, y)");
    check_anchor(out[2], 10.0f, -8.0f, 20.0f, "pillar top anchor is z - height");
    check_offsets(out[0], -1.0f, 0.0f, 0.0f, 0.0f, "pillar left offset is -width/2");
    check_offsets(out[1], 1.0f, 0.0f, 0.0f, 0.0f, "pillar right offset is +width/2");

    // The header's Quad: (a, b, c) then (c, b, d).
    check(out[3].x == out[2].x && out[3].ox == out[2].ox, "quad vertex 3 repeats c");
    check(out[4].ox == out[1].ox && out[4].y == out[1].y, "quad vertex 4 repeats b");
    check_exact(out[5].ox, 1.0f, "quad vertex 5 is d");
    check_exact(out[5].y, -8.0f, "quad vertex 5 is on the top anchor");
    check(out[0].color == 0xFF445566 && out[5].color == 0xFF445566, "pillar carries its colour");
    check(out[0].u == 0.0f && out[0].v == 0.0f, "pillar is untextured, uv 0");
}

// ---- 3. sprite: ground right across, world up along ------------------------
void test_sprite() {
    const float values[5] = {1.0f, 2.0f, -3.0f, 4.0f, 6.0f};
    ffxi::GpuVertex out[6];
    tessellate(make(Command::Sprite, values, 5, 0xFFFFFFFF, 9), out);

    for (int i = 0; i < 6; ++i) {
        check_anchor(out[i], 1.0f, -3.0f, 2.0f, "every sprite vertex shares one anchor");
    }

    // WorldDraw::Sprite is centred, bottom at z + height/2 (raw height grows
    // downward), so the bottom pair is oy = -half and carries v = 1.
    check_offsets(out[0], -2.0f, -3.0f, 0.0f, 0.0f, "sprite bottom-left offsets");
    check(out[0].u == 0.0f && out[0].v == 1.0f, "sprite bottom-left uv is (0, 1)");
    check_offsets(out[1], 2.0f, -3.0f, 0.0f, 0.0f, "sprite bottom-right offsets");
    check(out[1].u == 1.0f && out[1].v == 1.0f, "sprite bottom-right uv is (1, 1)");
    check_offsets(out[2], -2.0f, 3.0f, 0.0f, 0.0f, "sprite top-left offsets");
    check(out[2].u == 0.0f && out[2].v == 0.0f, "sprite top-left uv is (0, 0)");
    check_offsets(out[5], 2.0f, 3.0f, 0.0f, 0.0f, "sprite top-right offsets");
    check(out[5].u == 1.0f && out[5].v == 0.0f, "sprite top-right uv is (1, 0)");
}

// ---- 4. panel: fixed facing, no offsets ------------------------------------
void test_panel() {
    // facing 0 looks east, so the width runs north/south: across = (0, 1).
    const float values[6] = {10.0f, 20.0f, -5.0f, 4.0f, 2.0f, 0.0f};
    ffxi::GpuVertex out[6];
    tessellate(make(Command::Panel, values, 6, 0xFFFFFFFF, 3), out);

    check_anchor(out[0], 10.0f, -4.0f, 18.0f, "panel bottom-left corner");
    check_anchor(out[1], 10.0f, -4.0f, 22.0f, "panel bottom-right corner");
    check_anchor(out[2], 10.0f, -6.0f, 18.0f, "panel top-left corner");
    check_anchor(out[5], 10.0f, -6.0f, 22.0f, "panel top-right corner");
    for (int i = 0; i < 6; ++i) {
        check_offsets(out[i], 0.0f, 0.0f, 0.0f, 0.0f, "a panel has no offsets");
    }
    check(out[0].u == 0.0f && out[0].v == 1.0f, "panel bottom-left uv is (0, 1)");
    check(out[5].u == 1.0f && out[5].v == 0.0f, "panel top-right uv is (1, 0)");

    // facing pi/2 looks north, so the width runs east/west: across = (-1, 0).
    const float turned[6] = {10.0f, 20.0f, -5.0f, 4.0f, 2.0f, 1.57079632679f};
    tessellate(make(Command::Panel, turned, 6, 0xFFFFFFFF, 3), out);
    check_near(out[0].x, 12.0f, 1.0e-5f, "turned panel bottom-left runs east/west");
    check_near(out[0].z, 20.0f, 1.0e-5f, "turned panel keeps its north/south");
    check_near(out[1].x, 8.0f, 1.0e-5f, "turned panel bottom-right runs east/west");
}

// ---- 5. line: endpoints and the direction the shader widens across --------
void test_line() {
    // Straight north, five yalms long, in Windower axes.
    const float values[7] = {1.0f, 2.0f, -3.0f, 1.0f, 7.0f, -3.0f, 0.4f};
    ffxi::GpuVertex out[6];
    tessellate(make(Command::Line, values, 7, 0xFF00FF00), out);

    check_anchor(out[0], 1.0f, -3.0f, 2.0f, "line start anchor");
    check_anchor(out[2], 1.0f, -3.0f, 7.0f, "line end anchor");
    check_exact(out[0].ox, -0.2f, "line start left is -width/2");
    check_exact(out[1].ox, 0.2f, "line start right is +width/2");
    check_exact(out[2].ox, -0.2f, "line end left is -width/2");
    check_exact(out[5].ox, 0.2f, "line end right is +width/2");

    // (oy, px, py) is the unit direction in Direct3D axes: due north is +z.
    for (int i = 0; i < 6; ++i) {
        check_exact(out[i].oy, 0.0f, "line direction x");
        check_exact(out[i].px, 0.0f, "line direction y");
        check_exact(out[i].py, 1.0f, "line direction z");
    }

    // A line straight up: Windower height grows downward, so up is -y.
    const float upward[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -2.0f, 0.1f};
    tessellate(make(Command::Line, upward, 7), out);
    check_exact(out[0].oy, 0.0f, "upward line direction x");
    check_exact(out[0].px, -1.0f, "upward line direction y is -1 (up)");
    check_exact(out[0].py, 0.0f, "upward line direction z");
}

// ---- 6. ring: an upright band of `segments` quads --------------------------
void test_ring() {
    const float values[5] = {10.0f, 20.0f, -5.0f, 2.0f, 0.5f};
    ffxi::GpuVertex out[8 * 6];
    tessellate(make(Command::Ring, values, 5, 0xFFAA0000, 0, 8), out);

    // The first quad runs from angle 0 to 2pi/8: (x + r, y) to the north-east.
    check_near(out[0].x, 12.0f, 1.0e-5f, "ring starts at angle 0, x + radius");
    check_near(out[0].z, 20.0f, 1.0e-5f, "ring starts at angle 0, y");
    check_exact(out[0].y, -5.0f, "ring band bottom is z");
    check_exact(out[2].y, -5.5f, "ring band top is z - thickness");
    check_near(out[1].x, 10.0f + 2.0f * std::cos(6.28318530718f / 8.0f), 1.0e-5f,
        "the second corner is one segment on");
    for (int i = 0; i < 8 * 6; ++i) {
        check_offsets(out[i], 0.0f, 0.0f, 0.0f, 0.0f, "a ring has no offsets");
    }

    // The last quad closes the circle back onto angle 0.
    check_near(out[8 * 6 - 1].x, 12.0f, 1.0e-4f, "the last segment closes the ring");
    check_near(out[8 * 6 - 1].z, 20.0f, 1.0e-4f, "the last segment closes the ring");
}

// ---- 7. triangle -----------------------------------------------------------
void test_triangle() {
    const float values[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    ffxi::GpuVertex out[3];
    tessellate(make(Command::Triangle, values, 9, 0xFF010203), out);

    check_anchor(out[0], 1.0f, 3.0f, 2.0f, "triangle vertex 0 swapped");
    check_anchor(out[1], 4.0f, 6.0f, 5.0f, "triangle vertex 1 swapped");
    check_anchor(out[2], 7.0f, 9.0f, 8.0f, "triangle vertex 2 swapped");
    for (int i = 0; i < 3; ++i) {
        check_offsets(out[i], 0.0f, 0.0f, 0.0f, 0.0f, "a triangle has no offsets");
        check(out[i].color == 0xFF010203, "triangle carries its colour");
    }
}

// ---- 8. range grouping: adjacent only, never re-sorted --------------------
void test_ranges() {
    const float pillar[5] = {0, 0, 0, 1, 1};
    const float panel[6] = {0, 0, 0, 1, 1, 0};
    const float line[7] = {0, 0, 0, 1, 0, 0, 0.1f};
    const float triangle[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};

    SlotState state;
    check(grow_commands(state.list), "the staging list takes its first buffer");

    add_command(state, make(Command::Pillar, pillar, 5));
    add_command(state, make(Command::Pillar, pillar, 5));
    add_command(state, make(Command::Panel, panel, 6, 0xFF, 1));
    add_command(state, make(Command::Panel, panel, 6, 0xFF, 1));
    add_command(state, make(Command::Panel, panel, 6, 0xFF, 2));
    add_command(state, make(Command::Line, line, 7));
    add_command(state, make(Command::Triangle, triangle, 9));
    add_command(state, make(Command::Pillar, pillar, 5));

    check(build_draw_data(state.list, state.scratch, state.scratch_ranges),
        "the commands tessellate");
    check_size(state.scratch.size(), 6 + 6 + 6 + 6 + 6 + 6 + 3 + 6, "total vertices");
    check_size(state.scratch_ranges.size(), 5, "five ranges, adjacency only");

    const std::vector<Range>& ranges = state.scratch_ranges;
    check(ranges[0].shader == ffxi::GpuShaderBillboard && ranges[0].texture == 0
        && ranges[0].start_vertex == 0 && ranges[0].primitive_count == 4,
        "range 0: the two untextured pillars merged");
    check(ranges[1].texture == 1 && ranges[1].start_vertex == 12
        && ranges[1].primitive_count == 4, "range 1: the two panels on texture 1 merged");
    check(ranges[2].texture == 2 && ranges[2].start_vertex == 24
        && ranges[2].primitive_count == 2, "range 2: a different texture is a new range");
    check(ranges[3].shader == ffxi::GpuShaderLine && ranges[3].texture == 0
        && ranges[3].start_vertex == 30 && ranges[3].primitive_count == 2,
        "range 3: the line takes the line shader");
    check(ranges[4].shader == ffxi::GpuShaderBillboard && ranges[4].texture == 0
        && ranges[4].start_vertex == 36 && ranges[4].primitive_count == 3,
        "range 4: triangle and pillar merged, and NOT merged back into range 0");

    // The very geometry each range points at is still the geometry its command
    // described, in the order it was described: range 3's first vertex is the
    // line's start anchor.
    check_exact(state.scratch[30].py, 0.0f, "range 3 starts on the line's own vertices");
    check_exact(state.scratch[30].oy, 1.0f, "the line direction is +x, east");

    // A degenerate line contributes no vertices and no range at all.
    state.list.count = 0;
    const float degenerate[7] = {1, 1, 1, 1, 1, 1, 0.1f};
    add_command(state, make(Command::Line, degenerate, 7));
    add_command(state, make(Command::Pillar, pillar, 5));
    check(build_draw_data(state.list, state.scratch, state.scratch_ranges),
        "a degenerate line still tessellates");
    check_size(state.scratch.size(), 6, "a degenerate line adds no vertices");
    check_size(state.scratch_ranges.size(), 1, "a degenerate line adds no range");
    check(state.scratch_ranges[0].shader == ffxi::GpuShaderBillboard,
        "the surviving range is the pillar's");

    free_command_storage(state.list);
}

// ---- 9. staging growth -----------------------------------------------------
void test_growth() {
    const float pillar[5] = {1, 2, 3, 0.5f, 1.0f};

    SlotState state;
    check(grow_commands(state.list), "first buffer");
    check(state.list.capacity == initial_commands_, "the first buffer is 256 commands");

    const int total = initial_commands_ * 4 + 7;
    for (int i = 0; i < total; ++i) {
        float values[5] = {pillar[0] + static_cast<float>(i), pillar[1], pillar[2],
            pillar[3], pillar[4]};
        if (!add_command(state, make(Command::Pillar, values, 5))) {
            check(false, "an add ran out of memory");
            break;
        }
    }

    check(state.list.count == total, "every command staged");
    check(state.list.capacity >= total, "the buffer grew to fit");
    check(state.list.capacity == initial_commands_ * 8, "growth doubles");

    // The commands that were copied across each growth are still themselves.
    check_exact(state.list.items[0].v[0], 1.0f, "the first command survived the growth");
    check_exact(state.list.items[total - 1].v[0], 1.0f + static_cast<float>(total - 1),
        "the last command survived the growth");

    check(build_draw_data(state.list, state.scratch, state.scratch_ranges),
        "a grown list tessellates");
    check_size(state.scratch.size(), static_cast<std::size_t>(total) * 6,
        "a grown list tessellates every command");
    check_size(state.scratch_ranges.size(), 1, "identical adjacent commands are one range");
    check(state.scratch_ranges[0].primitive_count == static_cast<UINT>(total) * 2,
        "one range covers every triangle");
    check_exact(state.scratch[(static_cast<std::size_t>(total) - 1) * 6].x,
        1.0f + static_cast<float>(total - 1), "the last command's geometry is last");

    free_command_storage(state.list);
}

// ---- 10. mesh staging: tri and mark in one mesh ----------------------------
void test_mesh_staging() {
    Mesh mesh;
    const float triangle[15] = {
        1, 2, 3, 0.0f, 0.0f,
        4, 5, 6, 1.0f, 0.0f,
        7, 8, 9, 0.5f, 1.0f};
    stage_mesh_triangle(mesh, triangle, 0xFF778899);

    check_size(mesh.staging.size(), 3, "a mesh triangle stages three vertices");
    check_anchor(mesh.staging[0], 1.0f, 3.0f, 2.0f, "mesh vertex 0 is swapped into D3D axes");
    check_anchor(mesh.staging[2], 7.0f, 9.0f, 8.0f, "mesh vertex 2 is swapped into D3D axes");
    check_exact(mesh.staging[1].u, 1.0f, "mesh vertex 1 keeps its u");
    check_exact(mesh.staging[2].v, 1.0f, "mesh vertex 2 keeps its v");
    for (int i = 0; i < 3; ++i) {
        check_offsets(mesh.staging[i], 0.0f, 0.0f, 0.0f, 0.0f, "a mesh triangle has no offsets");
        check(mesh.staging[i].color == 0xFF778899, "a mesh triangle carries its colour");
    }

    stage_mesh_mark(mesh, 10.0f, 20.0f, -5.0f, 2.0f, 3.0f, 0xFF010101);
    check_size(mesh.staging.size(), 9, "a mark stages six more vertices");

    // The same shape a pillar has, in model space.
    ffxi::GpuVertex pillar[6];
    tessellate_pillar_shape(pillar, 10.0f, 20.0f, -5.0f, 2.0f, 3.0f, 0xFF010101);
    bool same = true;
    for (int i = 0; i < 6; ++i) {
        const ffxi::GpuVertex& a = mesh.staging[3 + i];
        same = same && a.x == pillar[i].x && a.y == pillar[i].y && a.z == pillar[i].z
            && a.ox == pillar[i].ox && a.oy == pillar[i].oy && a.color == pillar[i].color;
    }
    check(same, "a mark is pillar-shaped, vertex for vertex");
    check_anchor(mesh.staging[3], 10.0f, -5.0f, 20.0f, "mark bottom anchor");
    check_anchor(mesh.staging[5], 10.0f, -8.0f, 20.0f, "mark top anchor");
    check_exact(mesh.staging[3].ox, -1.0f, "mark half width");

    // tri and mark mix freely: the triangle staged first is still where it was.
    check_anchor(mesh.staging[0], 1.0f, 3.0f, 2.0f, "the triangle before the mark is untouched");
}

// ---- 11. mesh placement: the matrix is the old per-vertex maths ------------

// What the CPU path computed for every vertex of every mesh, every frame,
// before the retained pipeline: model space to world in Windower axes.
void legacy_place(const Mesh& mesh, float vx, float vy, float vz,
    float& wx, float& wy, float& wz) {
    const float cosine = std::cos(mesh.facing);
    const float sine = std::sin(mesh.facing);
    const float model_x = vx * mesh.scale;
    const float model_y = vy * mesh.scale;
    wx = mesh.x + model_x * cosine - model_y * sine;
    wy = mesh.y + model_x * sine + model_y * cosine;
    wz = mesh.z + vz * mesh.scale;
}

// A full four-component row vector through a matrix, w and all.
void transform4(const D3DMATRIX& m, const float* in, float* out) {
    for (int column = 0; column < 4; ++column) {
        out[column] = in[0] * m.m[0][column] + in[1] * m.m[1][column]
            + in[2] * m.m[2][column] + in[3] * m.m[3][column];
    }
}

void transform_point(const D3DMATRIX& m, float x, float y, float z, float* out) {
    out[0] = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
    out[1] = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
    out[2] = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
}

void test_world_matrix() {
    Mesh mesh;
    mesh.x = 10.0f;
    mesh.y = 20.0f;
    mesh.z = -5.0f;
    mesh.facing = 1.57079632679f;  // a quarter turn: 0 faces east
    mesh.scale = 2.0f;

    D3DMATRIX world {};
    mesh_world_matrix(mesh, world);

    // Hand-computed: cos = 0, sin = 1, scale 2.
    check_near(world.m[0][0], 0.0f, 1.0e-6f, "world row 0 x");
    check_near(world.m[0][2], 2.0f, 1.0e-6f, "world row 0 z is +scale*sin");
    check_exact(world.m[1][1], 2.0f, "world row 1 is the scale on height");
    check_near(world.m[2][0], -2.0f, 1.0e-6f, "world row 2 x is -scale*sin");
    check_exact(world.m[3][0], 10.0f, "world translation x is the Windower x");
    check_exact(world.m[3][1], -5.0f, "world translation y is the Windower z");
    check_exact(world.m[3][2], 20.0f, "world translation z is the Windower y");
    check_exact(world.m[3][3], 1.0f, "world is affine");

    // A model vertex one yalm east of the model origin lands two yalms north.
    float placed[3];
    transform_point(world, 1.0f, 0.0f, 0.0f, placed);
    check_near(placed[0], 10.0f, 1.0e-5f, "hand case: x");
    check_near(placed[1], -5.0f, 1.0e-5f, "hand case: height");
    check_near(placed[2], 22.0f, 1.0e-5f, "hand case: z");

    // And in general it agrees with the maths it replaced, vertex for vertex.
    const float samples[5][3] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}, {-3.5f, 2.25f, 7.125f}};
    const float facings[4] = {0.0f, 0.7f, -2.2f, 3.14159f};
    const float scales[3] = {1.0f, 0.25f, 3.0f};

    bool agrees = true;
    for (int f = 0; f < 4; ++f) {
        for (int s = 0; s < 3; ++s) {
            mesh.facing = facings[f];
            mesh.scale = scales[s];
            mesh_world_matrix(mesh, world);
            for (int i = 0; i < 5; ++i) {
                float wx = 0.0f, wy = 0.0f, wz = 0.0f;
                legacy_place(mesh, samples[i][0], samples[i][1], samples[i][2], wx, wy, wz);
                const Point model = to_d3d(samples[i][0], samples[i][1], samples[i][2]);
                transform_point(world, model.x, model.y, model.z, placed);
                agrees = agrees
                    && std::fabs(placed[0] - wx) < 1.0e-4f
                    && std::fabs(placed[1] - wz) < 1.0e-4f
                    && std::fabs(placed[2] - wy) < 1.0e-4f;
            }
        }
    }
    check(agrees, "the world matrix places every sample where the CPU path did");
}

// ---- 12. the billboard basis turned into model space -----------------------
void test_model_space_basis() {
    Mesh mesh;
    mesh.facing = 1.57079632679f;
    mesh.scale = 2.0f;

    // Hand case: camera right due east, a mesh turned a quarter and scaled by
    // two. The offset the shader adds before the world matrix must be
    // (0, 0, -0.5) so that it comes out (1, 0, 0) after it.
    const float right3[3] = {1.0f, 0.0f, 0.0f};
    float model_right[3];
    to_model_space(mesh, right3, model_right);
    check_near(model_right[0], 0.0f, 1.0e-6f, "hand case: model right x");
    check_near(model_right[1], 0.0f, 1.0e-6f, "hand case: model right y");
    check_near(model_right[2], -0.5f, 1.0e-6f, "hand case: model right z");

    // Up is invariant under a turn in the ground plane, and only scaled.
    const float up3[3] = {0.0f, -1.0f, 0.0f};
    float model_up[3];
    to_model_space(mesh, up3, model_up);
    check_near(model_up[0], 0.0f, 1.0e-6f, "model up x");
    check_near(model_up[1], -0.5f, 1.0e-6f, "model up y is up over the scale");
    check_near(model_up[2], 0.0f, 1.0e-6f, "model up z");

    // In general: pushing the model-space basis through the mesh's rotation
    // and scale -- the world matrix without its translation, which is what the
    // shader does to an offset -- gives the world basis back.
    const float facings[5] = {0.0f, 0.4f, 1.9f, -2.6f, 5.9f};
    const float scales[4] = {1.0f, 0.5f, 4.0f, 0.125f};
    const float directions[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
        {0.6f, 0.0f, 0.8f}};

    bool recovers = true;
    for (int f = 0; f < 5; ++f) {
        for (int s = 0; s < 4; ++s) {
            mesh.facing = facings[f];
            mesh.scale = scales[s];
            D3DMATRIX world {};
            mesh_world_matrix(mesh, world);
            for (int d = 0; d < 3; ++d) {
                float model[3];
                to_model_space(mesh, directions[d], model);
                // A direction, so the translation row is left out.
                const float x = model[0] * world.m[0][0] + model[1] * world.m[1][0]
                    + model[2] * world.m[2][0];
                const float y = model[0] * world.m[0][1] + model[1] * world.m[1][1]
                    + model[2] * world.m[2][1];
                const float z = model[0] * world.m[0][2] + model[1] * world.m[1][2]
                    + model[2] * world.m[2][2];
                recovers = recovers
                    && std::fabs(x - directions[d][0]) < 1.0e-4f
                    && std::fabs(y - directions[d][1]) < 1.0e-4f
                    && std::fabs(z - directions[d][2]) < 1.0e-4f;
            }
        }
    }
    check(recovers, "the inverse rotation gives the world basis back through any mesh");

    // A zero scale has no inverse; the offsets flatten rather than blow up.
    mesh.facing = 0.3f;
    mesh.scale = 0.0f;
    to_model_space(mesh, right3, model_right);
    check_exact(model_right[0], 0.0f, "a zero-scale mesh flattens its offsets");
    check_exact(model_right[1], 0.0f, "a zero-scale mesh flattens its offsets");
    check_exact(model_right[2], 0.0f, "a zero-scale mesh flattens its offsets");
}

// ---- 13. matrix multiply ---------------------------------------------------
void test_multiply() {
    D3DMATRIX a {};
    D3DMATRIX b {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a.m[r][c] = static_cast<float>(r * 4 + c + 1);
            b.m[r][c] = static_cast<float>((3 - r) * 4 + c + 1);
        }
    }

    D3DMATRIX product {};
    multiply_matrix(a, b, product);
    // Row 0 column 0: 1*13 + 2*9 + 3*5 + 4*1 = 13 + 18 + 15 + 4 = 50.
    check_exact(product.m[0][0], 50.0f, "row-vector multiply, element (0,0)");
    // Row 3 column 2: 13*15 + 14*11 + 15*7 + 16*3 = 195 + 154 + 105 + 48 = 502.
    check_exact(product.m[3][2], 502.0f, "row-vector multiply, element (3,2)");

    // A point through (a then b) is the same as a point through their product.
    // All four components: the composite the module builds is world matrix
    // times view-projection and the projection's last column is not
    // (0, 0, 0, 1), which is precisely why the shader finishes the job with
    // four dp4s rather than three.
    const float point[4] = {1.5f, -2.0f, 0.25f, 1.0f};
    float first[4];
    float second[4];
    float direct[4];
    transform4(a, point, first);
    transform4(b, first, second);
    transform4(product, point, direct);
    for (int i = 0; i < 4; ++i) {
        check_near(direct[i], second[i], 1.0e-2f, "composing matches applying in turn");
    }

    // Aliasing: the destination may be one of the sources.
    D3DMATRIX aliased = a;
    multiply_matrix(aliased, b, aliased);
    bool same = true;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            same = same && aliased.m[r][c] == product.m[r][c];
        }
    }
    check(same, "multiply_matrix survives writing into one of its sources");
}

// ---- 14. the vertex the GPU is handed --------------------------------------
void test_vertex_layout() {
    check_size(sizeof(ffxi::GpuVertex), 40, "GpuVertex is 40 bytes");
    check_size(offsetof(ffxi::GpuVertex, ox), 24, "the offsets sit where both declarations read");

    // The line declaration reads ox as one float and then oy, px, py as a
    // float3, so those four must be contiguous in that order.
    check_size(offsetof(ffxi::GpuVertex, oy), 28, "oy follows ox");
    check_size(offsetof(ffxi::GpuVertex, px), 32, "px follows oy");
    check_size(offsetof(ffxi::GpuVertex, py), 36, "py follows px");
}


// ---- 15. the minimum-width floor -------------------------------------------
//
// The screen-space path never lets a projected bar fall below
// min_projected_width_ = 1.5 pixels: expand_pair pushes the two ends apart to
// exactly that when they land closer. The markers this library exists for are
// 0.02 yalms wide and go under a pixel at ordinary distances, so the billboard
// shader has to floor the same way. These checks compare the shader's own
// arithmetic, replicated instruction for instruction below, against that
// legacy behaviour.

// A viewport, and the clip-to-pixel maths live_world_to_screen performs.
struct Screen {
    float width;
    float height;
};

void clip_to_pixels(const float* clip, const Screen& screen, float& x, float& y) {
    const float ndc_x = clip[0] / clip[3];
    const float ndc_y = clip[1] / clip[3];
    x = (ndc_x + 1.0f) * screen.width * 0.5f;
    y = (1.0f - ndc_y) * screen.height * 0.5f;
}

// Transcribed from WorldDrawPlugin::expand_pair, which is private -- this is
// the oracle the floor is asserted against, not a second implementation of it.
// min_projected_width_ is 1.5f there.
const float legacy_floor_ = 1.5f;

void legacy_expand_pair(float& minus_x, float& minus_y, float& plus_x, float& plus_y,
    float normal_x, float normal_y) {
    const float delta_x = plus_x - minus_x;
    const float delta_y = plus_y - minus_y;
    const float distance = std::sqrt(delta_x * delta_x + delta_y * delta_y);
    if (distance >= legacy_floor_) {
        return;
    }

    const float center_x = (minus_x + plus_x) * 0.5f;
    const float center_y = (minus_y + plus_y) * 0.5f;
    const float half_target = legacy_floor_ * 0.5f;

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

// The billboard shader, on the CPU, instruction for instruction against the
// listing in ffxi_world_draw.h. The constants come from the real
// gpu_width_floor_constants, so only the shader itself is transcribed.
void billboard_shader(const D3DMATRIX& m, const float* right3, const float* up3,
    float pixel_scale_x, float pixel_scale_y, const ffxi::GpuVertex& vertex,
    float* out_clip) {
    float c9[4] {};
    float c10[2] {};
    float c11[2] {};
    ffxi::gpu_width_floor_constants(m, right3, pixel_scale_x, pixel_scale_y,
        legacy_floor_ * 0.5f, c9, c10, c11);

    // mad r0.xyz, c5.xyz, v3.y, v0.xyz   /   mov r0.w, c7.x
    const float r0[4] = {
        up3[0] * vertex.oy + vertex.x,
        up3[1] * vertex.oy + vertex.y,
        up3[2] * vertex.oy + vertex.z,
        1.0f};

    // dp4 r1.x..w, r0, c0..c3   (c0-c3 are the columns of m)
    float r1[4];
    for (int column = 0; column < 4; ++column) {
        r1[column] = r0[0] * m.m[0][column] + r0[1] * m.m[1][column]
            + r0[2] * m.m[2][column] + r0[3] * m.m[3][column];
    }

    // mul r2, c9, v3.x
    float r2[4];
    for (int i = 0; i < 4; ++i) {
        r2[i] = c9[i] * vertex.ox;
    }

    // max r3.x, v3.x, -v3.x  /  mul r3.x, r3.x, c11.x  /  mul r3.y, c11.y, r1.w
    const float absolute_ox = vertex.ox > -vertex.ox ? vertex.ox : -vertex.ox;
    const float projected = absolute_ox * c11[0];
    const float floor_width = c11[1] * r1[3];

    // sge r3.z, r3.x, r3.y
    const float exact = projected >= floor_width ? 1.0f : 0.0f;

    // sge r4.x, v3.x, c7.y  /  sge r4.y, -v3.x, c7.y  /  add r3.w, r4.x, -r4.y
    const float side = (vertex.ox >= 0.0f ? 1.0f : 0.0f) - (-vertex.ox >= 0.0f ? 1.0f : 0.0f);

    // mul r4.xy, c10.xy, r1.w  /  mul r4.xy, r4.xy, r3.w
    const float b[2] = {c10[0] * r1[3] * side, c10[1] * r1[3] * side};

    // add r5.xy, r2.xy, -r4.xy  /  mad r2.xy, r5.xy, r3.z, r4.xy
    r2[0] = (r2[0] - b[0]) * exact + b[0];
    r2[1] = (r2[1] - b[1]) * exact + b[1];

    // mul r2.w, r2.w, r3.z: a floored quad keeps the anchor's w, so its two
    // vertices divide by the same one and land exactly the floor apart.
    r2[3] *= exact;

    // add r1, r1, r2
    for (int i = 0; i < 4; ++i) {
        r1[i] += r2[i];
    }

    // mul r5.xy, v3.zw, c6.xy  /  mad r1.xy, r5.xy, r1.w, r1.xy
    r1[0] += vertex.px * pixel_scale_x * r1[3];
    r1[1] += vertex.py * pixel_scale_y * r1[3];

    for (int i = 0; i < 4; ++i) {
        out_clip[i] = r1[i];
    }
}

// A left-handed perspective projection, row-vector, the shape D3DXMatrixPerspectiveFovLH makes.
void perspective(float fov, float aspect, float near_plane, float far_plane, D3DMATRIX& out) {
    const float h = 1.0f / std::tan(fov * 0.5f);
    const float w = h / aspect;
    std::memset(&out, 0, sizeof(out));
    out.m[0][0] = w;
    out.m[1][1] = h;
    out.m[2][2] = far_plane / (far_plane - near_plane);
    out.m[2][3] = 1.0f;
    out.m[3][2] = -near_plane * far_plane / (far_plane - near_plane);
}

// A yaw about the Direct3D y axis, so the camera right is not axis-aligned.
void yaw(float angle, D3DMATRIX& out) {
    std::memset(&out, 0, sizeof(out));
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    out.m[0][0] = c;   out.m[0][2] = -s;
    out.m[1][1] = 1.0f;
    out.m[2][0] = s;   out.m[2][2] = c;
    out.m[3][3] = 1.0f;
}

// One case, end to end: the two vertices of a width pair through the shader,
// and the same two world points through the legacy project-then-expand path.
// Returns whether the floor engaged, per the legacy oracle.
bool floor_case(const D3DMATRIX& m, const float* right3, const Screen& screen,
    const float* anchor, float half_width, const char* what) {
    const float up3[3] = {0.0f, -1.0f, 0.0f};
    const float pixel_scale_x = 2.0f / screen.width;
    const float pixel_scale_y = -2.0f / screen.height;

    // --- the legacy path: project both ends exactly, then expand_pair ---
    float legacy_minus[4];
    float legacy_plus[4];
    for (int column = 0; column < 4; ++column) {
        const float minus_world[3] = {
            anchor[0] - right3[0] * half_width,
            anchor[1] - right3[1] * half_width,
            anchor[2] - right3[2] * half_width};
        const float plus_world[3] = {
            anchor[0] + right3[0] * half_width,
            anchor[1] + right3[1] * half_width,
            anchor[2] + right3[2] * half_width};
        legacy_minus[column] = minus_world[0] * m.m[0][column] + minus_world[1] * m.m[1][column]
            + minus_world[2] * m.m[2][column] + m.m[3][column];
        legacy_plus[column] = plus_world[0] * m.m[0][column] + plus_world[1] * m.m[1][column]
            + plus_world[2] * m.m[2][column] + m.m[3][column];
    }

    float minus_x = 0.0f, minus_y = 0.0f, plus_x = 0.0f, plus_y = 0.0f;
    clip_to_pixels(legacy_minus, screen, minus_x, minus_y);
    clip_to_pixels(legacy_plus, screen, plus_x, plus_y);
    const float exact_width = std::sqrt((plus_x - minus_x) * (plus_x - minus_x)
        + (plus_y - minus_y) * (plus_y - minus_y));
    const bool engaged = exact_width < legacy_floor_;
    legacy_expand_pair(minus_x, minus_y, plus_x, plus_y, 1.0f, 0.0f);

    // --- the shader path: one anchor, two signed offsets ---
    ffxi::GpuVertex vertex {};
    vertex.x = anchor[0];
    vertex.y = anchor[1];
    vertex.z = anchor[2];
    vertex.color = 0xFFFFFFFF;
    vertex.u = 0.0f;
    vertex.v = 0.0f;
    vertex.oy = 0.0f;
    vertex.px = 0.0f;
    vertex.py = 0.0f;

    float shader_minus[4];
    float shader_plus[4];
    vertex.ox = -half_width;
    billboard_shader(m, right3, up3, pixel_scale_x, pixel_scale_y, vertex, shader_minus);
    vertex.ox = half_width;
    billboard_shader(m, right3, up3, pixel_scale_x, pixel_scale_y, vertex, shader_plus);

    float shader_minus_x = 0.0f, shader_minus_y = 0.0f;
    float shader_plus_x = 0.0f, shader_plus_y = 0.0f;
    clip_to_pixels(shader_minus, screen, shader_minus_x, shader_minus_y);
    clip_to_pixels(shader_plus, screen, shader_plus_x, shader_plus_y);
    const float shader_width = std::sqrt(
        (shader_plus_x - shader_minus_x) * (shader_plus_x - shader_minus_x)
        + (shader_plus_y - shader_minus_y) * (shader_plus_y - shader_minus_y));

    if (engaged) {
        // The floor engaged: the bar is exactly the floor wide, and so is the
        // legacy one it is being held to.
        check_near(shader_width, legacy_floor_, 2.0e-3f, what);
        check_near(exact_width < legacy_floor_ ? legacy_floor_ : exact_width,
            std::sqrt((plus_x - minus_x) * (plus_x - minus_x)
                + (plus_y - minus_y) * (plus_y - minus_y)),
            2.0e-3f, "the oracle floors to 1.5 px too");
    } else {
        // Above the floor nothing is touched: the shader's two vertices are
        // the two world points the legacy path projected, to the pixel.
        check_near(shader_minus_x, minus_x, 2.0e-3f, what);
        check_near(shader_minus_y, minus_y, 2.0e-3f, what);
        check_near(shader_plus_x, plus_x, 2.0e-3f, what);
        check_near(shader_plus_y, plus_y, 2.0e-3f, what);
    }
    return engaged;
}

void test_width_floor() {
    const Screen screen = {800.0f, 600.0f};
    const float pixel_scale_x = 2.0f / screen.width;
    const float pixel_scale_y = -2.0f / screen.height;

    D3DMATRIX projection {};
    perspective(1.04719755f, screen.width / screen.height, 0.1f, 1000.0f, projection);

    // --- the constants themselves ---
    const float right3[3] = {1.0f, 0.0f, 0.0f};
    float c9[4] {};
    float c10[2] {};
    float c11[2] {};
    ffxi::gpu_width_floor_constants(projection, right3, pixel_scale_x, pixel_scale_y,
        0.75f, c9, c10, c11);

    // c9 is (right, 0) * m, so with this projection it is (w, 0, 0, 0).
    check_near(c9[0], projection.m[0][0], 1.0e-6f, "c9 is the right vector through the matrix");
    check_exact(c9[1], 0.0f, "c9 y");
    check_exact(c9[3], 0.0f, "c9 w: the camera right does not change w here");
    // c11.x is that in pixels at w = 1: clip * (width/2).
    check_near(c11[0], projection.m[0][0] * screen.width * 0.5f, 1.0e-4f,
        "c11.x is the pixel length of c9 at w = 1");
    check_exact(c11[1], 0.75f, "c11.y is the half floor");
    // c10 lands exactly half the floor away, in pixels, after the divide by w.
    check_near(c10[0] / pixel_scale_x, 0.75f, 1.0e-5f, "c10 is 0.75 px in clip units");
    check_exact(c10[1], 0.0f, "c10 y, for a purely horizontal right vector");

    // Degenerate: a right vector the camera looks straight along has no
    // on-screen direction to stretch along, and the floor stands down.
    const float parallel[3] = {0.0f, 0.0f, 1.0f};
    ffxi::gpu_width_floor_constants(projection, parallel, pixel_scale_x, pixel_scale_y,
        0.75f, c9, c10, c11);
    check_exact(c11[1], 0.0f, "a degenerate width direction turns the floor off");
    check_exact(c10[0], 0.0f, "a degenerate width direction loads no floor offset");
    check_exact(c11[0], 0.0f, "a degenerate width direction has no pixel length");

    // --- the exact path is the same vertex the old shader produced ---
    // (anchor + right*ox + up*oy) * m, which is what the pre-transform mad
    // computed, against the clip-space form the floor needed.
    const float up3[3] = {0.0f, -1.0f, 0.0f};
    ffxi::GpuVertex wide {};
    wide.x = 3.0f; wide.y = -1.5f; wide.z = 12.0f;
    wide.ox = 2.0f; wide.oy = 0.75f;
    float shader_clip[4];
    billboard_shader(projection, right3, up3, pixel_scale_x, pixel_scale_y, wide, shader_clip);
    float pre[4];
    const float world[3] = {
        wide.x + right3[0] * wide.ox + up3[0] * wide.oy,
        wide.y + right3[1] * wide.ox + up3[1] * wide.oy,
        wide.z + right3[2] * wide.ox + up3[2] * wide.oy};
    for (int column = 0; column < 4; ++column) {
        pre[column] = world[0] * projection.m[0][column] + world[1] * projection.m[1][column]
            + world[2] * projection.m[2][column] + projection.m[3][column];
    }
    for (int i = 0; i < 4; ++i) {
        check_near(shader_clip[i], pre[i], 1.0e-5f,
            "a wide quad is exactly the vertex the pre-transform offset made");
    }

    // --- a vertex with no width is never moved ---
    ffxi::GpuVertex flat {};
    flat.x = -4.0f; flat.y = 2.0f; flat.z = 30.0f;
    flat.ox = 0.0f; flat.oy = 0.0f;
    billboard_shader(projection, right3, up3, pixel_scale_x, pixel_scale_y, flat, shader_clip);
    for (int column = 0; column < 4; ++column) {
        pre[column] = flat.x * projection.m[0][column] + flat.y * projection.m[1][column]
            + flat.z * projection.m[2][column] + projection.m[3][column];
    }
    for (int i = 0; i < 4; ++i) {
        check_exact(shader_clip[i], pre[i],
            "a triangle, panel, ring or mesh face is left exactly where it was");
    }

    // --- the floor against the legacy oracle, over a distance sweep ---
    // A 0.02-yalm marker: the width the library was built for.
    int engaged_count = 0;
    int exact_count = 0;
    for (int i = 0; i < 24; ++i) {
        const float distance = 1.0f + static_cast<float>(i) * 4.0f;
        const float anchor[3] = {0.0f, 0.0f, distance};
        if (floor_case(projection, right3, screen, anchor, 0.01f,
                "a 0.02-yalm marker matches the legacy path")) {
            ++engaged_count;
        } else {
            ++exact_count;
        }
    }
    check(engaged_count > 0 && exact_count > 0,
        "the sweep crosses the floor: some floored, some exact");

    // The crossing itself: the floor engages exactly when the projected width
    // is under 1.5 px and not one step sooner. At this projection a half width
    // of h yalms at distance d is h * m00 * width/2 / d pixels.
    const float pixels_per_yalm_at_1 = projection.m[0][0] * screen.width * 0.5f;
    for (int i = 0; i < 40; ++i) {
        const float distance = 5.0f + static_cast<float>(i) * 2.5f;
        // The half width whose projected half width is exactly the 0.75 floor.
        const float boundary = 0.75f * distance / pixels_per_yalm_at_1;
        const float anchor[3] = {0.0f, 0.0f, distance};

        const bool below = floor_case(projection, right3, screen, anchor, boundary * 0.9f,
            "just under the floor");
        const bool above = floor_case(projection, right3, screen, anchor, boundary * 1.1f,
            "just over the floor");
        check(below, "a quad under 1.5 px is floored");
        check(!above, "a quad over 1.5 px is left exact");
    }

    // --- the same, through a turned camera and an off-axis right vector ---
    D3DMATRIX turn {};
    yaw(0.6f, turn);
    D3DMATRIX view_projection {};
    multiply_matrix(turn, projection, view_projection);
    const float turned_right[3] = {std::cos(0.35f), 0.0f, std::sin(0.35f)};

    int turned_engaged = 0;
    for (int i = 0; i < 16; ++i) {
        const float distance = 2.0f + static_cast<float>(i) * 3.0f;
        const float anchor[3] = {0.4f, -1.0f, distance};
        if (floor_case(view_projection, turned_right, screen, anchor, 0.01f,
                "a marker under a turned camera matches the legacy path")) {
            ++turned_engaged;
        }
        // Wide bars stay exact whatever the camera is doing.
        floor_case(view_projection, turned_right, screen, anchor, 1.5f,
            "a wide bar under a turned camera is untouched");
    }
    check(turned_engaged > 0, "the turned camera sweep reaches the floor too");

    // --- and through a mesh transform: world * view-projection, with the
    // right vector already turned into model space, which is how a mark inside
    // a rotated mesh reaches the shader ---
    Mesh mesh;
    mesh.x = 12.0f;
    mesh.y = 40.0f;
    mesh.z = -2.0f;
    mesh.facing = 0.9f;
    mesh.scale = 0.5f;
    D3DMATRIX mesh_world {};
    mesh_world_matrix(mesh, mesh_world);
    D3DMATRIX mesh_transform {};
    multiply_matrix(mesh_world, projection, mesh_transform);

    float model_right[3];
    to_model_space(mesh, right3, model_right);

    int mesh_engaged = 0;
    for (int i = 0; i < 12; ++i) {
        const float anchor[3] = {static_cast<float>(i) * 0.5f, 0.0f, 0.0f};
        if (floor_case(mesh_transform, model_right, screen, anchor, 0.02f,
                "a mark inside a rotated, scaled mesh matches the legacy path")) {
            ++mesh_engaged;
        }
    }
    check(mesh_engaged > 0, "the mesh sweep reaches the floor as well");
}

// ---- 16. growth: every table, past the cap it used to have -----------------
//
// The handle table held 8, a handle's meshes 64, a handle's textures 64 and a
// mesh 65535 vertices. None of those numbers exists any more, and what
// replaces them is doubling -- so the checks are that the table gets past the
// old number, that an index handed out before a growth still names the same
// thing after it, and that a refusal, when there is one, is clean.

// The doubling itself, on its own: what it carries across and when it refuses.
void test_doubled_table() {
    Command source[4];
    for (int i = 0; i < 4; ++i) {
        source[i].v[0] = static_cast<float>(i + 1);
    }

    int capacity = -1;
    Command* grown = doubled_table(source, 4, 4, initial_commands_, capacity);
    check(grown != nullptr, "a table doubles");
    check(capacity == 8, "doubling doubles");
    if (grown) {
        bool carried = true;
        for (int i = 0; i < 4; ++i) {
            carried = carried && grown[i].v[0] == source[i].v[0];
        }
        check(carried, "the entries already in the table are carried across in order");
        check(grown[4].v[0] == 0.0f, "and the new entries come up default-initialised");
        delete[] grown;
    }

    // A first table takes the initial size rather than doubling nothing.
    capacity = -1;
    grown = doubled_table<Command>(nullptr, 0, 0, initial_commands_, capacity);
    check(grown != nullptr, "an empty table takes its first buffer");
    check(capacity == initial_commands_, "the first buffer is the initial size");
    delete[] grown;

    // The one refusal there is: a doubling whose size in bytes could not be
    // expressed. Nothing is allocated and nothing is touched, and every caller
    // turns this into a clean Lua error.
    capacity = -1;
    const int past_half = static_cast<int>(table_ceiling_<Command> / 2) + 1;
    grown = doubled_table<Command>(nullptr, past_half, 0, initial_commands_, capacity);
    check(grown == nullptr, "a doubling past what a size_t can express refuses");
    check(capacity == -1, "and reports no capacity, so the caller keeps what it had");
}

// Twenty handles, where the old table refused the ninth, and every one of them
// still resolving through the index it was given.
void test_handle_growth() {
    constexpr int wanted = 20;
    int indices[wanted] {};
    std::uint32_t generations[wanted] {};
    SlotState* states[wanted] {};

    const int capacity_before = g_slot_capacity;
    const Slot* const table_before = g_slots;

    for (int i = 0; i < wanted; ++i) {
        states[i] = new (std::nothrow) SlotState();
        char name[64] {};
        std::snprintf(name, sizeof(name), "addon number %d", i);
        indices[i] = states[i] ? claim_slot(states[i], copy_name(name)) : -1;
        if (indices[i] >= 0) {
            generations[i] = g_slots[indices[i]].generation;
        }
    }

    bool claimed = true;
    for (int i = 0; i < wanted; ++i) {
        claimed = claimed && indices[i] >= 0;
    }
    check(claimed, "twenty handles are claimed, where the table used to hold eight");
    check(g_slot_capacity >= wanted, "the handle table grew to fit them");
    check(g_slot_capacity > capacity_before, "which is more than it started with");
    check(g_slots != table_before, "and the storage moved, so the indices are what carried");

    // Index stability: what a HandleRef holds is the index, and the slot it
    // names is still the same state with the same generation.
    bool stable = true;
    for (int i = 0; i < wanted; ++i) {
        if (indices[i] < 0) {
            stable = false;
            continue;
        }
        stable = stable && g_slots[indices[i]].state == states[i]
            && g_slots[indices[i]].generation == generations[i]
            && find_slot(indices[i], generations[i]) == &g_slots[indices[i]];
    }
    check(stable, "every index handed out before a growth still names its own slot after it");

    // Distinct indices: no two handles were given the same slot.
    bool distinct = true;
    for (int i = 0; i < wanted; ++i) {
        for (int j = i + 1; j < wanted; ++j) {
            distinct = distinct && indices[i] != indices[j];
        }
    }
    check(distinct, "no two handles were given the same slot");

    // The name is the caller's own, in full: the field that used to hold
    // thirty-two bytes cut everything past the thirty-first.
    check(indices[7] >= 0 && g_slots[indices[7]].name != nullptr
            && std::strcmp(g_slots[indices[7]].name, "addon number 7") == 0,
        "a handle carries the name it was given");

    char long_name[300] {};
    for (int i = 0; i + 1 < static_cast<int>(sizeof(long_name)); ++i) {
        long_name[i] = static_cast<char>('a' + (i % 26));
    }
    SlotState* long_state = new (std::nothrow) SlotState();
    const int long_index = long_state ? claim_slot(long_state, copy_name(long_name)) : -1;
    check(long_index >= 0 && g_slots[long_index].name != nullptr
            && std::strcmp(g_slots[long_index].name, long_name) == 0,
        "a 299-character name is stored whole, not cut to thirty-one");

    // Closing gives the slots back, bumps their generations and frees the
    // names; the table itself never shrinks.
    for (int i = 0; i < wanted; ++i) {
        if (indices[i] >= 0) {
            close_slot(g_slots[indices[i]]);
        }
    }
    if (long_index >= 0) {
        close_slot(g_slots[long_index]);
    }

    bool closed = true;
    for (int i = 0; i < wanted; ++i) {
        closed = closed && indices[i] >= 0 && !g_slots[indices[i]].active
            && g_slots[indices[i]].name == nullptr
            && find_slot(indices[i], generations[i]) == nullptr;
    }
    check(closed, "closing a handle releases its slot and its name, and its index goes stale");
    check(g_slot_capacity >= wanted, "the table keeps the size it grew to");

    // And the slots come back: the next claim reuses index 0 rather than
    // growing again.
    SlotState* again = new (std::nothrow) SlotState();
    const int reused = again ? claim_slot(again, copy_name("reuse")) : -1;
    check(reused == 0, "a freed slot is claimed again before the table grows");
    check(again != nullptr && g_slots[0].generation != generations[0],
        "and its generation moved on, so the old handle stays closed");
    if (reused >= 0) {
        close_slot(g_slots[reused]);
    }
}

// Two hundred meshes on one handle, where the old array held sixty-four.
void test_mesh_growth() {
    constexpr int wanted = 200;
    SlotState state;

    int indices[wanted] {};
    for (int i = 0; i < wanted; ++i) {
        indices[i] = claim_mesh(state);
    }

    bool ordered = true;
    for (int i = 0; i < wanted; ++i) {
        ordered = ordered && indices[i] == i;
    }
    check(ordered, "two hundred meshes are claimed in order, where sixty-four used to be all");
    check(state.mesh_count == wanted, "the handle holds every one of them");
    check(state.mesh_capacity >= wanted, "the table of pointers grew to fit");

    // Index stability, and more: the meshes themselves never move, so a Mesh*
    // taken before a growth is still that mesh afterwards.
    Mesh* const first = state.meshes[0];
    const float triangle[15] = {
        1, 2, 3, 0.0f, 0.0f,
        4, 5, 6, 1.0f, 0.0f,
        7, 8, 9, 0.5f, 1.0f};
    stage_mesh_triangle(*first, triangle, 0xFF223344);

    // Freeing a run of them and claiming them again reuses rather than grows.
    for (int i = 3; i < 100; ++i) {
        state.meshes[i]->used = false;
    }
    bool reused_in_order = true;
    for (int i = 3; i < 100; ++i) {
        reused_in_order = reused_in_order && claim_mesh(state) == i;
    }
    check(reused_in_order, "freed meshes are claimed again, lowest first");
    check(state.mesh_count == wanted, "and the table did not grow for any of them");
    check(state.meshes[0] == first,
        "the mesh handed out first is the same object after every growth");

    // Its staged geometry is untouched by any of it.
    check_size(first->staging.size(), 3, "and still holds what was staged into it");
    check_anchor(first->staging[0], 1.0f, 3.0f, 2.0f, "with the vertices it was given");

    free_slot_storage(state);
    check(state.mesh_count == 0 && state.meshes == nullptr,
        "closing frees every mesh and the table that named them");
}

// Three hundred textures owned by one handle, where the old list quietly
// stopped recording after sixty-four -- and quietly leaked the rest.
void test_texture_list_growth() {
    constexpr int wanted = 300;
    SlotState state;

    bool recorded = true;
    for (int i = 0; i < wanted; ++i) {
        recorded = recorded && record_texture(state, i + 1);
    }
    check(recorded, "three hundred textures are recorded, where sixty-four used to be all");
    check(state.texture_count == wanted, "every one of them is on the handle's list");
    check(state.texture_capacity >= wanted, "the list grew to fit");

    bool in_order = true;
    for (int i = 0; i < wanted; ++i) {
        in_order = in_order && state.textures[i] == i + 1;
    }
    check(in_order, "and every id recorded before a growth is still where it was after it");

    free_slot_storage(state);
    check(state.texture_count == 0 && state.textures == nullptr,
        "closing releases them and frees the list");
}

// A mesh past the 65535 vertices it used to refuse at.
void test_mesh_past_old_cap() {
    Mesh mesh;
    constexpr int triangles = 30000;  // 90000 vertices

    bool staged = true;
    for (int i = 0; i < triangles; ++i) {
        const float base = static_cast<float>(i);
        const float triangle[15] = {
            base, 0, 0, 0.0f, 0.0f,
            base, 1, 0, 1.0f, 0.0f,
            base, 0, 1, 0.5f, 1.0f};
        if (!reserve_staging(mesh, 3)) {
            staged = false;
            break;
        }
        stage_mesh_triangle(mesh, triangle, 0xFF556677);
    }

    check(staged, "ninety thousand vertices stage, where 65535 used to be the wall");
    check_size(mesh.staging.size(), static_cast<std::size_t>(triangles) * 3,
        "every one of them is there");
    check_exact(mesh.staging[0].x, 0.0f, "the first vertex survived the growth");
    check_exact(mesh.staging[(triangles - 1) * 3].x, static_cast<float>(triangles - 1),
        "and so did the last");

    // The one thing still bounded is what a vertex buffer can be addressed
    // with, which is arithmetic rather than a chosen count.
    check(static_cast<std::size_t>(triangles) * 3 < max_vertices_,
        "and this is still nowhere near where the vertex count stops being expressible");
}

// ---- 17. the Lua entry points, with no graphics device --------------------
//
// The one thing that cannot be measured from underneath the Lua layer:
// whether an entry point raises. Raising is a longjmp out of luaL_error, so
// these go in through the shipping entry points with the fake state above
// behind them.
//
// Why it matters. README and worlddraw.lua both promise that an addon whose
// library cannot draw still runs: the handle comes back, the calls succeed,
// nothing is drawn, and the reason is reported. Two calls used to break that
// promise by raising -- m:build(), which an addon reaches at load, before the
// game has a device, which is the case PostRender exists to recover from; and
// d:commit(), which an addon reaches from a prerender handler, where a raise
// is an uncaught error thirty times a second and buries the one line that
// says why nothing is drawn.
//
// The line these checks pin down: a state-of-the-world failure reports and
// returns false; a programmer error still raises.

// One entry point, called the way LuaCore calls it. The number of values it
// pushed, or -1 when it raised.
int call_entry(lua_CFunction entry) {
    harness::lua_.raised = false;
    harness::lua_.error[0] = '\0';
    harness::lua_.pushed = 0;
    harness::lua_.boolean = -1;
    harness::lua_.nil = false;
    harness::lua_.has_string = false;
    harness::lua_.string[0] = '\0';
    harness::lua_.has_number = false;
    harness::lua_.number = 0.0;

    volatile int results = -1;
    harness::lua_.active = true;
    {
        // The gate. For the length of this call the harness is Windower's Lua
        // thread, and any device method any of the fakes is asked for names
        // itself and fails the run. It wraps the entry point rather than one
        // function inside it, so a device call that appears anywhere the entry
        // point can reach is caught wherever it hides.
        harness::MainThread main_thread;
        if (setjmp(harness::lua_jump_) == 0) {
            results = entry(nullptr);
        }
    }
    harness::lua_.active = false;
    return harness::lua_.raised ? -1 : results;
}


// A handle through the real l_new, and the slot behind it.
HandleRef* open_handle(const char* name) {
    harness::lua_.string_argument = name;
    harness::lua_.userdata = nullptr;
    const int results = call_entry(l_new);
    if (results != 1) {
        return nullptr;
    }
    HandleRef* const ref = static_cast<HandleRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = ref;
    return ref;
}

SlotState* state_of(const HandleRef* ref) {
    return ref && ref->slot >= 0 && ref->slot < g_slot_capacity ? g_slots[ref->slot].state
                                                                : nullptr;
}

void test_lua_entry_points() {
    // Nothing has been Loaded onto the module's engine yet and DeviceFromGame
    // resolves nothing in a test exe, so this is exactly the window an addon
    // loads in: a library that cannot draw.
    check(g_draw.Device() == nullptr, "the entry-point checks run with no graphics device");

    HandleRef* const handle = open_handle("entrypoints");
    check(handle != nullptr, "wd.new hands back a handle with no device, rather than failing");
    if (!handle) {
        return;
    }
    SlotState* const state = state_of(handle);
    check(state != nullptr, "and the handle names a live slot");
    if (!state) {
        return;
    }

    // Something to publish. An empty list commits successfully by design --
    // there is nothing to put on a device -- so the failure needs geometry.
    harness::lua_.number_argument = 1.0;
    check(call_entry(l_pillar) == 0, "a pillar is staged");
    check(state->list.count == 1, "and the list holds it");

    // ---- the blocker: commit with no device ------------------------------
    //
    // It no longer has a device half. A commit tessellates on this thread and
    // hands the array to the render thread, which is the only one that may
    // call the device at all, so "no graphics device" has stopped being a
    // condition a commit can fail on: it is a picture that arrives on the
    // first frame after there is one.
    const LONG before = state->log.engineering_serial;
    const int results = call_entry(l_commit);
    check(results == 1, "d:commit() with no device DOES NOT RAISE");
    check(harness::lua_.boolean == 1,
        "and SUCCEEDS: with the upload deferred, no device is not a failure any more");
    check(state->log.engineering_serial == before, "with nothing reported at all");
    check(state->log.player[0] == '\0',
        "and nothing player-facing invented for it: a player can do nothing with this");

    // Nothing is on a device, because there is none and because this thread
    // may not put anything on one. The vertices are staged, waiting.
    check(state->published.buffer == nullptr, "nothing was published to a device");
    check(state->published.ranges.empty(), "and no range was left describing nothing");
    check(state->pending.valid, "the commit is pending an upload");
    check_size(state->pending.vertices.size(), 6, "carrying the pillar it tessellated");
    check(state->pending.ranges.size() == 1, "and the range that draws it");
    check(state->list.count == 0, "with the described list cleared, as a commit always did");

    // ---- the same for m:build() ------------------------------------------
    check(call_entry(l_mesh) == 1, "d:mesh() hands back a mesh with no device");
    MeshRef* const mesh_ref = static_cast<MeshRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = mesh_ref;
    check(call_entry(l_mesh_tri) == 0, "a triangle is staged into it");

    Mesh* const mesh = state->meshes[mesh_ref->mesh];
    check(call_entry(l_mesh_build) == 1, "m:build() with no device DOES NOT RAISE");
    check(harness::lua_.boolean == 1,
        "and SUCCEEDS, for the same reason: building no longer needs a device");
    check(mesh->built, "the mesh is built");
    check(mesh->pending, "and pending an upload");
    check(mesh->buffer == nullptr, "with no buffer, so draw_meshes skips it");
    check(mesh->count == 3, "but the count the render thread will make it from");
    check_size(mesh->staging.size(), 3,
        "and its staged vertices KEPT, because they are what the upload reads");

    // Building twice is a programmer error whether or not the upload has
    // happened: the second call is a bug in the addon either way.
    check(call_entry(l_mesh_build) == -1, "building it again raises");
    check(std::strcmp(harness::lua_.error, "worlddraw: mesh is already built") == 0,
        "as the programmer error it always was");

    // ---- programmer errors still raise -----------------------------------
    harness::lua_.userdata = handle;
    check(call_entry(l_mesh) == 1, "a second mesh is claimed");
    MeshRef* const empty_ref = static_cast<MeshRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = empty_ref;
    check(call_entry(l_mesh_build) == -1, "building a mesh with nothing staged raises");
    check(std::strcmp(harness::lua_.error, "worlddraw: mesh has no vertices") == 0,
        "with the message that says which programmer error it was");

    check(call_entry(l_mesh_free) == 0, "the mesh is freed");
    check(call_entry(l_mesh_build) == -1, "and building a freed mesh raises");
    check(std::strcmp(harness::lua_.error, "worlddraw: mesh is freed") == 0,
        "as a freed mesh, which is a bug in the addon and not a state of the world");

    harness::lua_.userdata = handle;
    check(call_entry(l_close) == 0, "the handle closes");
    check(call_entry(l_commit) == -1, "committing on a closed handle raises");
    check(std::strcmp(harness::lua_.error, "worlddraw: handle is closed") == 0,
        "because a closed handle is a bug in the addon, whatever the world is doing");
}

// ---- 17b. described before there is any device, drawn once there is -------
//
// The shape the library documents -- an addon describes its meshes at load --
// runs before the game has made its device more often than not. It used to
// mean m:build() returned false and the author had to try again; now it means
// the vertices wait for the render thread, and the first composite past the
// device's arrival makes the buffer.
//
// The handle is claimed the way test_handle_growth claims one, rather than
// through wd.new: what has to survive into the device era is the slot, and
// wd.new's other half -- the open count that decides when the image
// unregisters -- belongs to the tests between here and there. Everything done
// to it goes through the shipping entry points.
HandleRef survivor_ref_ {};
SlotState* survivor_state_ = nullptr;
Mesh* survivor_mesh_ = nullptr;

void test_described_before_any_device() {
    check(g_draw.Device() == nullptr,
        "these are described with no graphics device in the client at all");

    survivor_state_ = new (std::nothrow) SlotState();
    check(survivor_state_ != nullptr, "a handle's state is allocated");
    if (!survivor_state_) {
        return;
    }
    check(grow_commands(survivor_state_->list), "and its command list");

    const int index = claim_slot(survivor_state_, copy_name("survivor"));
    check(index >= 0, "and it claims a slot");
    if (index < 0) {
        return;
    }
    survivor_ref_.slot = index;
    survivor_ref_.generation = g_slots[index].generation;

    harness::lua_.userdata = &survivor_ref_;
    harness::lua_.number_argument = 1.0;
    check(call_entry(l_pillar) == 0, "a pillar is described");
    check(call_entry(l_commit) == 1, "and committed with no device");
    check(harness::lua_.boolean == 1, "which succeeds");
    check(survivor_state_->pending.valid, "leaving an upload pending");
    check(survivor_state_->published.buffer == nullptr, "and nothing on any device");

    check(call_entry(l_mesh) == 1, "a mesh is claimed");
    MeshRef* const mesh_ref = static_cast<MeshRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = mesh_ref;
    check(call_entry(l_mesh_tri) == 0, "a triangle is staged into it");
    check(call_entry(l_mesh_build) == 1, "and it is built with no device");
    check(harness::lua_.boolean == 1, "which succeeds");

    survivor_mesh_ = survivor_state_->meshes[mesh_ref->mesh];
    check(survivor_mesh_->built, "the mesh says it is built, so m:at() will take it");
    check(survivor_mesh_->pending, "it is pending an upload");
    check(survivor_mesh_->buffer == nullptr,
        "it has no buffer, which is what stops draw_meshes drawing it");
    check_size(survivor_mesh_->staging.size(), 3, "and it kept the vertices to be made from");
}

// The metatables luaopen_worlddraw makes, and what it puts on them. A __gc is
// one line, and a userdata whose metatable has none is simply never reclaimed:
// the mesh type had none at all, so every d:mesh() nobody freed was a
// permanent entry on the handle and, past build(), a permanent
// IDirect3DVertexBuffer8.
const harness::LuaFake::Registered* registered_type(const char* name) {
    for (int i = 0; i < harness::lua_.registered_count; ++i) {
        if (harness::lua_.registered[i].name
            && std::strcmp(harness::lua_.registered[i].name, name) == 0) {
            return &harness::lua_.registered[i];
        }
    }
    return nullptr;
}

bool has_method(const luaL_Reg* methods, const char* name) {
    for (const luaL_Reg* entry = methods; entry && entry->name; ++entry) {
        if (std::strcmp(entry->name, name) == 0) {
            return true;
        }
    }
    return false;
}

void test_registration() {
    harness::lua_.registered_count = 0;
    harness::lua_.registering = -1;
    harness::lua_.active = true;
    const int results = luaopen_worlddraw(nullptr);
    harness::lua_.active = false;

    check(results == 1, "luaopen_worlddraw hands back one value, the module table");

    const harness::LuaFake::Registered* const handle = registered_type(handle_type_);
    const harness::LuaFake::Registered* const mesh = registered_type(mesh_type_);
    check(handle != nullptr, "it registers the handle type");
    check(mesh != nullptr, "and the mesh type");
    if (!handle || !mesh) {
        return;
    }

    check(handle->gc == &l_close, "the handle type is finalised by l_close, as it always was");
    check(mesh->gc == &l_mesh_gc,
        "and the MESH type by l_mesh_gc, without which a mesh nobody freed is never reclaimed");

    check(has_method(handle->methods, "commit"), "the handle carries commit");
    check(has_method(handle->methods, "last_error"), "and last_error");
    check(has_method(handle->methods, "player_error"),
        "and player_error, which is what the chat path asks for");
    check(has_method(handle->methods, "engineering_error"),
        "and engineering_error, so a player-facing message cannot hide a technical one");
    check(has_method(mesh->methods, "build"), "the mesh carries build");
    check(has_method(mesh->methods, "free"),
        "and free, which stays the documented way to say so early");
}

// ---- 18. the two kinds of message, one of them a handoff ------------------
//
// The bug: a failed load_texture parked "texture: ..." in the one buffer a
// handle had, worlddraw.lua's player-facing filter then suppressed it, and
// every player-facing message that handle would ever have had -- the daemon
// missing, an ABI refusal, a stomped hook -- was invisible behind it for the
// life of the handle. The only site that cleared that buffer was load_texture
// itself, so nothing else could ever unstick it.
//
// Two buffers closed that. What is checked here as well is the shape the
// player-facing one has now: a handoff. Reading it takes it, so it is answered
// exactly once and neither side of the boundary keeps a record of what has
// already been said. The engineering side is not a queue and must not become
// one -- an author inspects it as often as they like and gets the same answer.
void test_error_log() {
    HandleRef* const handle = open_handle("errorlog");
    check(handle != nullptr, "the error-log checks have a handle");
    if (!handle) {
        return;
    }
    SlotState* const state = state_of(handle);

    g_module_log = ErrorLog();
    state->log = ErrorLog();

    const char* const missing =
        "worlddraw can't draw: a file is missing.\ndetails: hook: nothing";
    const char* const stomped =
        "worlddraw stopped drawing: another program took over the graphics.";

    // A player-facing message, the way the engine writes them.
    g_draw.Report(missing);
    check(std::strcmp(g_module_log.player, missing) == 0,
        "a player-facing message goes in the image's log, where every handle sees it");
    check(g_module_log.engineering[0] == '\0', "and never in the engineering buffer");

    // Now the string that used to bury it, filed against this handle, and
    // recorded while a message for the player is still waiting to be shown.
    g_error_slot = handle->slot;
    g_draw.Report("texture: image could not be read");
    g_error_slot = -1;
    check(std::strcmp(state->log.engineering, "texture: image could not be read") == 0,
        "an engineering string is filed against the handle whose call produced it");
    check(state->log.player[0] == '\0', "and cannot reach a player buffer at all");

    // Inspecting the engineering side spends nothing, however often it is done:
    // last_error hands back the most recent of either kind, so the technical
    // detail an author wants is not hidden by the player-facing one either.
    check(call_entry(l_last_error) == 1, "d:last_error() answers with one value");
    check(std::strcmp(harness::lua_.string, "texture: image could not be read") == 0,
        "the most recent message of either kind");
    check(call_entry(l_engineering_error) == 1,
        "and d:engineering_error() with one value of its own");
    check(std::strcmp(harness::lua_.string, "texture: image could not be read") == 0,
        "an author asked for by name");
    check(call_entry(l_engineering_error) == 1
            && std::strcmp(harness::lua_.string, "texture: image could not be read") == 0,
        "and asked for again, because reading a message of that kind does not take it");
    check(std::strcmp(g_module_log.player, missing) == 0,
        "with the player's message untouched behind all of it, which is the whole bug");

    // And the handoff. One value and no serial: nothing on either side of the
    // boundary compares messages any more.
    check(call_entry(l_player_error) == 1, "d:player_error() answers with the message alone");
    check(std::strcmp(harness::lua_.string, missing) == 0, "which is the one nobody has shown");
    check(g_module_log.player_serial == 0,
        "and taking it leaves the buffer with nothing left to say");
    check(call_entry(l_player_error) == 1 && harness::lua_.nil,
        "so the read after it is nil: the message was handed over exactly once");

    // A second message, different from the first, is handed over in its turn.
    g_draw.Report(stomped);
    check(call_entry(l_player_error) == 1, "a new player-facing message answers");
    check(std::strcmp(harness::lua_.string, stomped) == 0, "with the new text");
    check(call_entry(l_player_error) == 1 && harness::lua_.nil, "and is gone once taken as well");

    // The same words again are a message in their own right, because nothing
    // compares the text: a condition that comes back is said again when it does.
    g_draw.Report(stomped);
    check(call_entry(l_player_error) == 1 && std::strcmp(harness::lua_.string, stomped) == 0,
        "the same words reported again are handed over again");

    // An engineering string landing on top of a player-facing one neither
    // takes it nor hides it: the player is still told on the next poll.
    g_draw.Report(missing);
    g_error_slot = handle->slot;
    g_draw.Report("texture: image lock failed");
    g_error_slot = -1;
    check(call_entry(l_player_error) == 1 && std::strcmp(harness::lua_.string, missing) == 0,
        "a player-facing message with a later engineering string on top of it still shows");
    check(call_entry(l_engineering_error) == 1
            && std::strcmp(harness::lua_.string, "texture: image lock failed") == 0,
        "and the engineering string is still there behind it, unspent");

    // And a closed handle answers nothing, as it always did -- and takes
    // nothing either: there is no handle left to hand the message to.
    g_draw.Report(missing);
    check(call_entry(l_close) == 0, "the handle closes");
    check(call_entry(l_last_error) == 1 && harness::lua_.nil,
        "a closed handle's last_error is nil");
    check(call_entry(l_player_error) == 1 && harness::lua_.nil, "and so is its player_error");
    check(std::strcmp(g_module_log.player, missing) == 0,
        "while the message it did not answer with is still there to be taken");
    g_module_log = ErrorLog();
}

// ---- 19. a mesh nobody freed ----------------------------------------------
//
// claim_mesh only ever reuses an entry whose `used` is false, and only
// m:free() set it. A mesh reached by `local m = d:mesh()` in a per-frame path
// therefore added a permanent entry -- and, past build(), a permanent
// IDirect3DVertexBuffer8 -- thirty times a second, every one of them walked by
// draw_meshes every frame, until the handle closed. The mesh userdata's
// metatable had no __gc at all.
void test_mesh_collection() {
    HandleRef* const handle = open_handle("collect");
    check(handle != nullptr, "the collection checks have a handle");
    if (!handle) {
        return;
    }
    SlotState* const state = state_of(handle);

    // Thirty frames' worth of the shape that leaked.
    bool reused = true;
    for (int i = 0; i < 30; ++i) {
        harness::lua_.userdata = handle;
        if (call_entry(l_mesh) != 1) {
            reused = false;
            break;
        }
        harness::lua_.userdata = harness::lua_.last_userdata;
        harness::lua_.number_argument = static_cast<lua_Number>(i + 1);
        if (call_entry(l_mesh_tri) != 0 || call_entry(l_mesh_gc) != 0) {
            reused = false;
            break;
        }
        reused = reused && state->mesh_count == 1;
    }
    check(reused, "a mesh nobody freed is collected, so thirty of them are one entry");
    check(state->mesh_count == 1, "and the handle still holds exactly one");
    check(!state->meshes[0]->used, "which is free for the next d:mesh()");
    check_size(state->meshes[0]->staging.size(), 0,
        "with its staged vertices given back rather than held for the life of the handle");

    // Collection after an explicit free is a no-op, not a double free: the
    // generation the reference carries moved on when free() bumped it.
    harness::lua_.userdata = handle;
    check(call_entry(l_mesh) == 1, "a mesh is claimed");
    MeshRef* const freed = static_cast<MeshRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = freed;
    check(call_entry(l_mesh_free) == 0, "and freed explicitly");
    const std::uint32_t generation = state->meshes[freed->mesh]->generation;
    check(call_entry(l_mesh_gc) == 0, "collecting it afterwards does not raise");
    check(state->meshes[freed->mesh]->generation == generation,
        "and frees nothing a second time");
    check(call_entry(l_mesh_free) == -1, "while a second explicit free still raises");
    check(std::strcmp(harness::lua_.error, "worlddraw: mesh is freed") == 0,
        "with the message it always had");

    // A collection that arrives after the handle closed frees nothing: closing
    // already freed every mesh the handle owned.
    harness::lua_.userdata = handle;
    check(call_entry(l_mesh) == 1, "a last mesh is claimed");
    MeshRef* const orphan = static_cast<MeshRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = handle;
    check(call_entry(l_close) == 0, "the handle closes with it still alive");
    harness::lua_.userdata = orphan;
    check(call_entry(l_mesh_gc) == 0, "collecting it afterwards does not raise");
}

}  // namespace

// ---- the daemon path ------------------------------------------------------
// How the engine hooks: loading the daemon, registering one handler set, and
// unregistering it again with the self-reference balanced. The daemon here is
// a stub that counts what it is asked for, so these are checks on the engine
// half of the contract; the daemon half has its own harness in
// tools/daemon_harness, against the real DLL.

// Windower's PluginManager is a set of declarations whose definitions live in
// the host, and a vtable for anything derived from it needs them to exist. The
// only one this answers is the device; the rest fail the run if they are ever
// called.
MMFSettingsHandler* __stdcall PluginManager::GetMMFSettingsHandler(MMFSettingsHandler*) {
    stub_called("GetMMFSettingsHandler");
    return nullptr;
}
void* __stdcall PluginManager::GetHWND() { stub_called("GetHWND"); return nullptr; }
void* __stdcall PluginManager::GetDirect3D8Device() { stub_called("GetDirect3D8Device"); return nullptr; }
Console* __stdcall PluginManager::GetConsole() { stub_called("GetConsole"); return nullptr; }
TextHandler* __stdcall PluginManager::GetTextHandler() { stub_called("GetTextHandler"); return nullptr; }
PrimitiveHandler* __stdcall PluginManager::GetPrimitiveHandler() { stub_called("GetPrimitiveHandler"); return nullptr; }
PacketStreamHandler* __stdcall PluginManager::GetPacketStreamHandler() { stub_called("GetPacketStreamHandler"); return nullptr; }
FFXI* __stdcall PluginManager::GetFFXI() { stub_called("GetFFXI"); return nullptr; }
PluginManager* __thiscall PluginManager::Dtor(std::uint8_t) { stub_called("Dtor"); return nullptr; }

namespace harness {

// Fake module handles. Nothing is ever loaded, so these only have to be
// distinguishable and non-null.
HMODULE const daemon_module_ = reinterpret_cast<HMODULE>(0x00D0000);
HMODULE const self_module_ = reinterpret_cast<HMODULE>(0x00E0000);

struct ModuleCalls {
    int daemon_loads;
    int self_loads;
    int daemon_frees;
    int self_frees;
    bool refuse_daemon_load;
};
ModuleCalls modules_ {};

struct DaemonCalls {
    int acquires;
    int ensure_hooks;
    int register_set;
    int unregister_set;
    const WdHandlerSet* last_set;
    const WdHandlerSet* last_unregistered;
    IDirect3DDevice8* last_device;
    std::uint32_t register_result;
    int check_slots;
    std::uint32_t check_slots_result;
    int releases_at_unregister;
    int shutdowns_at_unregister;
};
DaemonCalls daemon_ {};

std::uint32_t __stdcall fake_ensure_hooks(IDirect3DDevice8* device) {
    ++daemon_.ensure_hooks;
    daemon_.last_device = device;
    return 1;
}

std::uint32_t __stdcall fake_register_set(const WdHandlerSet* set) {
    ++daemon_.register_set;
    daemon_.last_set = set;
    return daemon_.register_result;
}

// Counted in the sections below, and read here: what the teardown order is
// cannot be seen from outside, so the unregister -- which is the drain --
// takes a snapshot of the two counters on its way past.
extern int texture_releases_;
extern int gdiplus_shutdowns_;

void __stdcall fake_unregister_set(const WdHandlerSet* set) {
    ++daemon_.unregister_set;
    daemon_.last_unregistered = set;
    daemon_.releases_at_unregister = texture_releases_;
    daemon_.shutdowns_at_unregister = gdiplus_shutdowns_;
}

std::uint32_t __stdcall fake_check_slots(void) {
    ++daemon_.check_slots;
    return daemon_.check_slots_result;
}
void __stdcall fake_stats(WdDaemonStats*) {}
const char* __stdcall fake_build_id(void) { return "harness"; }

WdDaemonApi api_ {};

const WdDaemonApi* __stdcall fake_acquire(std::uint32_t min_abi) {
    ++daemon_.acquires;
    if (min_abi > WD_DAEMON_ABI) {
        return nullptr;
    }

    api_.abi_version = WD_DAEMON_ABI;
    api_.size = static_cast<std::uint32_t>(sizeof(WdDaemonApi));
    api_.ensure_hooks = &fake_ensure_hooks;
    api_.register_set = &fake_register_set;
    api_.unregister_set = &fake_unregister_set;
    api_.check_slots = &fake_check_slots;
    api_.stats = &fake_stats;
    api_.build_id = &fake_build_id;
    return &api_;
}

bool ends_with(const WCHAR* path, const WCHAR* tail) {
    std::size_t path_length = 0;
    while (path[path_length] != L'\0') {
        ++path_length;
    }
    std::size_t tail_length = 0;
    while (tail[tail_length] != L'\0') {
        ++tail_length;
    }
    if (tail_length > path_length) {
        return false;
    }
    for (std::size_t i = 0; i < tail_length; ++i) {
        if (path[path_length - tail_length + i] != tail[i]) {
            return false;
        }
    }
    return true;
}

HMODULE load_library(const WCHAR* path) {
    if (ends_with(path, L"worlddraw_daemon.dll")) {
        ++modules_.daemon_loads;
        return modules_.refuse_daemon_load ? nullptr : daemon_module_;
    }

    // Anything else is the engine asking for a reference on its own image,
    // which in this build is the harness exe.
    ++modules_.self_loads;
    return self_module_;
}

BOOL free_library(HMODULE module) {
    if (module == daemon_module_) {
        ++modules_.daemon_frees;
    } else {
        ++modules_.self_frees;
    }
    return TRUE;
}

FARPROC get_proc_address(HMODULE module, const char* name) {
    if (module != daemon_module_ || std::strcmp(name, WD_DAEMON_ACQUIRE_NAME) != 0) {
        return nullptr;
    }
    return reinterpret_cast<FARPROC>(reinterpret_cast<void (*)()>(&fake_acquire));
}

// GDI+ started and shut down for real, and counted. The count is the point:
// the startup happens on the first image a handle loads, and until the Lua
// teardown path learned to release everything the image owns there was no
// shutdown at all on it -- one GDI+ instance leaked per addon reload.
int gdiplus_starts_ = 0;
int gdiplus_shutdowns_ = 0;

int gdiplus_startup(void* token, const void* input) {
    ++gdiplus_starts_;
    return static_cast<int>(Gdiplus::GdiplusStartup(static_cast<ULONG_PTR*>(token),
        static_cast<const Gdiplus::GdiplusStartupInput*>(input), nullptr));
}

void gdiplus_shutdown(ULONG_PTR token) {
    note_device_call("GdiplusShutdown");
    ++gdiplus_shutdowns_;
    Gdiplus::GdiplusShutdown(token);
}

// ---- a device-shaped object ----------------------------------------------
// The handlers call methods on the device the pass arrives on, so the order
// they run in cannot be exercised without something device-shaped: an object
// whose first field is a vtable, filled with a stub that fails the run, and
// the one method these paths reach pointed at an implementation here.
//
// The slot numbers are derived rather than typed. A pointer to a virtual
// member function carries the slot it occupies, and the six the ABI header
// generates out of the SDK's d3d8.h are checked against that before any of
// them is used -- so if the derivation ever stopped holding, these checks
// would fail loudly instead of quietly testing a vtable of the wrong shape.
template <typename Method>
int vtable_index(Method method) {
    union Bits {
        Method method;
        std::uintptr_t raw;
    };

    Bits bits;
    bits.raw = 0;
    bits.method = method;
    return static_cast<int>((bits.raw - 1) / sizeof(void*));
}

void* device_vtable_[WD_DEVICE_VTABLE_SLOTS] {};
struct FakeDevice {
    void** vtable;
};
FakeDevice device_ {};

FakeDevice other_device_ {};

int get_transform_calls_ = 0;
int create_shader_calls_ = 0;
int delete_shader_calls_ = 0;
DWORD next_shader_handle_ = 1;
D3DMATRIX fake_view_ {};
D3DMATRIX fake_projection_ {};

HRESULT __stdcall unexpected_device_call() {
    std::printf("FAIL   : the harness device was called through a slot it does not implement\n");
    std::fflush(stdout);
    std::abort();
}

HRESULT __stdcall fake_get_transform(void*, D3DTRANSFORMSTATETYPE state, D3DMATRIX* out) {
    note_device_call("IDirect3DDevice8::GetTransform");
    ++get_transform_calls_;
    if (!out) {
        return E_FAIL;
    }
    *out = state == D3DTS_VIEW ? fake_view_ : fake_projection_;
    return D3D_OK;
}

// Shaders are made and deleted through the device that owns them, so both
// halves are counted: the point of the check below is that a device which
// changed has its handles dropped rather than deleted, and a delete that
// happened would be a call into a device that may already have gone.
bool fake_shader_fails_ = false;

HRESULT __stdcall fake_create_vertex_shader(void*, const DWORD*, const DWORD*, DWORD* handle,
    DWORD) {
    note_device_call("IDirect3DDevice8::CreateVertexShader");
    ++create_shader_calls_;
    if (!handle || fake_shader_fails_) {
        return E_FAIL;
    }
    *handle = next_shader_handle_++;
    return D3D_OK;
}

HRESULT __stdcall fake_delete_vertex_shader(void*, DWORD) {
    note_device_call("IDirect3DDevice8::DeleteVertexShader");
    ++delete_shader_calls_;
    return D3D_OK;
}

// The one question the diagnostic asks the device. The flags the fake answers
// with are set by the test; whether the real client's device carries
// D3DCREATE_MULTITHREADED is what the report is deployed to find out.
int creation_parameter_calls_ = 0;
DWORD fake_behavior_flags_ = 0;
bool fake_creation_parameters_fail_ = false;

HRESULT __stdcall fake_get_creation_parameters(void*,
    D3DDEVICE_CREATION_PARAMETERS* parameters) {
    note_device_call("IDirect3DDevice8::GetCreationParameters");
    ++creation_parameter_calls_;
    if (!parameters || fake_creation_parameters_fail_) {
        return E_FAIL;
    }
    parameters->AdapterOrdinal = 0;
    parameters->DeviceType = D3DDEVTYPE_HAL;
    parameters->hFocusWindow = nullptr;
    parameters->BehaviorFlags = fake_behavior_flags_;
    return D3D_OK;
}

// ---- a texture-shaped object ----------------------------------------------
// The engine's own CreateTexture makes one through the device, locks it, fills
// it and releases it, so the pool cannot be exercised through the shipping
// code without something that answers those three calls.
void* texture_vtable_[32] {};
int texture_creates_ = 0;
int texture_releases_ = 0;
// A device that refuses to make a texture. The refusal now happens on the
// render thread, at the first draw that wants the id, so this is how that
// path is reached.
bool texture_create_fails_ = false;

struct FakeTexture {
    void** vtable;
    BYTE* bits;
    int pitch;
};

HRESULT __stdcall fake_texture_lock_rect(void* self, UINT, D3DLOCKED_RECT* locked,
    const RECT*, DWORD) {
    note_device_call("IDirect3DTexture8::LockRect");
    FakeTexture* const texture = static_cast<FakeTexture*>(self);
    if (!locked || !texture) {
        return E_FAIL;
    }
    locked->Pitch = texture->pitch;
    locked->pBits = texture->bits;
    return D3D_OK;
}

HRESULT __stdcall fake_texture_unlock_rect(void*, UINT) {
    note_device_call("IDirect3DTexture8::UnlockRect");
    return D3D_OK;
}

ULONG __stdcall fake_texture_release(void* self) {
    note_device_call("IDirect3DTexture8::Release");
    ++texture_releases_;
    FakeTexture* const texture = static_cast<FakeTexture*>(self);
    if (texture) {
        delete[] texture->bits;
        delete texture;
    }
    return 0;
}

HRESULT __stdcall fake_create_texture(void*, UINT width, UINT height, UINT, DWORD,
    D3DFORMAT, D3DPOOL, IDirect3DTexture8** out) {
    note_device_call("IDirect3DDevice8::CreateTexture");
    ++texture_creates_;
    if (!out || width == 0 || height == 0 || texture_create_fails_) {
        return E_FAIL;
    }

    FakeTexture* const texture = new FakeTexture();
    texture->vtable = texture_vtable_;
    texture->pitch = static_cast<int>(width) * 4;
    texture->bits = new BYTE[static_cast<std::size_t>(texture->pitch) * height];
    *out = reinterpret_cast<IDirect3DTexture8*>(texture);
    return D3D_OK;
}

// ---- a vertex-buffer-shaped object ----------------------------------------
// A commit and a mesh build both make one of these through the device, write
// it and, when a bigger one replaces it, release it. Failing to make one and
// failing to write one are two of the four state-of-the-world failures
// d:commit() has to report rather than raise, so both are switchable here.
void* vertex_buffer_vtable_[32] {};
int buffer_creates_ = 0;
int buffer_releases_ = 0;
int buffer_locks_ = 0;
bool buffer_create_fails_ = false;
bool buffer_lock_fails_ = false;

struct FakeVertexBuffer {
    void** vtable;
    BYTE* bytes;
    UINT size;
};

HRESULT __stdcall fake_buffer_lock(void* self, UINT offset, UINT size, BYTE** data, DWORD) {
    note_device_call("IDirect3DVertexBuffer8::Lock");
    ++buffer_locks_;
    FakeVertexBuffer* const buffer = static_cast<FakeVertexBuffer*>(self);
    if (!buffer || !data || buffer_lock_fails_ || offset + size > buffer->size) {
        return E_FAIL;
    }
    *data = buffer->bytes + offset;
    return D3D_OK;
}

HRESULT __stdcall fake_buffer_unlock(void*) {
    note_device_call("IDirect3DVertexBuffer8::Unlock");
    return D3D_OK;
}

ULONG __stdcall fake_buffer_release(void* self) {
    note_device_call("IDirect3DVertexBuffer8::Release");
    ++buffer_releases_;
    FakeVertexBuffer* const buffer = static_cast<FakeVertexBuffer*>(self);
    if (buffer) {
        delete[] buffer->bytes;
        delete buffer;
    }
    return 0;
}

HRESULT __stdcall fake_create_vertex_buffer(void*, UINT length, DWORD, DWORD, D3DPOOL,
    IDirect3DVertexBuffer8** out) {
    note_device_call("IDirect3DDevice8::CreateVertexBuffer");
    ++buffer_creates_;
    if (!out || length == 0 || buffer_create_fails_) {
        return E_FAIL;
    }

    FakeVertexBuffer* const buffer = new FakeVertexBuffer();
    buffer->vtable = vertex_buffer_vtable_;
    buffer->size = length;
    buffer->bytes = new BYTE[length];
    *out = reinterpret_cast<IDirect3DVertexBuffer8*>(buffer);
    return D3D_OK;
}

// ---- the two calls a bind makes -------------------------------------------
// BindGpuTexture is where a texture is made now, so the bind path has to be
// reachable, and it sets the texture and the stage states around it.
// `bound_texture_` is what the pass would have been left with -- null is
// untextured, which is what an id whose texture does not exist yet resolves
// to.
IDirect3DBaseTexture8* bound_texture_ = nullptr;
int set_texture_calls_ = 0;

HRESULT __stdcall fake_set_texture(void*, DWORD, IDirect3DBaseTexture8* texture) {
    note_device_call("IDirect3DDevice8::SetTexture");
    ++set_texture_calls_;
    bound_texture_ = texture;
    return D3D_OK;
}

HRESULT __stdcall fake_set_texture_stage_state(void*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) {
    note_device_call("IDirect3DDevice8::SetTextureStageState");
    return D3D_OK;
}

IDirect3DDevice8* fake_device() {
    return reinterpret_cast<IDirect3DDevice8*>(&device_);
}

// A second device, sharing the vtable: what the game hands out after it has
// destroyed the first one.
IDirect3DDevice8* fake_replacement_device() {
    return reinterpret_cast<IDirect3DDevice8*>(&other_device_);
}

class FakeManager final : public PluginManager {
public:
    void* __stdcall GetDirect3D8Device() override { return device_; }
    void set_device(void* device) { device_ = device; }

private:
    void* device_ = nullptr;
};

}  // namespace harness

namespace {

// A view that looks like the game's: translated, and a projection with a zero
// w row, which is what is_world_pass tests for. It gets no further than that
// here -- the renderer global is never resolved in a test exe -- so a pass
// never dispatches and OnWorldDraw is never reached.
void build_fake_transforms() {
    std::memset(&harness::fake_view_, 0, sizeof(harness::fake_view_));
    harness::fake_view_.m[0][0] = 1.0f;
    harness::fake_view_.m[1][1] = 1.0f;
    harness::fake_view_.m[2][2] = 1.0f;
    harness::fake_view_.m[3][0] = 120.0f;
    harness::fake_view_.m[3][1] = 3.0f;
    harness::fake_view_.m[3][2] = -40.0f;
    harness::fake_view_.m[3][3] = 1.0f;

    // A perspective projection has a zero w row, which is the test
    // is_world_pass applies to tell the world from the interface.
    std::memset(&harness::fake_projection_, 0, sizeof(harness::fake_projection_));
    harness::fake_projection_.m[0][0] = 1.3f;
    harness::fake_projection_.m[1][1] = 1.7f;
    harness::fake_projection_.m[2][2] = 1.0f;
    harness::fake_projection_.m[2][3] = 1.0f;
}

void build_fake_device() {
    for (int i = 0; i < WD_DEVICE_VTABLE_SLOTS; ++i) {
        harness::device_vtable_[i] =
            reinterpret_cast<void*>(&harness::unexpected_device_call);
    }

    // Derived, then proved against the generated constants before use.
    check(harness::vtable_index(&IDirect3DDevice8::Reset) == WD_SLOT_RESET,
        "the derived Reset slot matches the one generated from d3d8.h");
    check(harness::vtable_index(&IDirect3DDevice8::SetRenderTarget) == WD_SLOT_SET_RENDER_TARGET,
        "the derived SetRenderTarget slot matches the generated one");
    check(harness::vtable_index(&IDirect3DDevice8::DrawPrimitive) == WD_SLOT_DRAW_PRIMITIVE,
        "the derived DrawPrimitive slot matches the generated one");
    check(harness::vtable_index(&IDirect3DDevice8::DrawIndexedPrimitive)
            == WD_SLOT_DRAW_INDEXED_PRIMITIVE,
        "the derived DrawIndexedPrimitive slot matches the generated one");
    check(harness::vtable_index(&IDirect3DDevice8::DrawPrimitiveUP) == WD_SLOT_DRAW_PRIMITIVE_UP,
        "the derived DrawPrimitiveUP slot matches the generated one");
    check(harness::vtable_index(&IDirect3DDevice8::DrawIndexedPrimitiveUP)
            == WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP,
        "the derived DrawIndexedPrimitiveUP slot matches the generated one");

    const int transform_slot = harness::vtable_index(&IDirect3DDevice8::GetTransform);
    check(transform_slot > 0 && transform_slot < WD_DEVICE_VTABLE_SLOTS,
        "the derived GetTransform slot is inside the vtable");
    harness::device_vtable_[transform_slot] =
        reinterpret_cast<void*>(&harness::fake_get_transform);

    const int create_slot = harness::vtable_index(&IDirect3DDevice8::CreateVertexShader);
    const int delete_slot = harness::vtable_index(&IDirect3DDevice8::DeleteVertexShader);
    check(create_slot > 0 && create_slot < WD_DEVICE_VTABLE_SLOTS
            && delete_slot > 0 && delete_slot < WD_DEVICE_VTABLE_SLOTS,
        "the derived shader slots are inside the vtable");
    harness::device_vtable_[create_slot] =
        reinterpret_cast<void*>(&harness::fake_create_vertex_shader);
    harness::device_vtable_[delete_slot] =
        reinterpret_cast<void*>(&harness::fake_delete_vertex_shader);

    const int creation_slot = harness::vtable_index(&IDirect3DDevice8::GetCreationParameters);
    check(creation_slot > 0 && creation_slot < WD_DEVICE_VTABLE_SLOTS,
        "the derived GetCreationParameters slot is inside the vtable");
    harness::device_vtable_[creation_slot] =
        reinterpret_cast<void*>(&harness::fake_get_creation_parameters);

    const int create_texture_slot = harness::vtable_index(&IDirect3DDevice8::CreateTexture);
    check(create_texture_slot > 0 && create_texture_slot < WD_DEVICE_VTABLE_SLOTS,
        "the derived CreateTexture slot is inside the vtable");
    harness::device_vtable_[create_texture_slot] =
        reinterpret_cast<void*>(&harness::fake_create_texture);

    // The bind path, which is where a texture is made now.
    const int set_texture_slot = harness::vtable_index(&IDirect3DDevice8::SetTexture);
    const int stage_state_slot =
        harness::vtable_index(&IDirect3DDevice8::SetTextureStageState);
    check(set_texture_slot > 0 && set_texture_slot < WD_DEVICE_VTABLE_SLOTS
            && stage_state_slot > 0 && stage_state_slot < WD_DEVICE_VTABLE_SLOTS,
        "the derived bind slots are inside the vtable");
    harness::device_vtable_[set_texture_slot] =
        reinterpret_cast<void*>(&harness::fake_set_texture);
    harness::device_vtable_[stage_state_slot] =
        reinterpret_cast<void*>(&harness::fake_set_texture_stage_state);

    // And the three the engine calls on a texture it just made.
    for (int i = 0; i < 32; ++i) {
        harness::texture_vtable_[i] = reinterpret_cast<void*>(&harness::unexpected_device_call);
    }
    const int lock_slot = harness::vtable_index(&IDirect3DTexture8::LockRect);
    const int unlock_slot = harness::vtable_index(&IDirect3DTexture8::UnlockRect);
    const int release_slot = harness::vtable_index(&IDirect3DTexture8::Release);
    check(lock_slot > 0 && lock_slot < 32 && unlock_slot > 0 && unlock_slot < 32
            && release_slot >= 0 && release_slot < 32,
        "the derived texture slots are inside the texture vtable");
    harness::texture_vtable_[lock_slot] =
        reinterpret_cast<void*>(&harness::fake_texture_lock_rect);
    harness::texture_vtable_[unlock_slot] =
        reinterpret_cast<void*>(&harness::fake_texture_unlock_rect);
    harness::texture_vtable_[release_slot] =
        reinterpret_cast<void*>(&harness::fake_texture_release);

    // And a vertex buffer, which a commit and a mesh build both make through
    // the device and write through the buffer.
    const int create_buffer_slot =
        harness::vtable_index(&IDirect3DDevice8::CreateVertexBuffer);
    check(create_buffer_slot > 0 && create_buffer_slot < WD_DEVICE_VTABLE_SLOTS,
        "the derived CreateVertexBuffer slot is inside the vtable");
    harness::device_vtable_[create_buffer_slot] =
        reinterpret_cast<void*>(&harness::fake_create_vertex_buffer);

    for (int i = 0; i < 32; ++i) {
        harness::vertex_buffer_vtable_[i] =
            reinterpret_cast<void*>(&harness::unexpected_device_call);
    }
    const int buffer_lock_slot = harness::vtable_index(&IDirect3DVertexBuffer8::Lock);
    const int buffer_unlock_slot = harness::vtable_index(&IDirect3DVertexBuffer8::Unlock);
    const int buffer_release_slot = harness::vtable_index(&IDirect3DVertexBuffer8::Release);
    check(buffer_lock_slot > 0 && buffer_lock_slot < 32
            && buffer_unlock_slot > 0 && buffer_unlock_slot < 32
            && buffer_release_slot >= 0 && buffer_release_slot < 32,
        "the derived vertex-buffer slots are inside the vertex-buffer vtable");
    harness::vertex_buffer_vtable_[buffer_lock_slot] =
        reinterpret_cast<void*>(&harness::fake_buffer_lock);
    harness::vertex_buffer_vtable_[buffer_unlock_slot] =
        reinterpret_cast<void*>(&harness::fake_buffer_unlock);
    harness::vertex_buffer_vtable_[buffer_release_slot] =
        reinterpret_cast<void*>(&harness::fake_buffer_release);

    harness::device_.vtable = harness::device_vtable_;
    harness::other_device_.vtable = harness::device_vtable_;
}

// One render-thread visit, made the way the daemon makes one: the handler that
// drains everything a main thread queued. What the client does at every
// render-target change, and the only thing that can perform a queued release.
void render_thread_visit() {
    const WdHandlerSet* const set = harness::daemon_.last_set;
    if (!set) {
        check(false, "a render-thread visit needs a registered handler set");
        return;
    }
    set->pre_set_render_target(set->user, harness::fake_device(), nullptr, nullptr);
}

// Open, open again, close, close: the registration is taken once and given
// back once, whatever the consumers do.
void test_daemon_attach() {
    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    g_draw.Load(&manager);
    check(g_draw.Device() == harness::fake_device(),
        "the plugin manager's device is the one the engine acquires");

    harness::daemon_.register_result = 1;
    g_draw.Attach();

    check(harness::modules_.daemon_loads == 1, "attaching loads the daemon beside the image");
    check(harness::daemon_.acquires == 1, "attaching acquires the daemon api once");
    check(harness::daemon_.ensure_hooks == 1, "attaching calls ensure_hooks once");
    check(harness::daemon_.last_device == harness::fake_device(),
        "ensure_hooks is given the device the engine acquired");
    check(harness::daemon_.register_set == 1, "attaching calls register_set once");
    check(harness::modules_.self_loads == 1, "attaching takes one reference on this image");
    check(harness::modules_.self_frees == 0, "attaching does not release it");

    const WdHandlerSet* const set = harness::daemon_.last_set;
    check(set != nullptr, "a handler set was registered");
    if (set) {
        check(set->user == &g_draw, "the registered set carries the engine instance as user");
        check(set->abi_version == WD_DAEMON_ABI, "the registered set declares the abi it was built for");
        check(set->size == sizeof(WdHandlerSet), "the registered set declares its own size");
        check(set->pre_reset != nullptr, "the set carries pre_reset");
        check(set->post_reset != nullptr, "the set carries post_reset");
        check(set->pre_set_render_target != nullptr, "the set carries pre_set_render_target");
        check(set->pre_draw != nullptr, "the set carries pre_draw");
    }

    // A second consumer of the same image registers nothing further.
    g_draw.Attach();
    check(harness::daemon_.ensure_hooks == 1, "a second open does not hook again");
    check(harness::daemon_.register_set == 1, "a second open does not register again");
    check(harness::modules_.self_loads == 1, "a second open does not take a second reference");

    g_draw.Detach();
    check(harness::daemon_.unregister_set == 0, "closing one of two consumers keeps the registration");
    check(harness::modules_.self_frees == 0, "closing one of two consumers keeps the reference");

    g_draw.Detach();
    check(harness::daemon_.unregister_set == 1, "closing the last consumer unregisters once");
    check(harness::daemon_.last_unregistered == set, "the set unregistered is the set registered");

    // The reference is never given back, on any path. Closing is one of the
    // paths a Lua handle's __gc reaches, and lua_close can run that __gc
    // after Lua has already dropped its reference to this image -- at which
    // point a FreeLibrary of ours is the last one, and the return from it
    // lands in unmapped memory. There is no ordering that makes releasing it
    // safe, so there is no release: what it costs is a mapping that stays.
    check(harness::modules_.self_frees == 0,
        "closing the last consumer does NOT release the reference on this image");

    // And again, to prove nothing is left behind that stops a second cycle --
    // an engine that opens, closes and opens again is a //lua reload.
    g_draw.Attach();
    check(harness::modules_.daemon_loads == 1, "re-opening does not load the daemon twice");
    check(harness::daemon_.ensure_hooks == 2, "re-opening hooks again");
    check(harness::daemon_.register_set == 2, "re-opening registers again");
    check(harness::modules_.self_loads == 1,
        "re-opening needs no second reference, because the first was never given back");

    g_draw.Detach();
    check(harness::daemon_.unregister_set == 2, "re-closing unregisters again");
    check(harness::modules_.self_frees == 0,
        "and two open-and-close cycles still free this image exactly never");

    // The manager is a local of this function; nothing may still be pointing
    // at it when it goes.
    g_draw.Load(nullptr);
}

// The four handlers, called the way the daemon calls them. What is observable
// without a real device is: that pre_draw captures the pass, that
// pre_set_render_target drops the capture after it has had its look at it, and
// that post_reset's work lands on the instance named by `user` and no other.
void test_daemon_handlers() {
    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    g_draw.Load(&manager);

    harness::daemon_.register_result = 1;
    g_draw.Attach();
    const WdHandlerSet* const set = harness::daemon_.last_set;
    check(set != nullptr, "the handlers test has a registered set");
    if (!set) {
        return;
    }

    harness::get_transform_calls_ = 0;
    set->pre_draw(set->user, harness::fake_device());
    check(harness::get_transform_calls_ == 2,
        "a draw captures the pass view and projection");

    set->pre_draw(set->user, harness::fake_device());
    check(harness::get_transform_calls_ == 2,
        "a second draw in the same pass captures nothing further");

    // The load-bearing order: the render-target change reads the capture and
    // then drops it, so the next pass captures its own.
    set->pre_set_render_target(set->user, harness::fake_device(), nullptr, nullptr);
    check(harness::get_transform_calls_ == 2,
        "the render-target change captures nothing itself");

    set->pre_draw(set->user, harness::fake_device());
    check(harness::get_transform_calls_ == 4,
        "the capture is dropped by the render-target change, so the next pass takes its own");

    // pre_reset and post_reset run on the instance in `user`. A second engine
    // instance proves it: the same static handler, a different user, and the
    // device lands on that one alone. It is static because an engine instance
    // carries a 64 KiB scan buffer and a 8190-vertex batch, which is not
    // something to put on a stack.
    static LuaWorldDraw other;
    harness::FakeManager other_manager;
    void* const other_device = reinterpret_cast<void*>(&other_manager);
    other_manager.set_device(other_device);
    other.Load(&other_manager);

    WdHandlerSet forged = *set;
    forged.user = &other;
    forged.pre_reset(forged.user, harness::fake_device(), nullptr);
    forged.post_reset(forged.user, harness::fake_device(), S_OK);

    check(other.Device() == static_cast<IDirect3DDevice8*>(other_device),
        "post_reset re-acquires the device of the instance in user");
    check(g_draw.Device() == harness::fake_device(),
        "and leaves the other instance's device alone");

    // The same handler on the registered set re-acquires g_draw's own device,
    // which the manager can change under it: identity, not null-checking.
    void* const replacement = reinterpret_cast<void*>(&manager);
    manager.set_device(replacement);
    set->post_reset(set->user, harness::fake_device(), S_OK);
    check(g_draw.Device() == static_cast<IDirect3DDevice8*>(replacement),
        "a device the game replaced is re-acquired rather than kept");

    manager.set_device(harness::fake_device());
    set->post_reset(set->user, harness::fake_device(), S_OK);
    check(g_draw.Device() == harness::fake_device(), "and again when it comes back");

    g_draw.Detach();
    other.Load(nullptr);
    g_draw.Load(nullptr);
}

// The daemon is not beside the image: the message says so, once, and nothing
// is registered or referenced. Run in a process of its own, because refusing
// is sticky for the life of the image by design.
void test_daemon_missing() {
    harness::modules_.refuse_daemon_load = true;

    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    g_draw.Load(&manager);
    g_draw.Attach();

    check(harness::modules_.daemon_loads == 1, "a missing daemon is looked for once");
    check(harness::daemon_.register_set == 0, "a missing daemon registers nothing");
    check(harness::modules_.self_loads == 0, "a missing daemon takes no reference on this image");

    // Action first, no jargon, and the exact path it looked for -- which is
    // the difference between a packaging mistake and a lost download. The path
    // is this exe's own directory, so only the fixed half is compared exactly
    // and the rest is checked for being a path at all.
    check(std::strncmp(g_module_log.player,
            "worlddraw can't draw: a file is missing.\n"
            "Copy this addon's folder again from where you downloaded it.\n"
            "Missing: ", 111) == 0,
        "the missing-file message is the one the design specifies");
    check(std::strstr(g_module_log.player, "worlddraw_daemon.dll") != nullptr,
        "and it names the file it went looking for");
    check(std::strstr(g_module_log.player, ":\\") != nullptr,
        "with the full path it looked for it at");
    check(g_module_log.player_serial != 0, "and it was recorded with a serial");
    check(std::strncmp(g_module_log.engineering, "worlddraw ", 10) != 0,
        "a player-facing message goes in the player buffer and never the engineering one");
    check(g_module_log.engineering[0] == '\0',
        "and the engineering buffer is empty: the device-behaviour report is asked on the "
        "RENDER thread, and nothing of ours ever ran there");

    // Sticky: the retry that PostRender does every frame must not go looking
    // again, or a client without the file pays for it 30 times a second.
    g_draw.Tick();
    g_draw.Tick();
    check(harness::modules_.daemon_loads == 1, "a refused daemon is never looked for again");

    g_draw.Detach();
    check(harness::daemon_.unregister_set == 0, "closing after a refusal unregisters nothing");
    check(harness::modules_.self_frees == 0, "closing after a refusal releases nothing");
}

// The daemon refuses a registration. Nothing an addon does can provoke one any
// more -- the daemon's table grows -- but the engine still has to give back the
// reference it took a moment earlier, and still has to say something a player
// can act on.
void test_daemon_full() {
    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    g_draw.Load(&manager);

    harness::daemon_.register_result = 0;
    g_draw.Attach();

    check(harness::daemon_.register_set == 1, "a full daemon is asked once");
    check(harness::modules_.self_loads == 1, "the reference is taken before registering");
    check(harness::modules_.self_frees == 0,
        "and is not given back even when the registration is refused: no path frees it");
    // The player lines first and unchanged, then the one line support needs.
    // The detail deliberately names no count: the refusal at eight is gone.
    check(std::strcmp(g_module_log.player,
            "worlddraw can't draw: it failed to start.\n"
            "Restart FFXI. If it happens again, please report it.\n"
            "details: hook: the daemon refused the registration") == 0,
        "a refused registration reports the generic setup failure, with no limit named");
    check(std::strstr(g_module_log.player, "limit") == nullptr,
        "and the word limit appears nowhere in it");

    g_draw.Tick();
    check(harness::daemon_.register_set == 1, "a refused registration is not retried every frame");

    g_draw.Detach();
    check(harness::daemon_.unregister_set == 0, "closing after a refusal unregisters nothing");
    check(harness::modules_.self_frees == 0, "and still releases nothing");
}

// A foreign hooker took a slot. The daemon counts it; the engine has to say so
// once, on a low duty cycle, and change nothing else.
void test_daemon_stomp() {
    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    g_draw.Load(&manager);

    harness::daemon_.register_result = 1;
    harness::daemon_.check_slots = 0;
    harness::daemon_.check_slots_result = 0;
    // Cleared because earlier tests in this process have reported: what this
    // one measures is what the stomp poll says, not what came before it.
    g_module_log.player[0] = '\0';
    g_module_log.player_serial = 0;
    g_draw.Attach();
    const int unregisters_before = harness::daemon_.unregister_set;

    // Nothing is stomped yet, and the poll is rare: 299 frames must not reach
    // the daemon at all.
    for (int i = 0; i < 299; ++i) {
        g_draw.Tick();
    }
    check(harness::daemon_.check_slots == 0, "the stomp poll costs nothing for 299 frames");

    g_draw.Tick();
    check(harness::daemon_.check_slots == 1, "the stomp poll asks the daemon on the 300th");
    check(g_module_log.player[0] == '\0', "a clean set of slots reports nothing");

    harness::daemon_.check_slots_result = 2;
    for (int i = 0; i < 300; ++i) {
        g_draw.Tick();
    }
    check(harness::daemon_.check_slots == 2, "and asks again 300 frames later");
    check(std::strcmp(g_module_log.player,
            "worlddraw stopped drawing: another program took over the graphics.\n"
            "Restart FFXI, and load your addons before starting overlays like Discord or "
            "ReShade.") == 0,
        "a stomped slot is reported in the words the design specifies");

    // Reported once, for good: the condition never clears, and re-chaining is
    // undecidable, so there is nothing to say a second time.
    g_module_log.player[0] = '\0';
    for (int i = 0; i < 900; ++i) {
        g_draw.Tick();
    }
    check(harness::daemon_.check_slots == 2, "a reported stomp stops the polling");
    check(g_module_log.player[0] == '\0', "and is never reported twice");

    check(harness::daemon_.unregister_set == unregisters_before,
        "nothing is unregistered over a stomp");

    // And no drawing path is touched: the handlers still run and still
    // capture, which is all the engine ever does about a stomp.
    const WdHandlerSet* const set = harness::daemon_.last_set;
    set->pre_set_render_target(set->user, harness::fake_device(), nullptr, nullptr);
    const int before = harness::get_transform_calls_;
    set->pre_draw(set->user, harness::fake_device());
    check(harness::get_transform_calls_ == before + 2,
        "the pass is still captured after a stomp has been reported");

    g_draw.Detach();
    g_draw.Load(nullptr);
}

// The device the game hands out is not the device the shaders were made on.
// The handles are dropped, never deleted -- a delete would be a call into a
// device that may already have gone -- and the change is said once.
void test_device_identity() {
    harness::create_shader_calls_ = 0;
    harness::delete_shader_calls_ = 0;
    g_module_log.engineering[0] = '\0';

    check(g_draw.EnsurePipeline(harness::fake_device()),
        "the pipeline is built on the device it is given");
    check(harness::create_shader_calls_ == 2, "both shaders are made, billboard and line");
    check(g_module_log.engineering[0] == '\0', "the first build reports nothing");

    check(g_draw.EnsurePipeline(harness::fake_device()),
        "the same device again reuses what it built");
    check(harness::create_shader_calls_ == 2, "and makes nothing further");

    check(g_draw.EnsurePipeline(harness::fake_replacement_device()),
        "a device that replaced the old one gets a pipeline of its own");
    check(harness::create_shader_calls_ == 4, "which means both shaders again");
    check(harness::delete_shader_calls_ == 0,
        "and nothing is deleted through the device that went away");
    check(std::strcmp(g_module_log.engineering,
            "gpu: the graphics device changed; the shaders were dropped and rebuilt on the "
            "new one") == 0,
        "the device change is reported");
    check(g_module_log.player[0] == '\0',
        "as an engineering string, which never reaches the player buffer");

    // Once, whatever happens after it.
    g_module_log.engineering[0] = '\0';
    check(g_draw.EnsurePipeline(harness::fake_device()), "and again when the device changes back");
    check(harness::create_shader_calls_ == 6, "rebuilding once more");
    check(g_module_log.engineering[0] == '\0', "but reporting only the first time");
}

// The image's texture pool, which used to be a fixed sixty-four entries and is
// now blocks of them, chained. Exercised through the engine's own
// CreateTexture on a plugin of the harness's own -- a second instance of the
// same base class, so what runs is the shipping code and the pool it touches
// is nobody else's.
//
// It is static because an engine instance carries a 64 KiB scan buffer and an
// 8190-vertex batch, which is not something to put on a stack.
class PoolProbe final : public ffxi::WorldDrawPlugin {
public:
    const char* __stdcall GetPluginName() override { return "pool"; }
    const char* __stdcall GetPluginAuthor() override { return "harness"; }

    using ffxi::WorldDrawPlugin::BindGpuTexture;
    using ffxi::WorldDrawPlugin::CreateTexture;
    using ffxi::WorldDrawPlugin::ReleaseTexture;
    using ffxi::WorldDrawPlugin::TextureSize;

private:
    void OnWorldDraw(ffxi::WorldDraw&) override {}
};

PoolProbe pool_probe_;

void test_texture_pool() {
    constexpr int wanted = 300;
    harness::texture_creates_ = 0;
    harness::texture_releases_ = 0;

    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    pool_probe_.Load(&manager);

    const std::uint8_t pixel[4] = {1, 2, 3, 4};
    ffxi::TextureId ids[wanted] {};
    {
        harness::MainThread main_thread;
        for (int i = 0; i < wanted; ++i) {
            ids[i] = pool_probe_.CreateTexture(pixel, 1, 1);
        }
    }

    bool in_order = true;
    for (int i = 0; i < wanted; ++i) {
        in_order = in_order && ids[i] == i + 1;
    }
    check(in_order,
        "three hundred textures take ids 1..300, where the pool used to hold sixty-four");
    check(harness::texture_creates_ == 0,
        "and NONE of them reached the device: the pool is claimed on the caller's thread");

    // Index stability: an id handed out before the pool grew still resolves,
    // and to the same entry -- the blocks chain, so nothing ever moves.
    int width = 0;
    int height = 0;
    check(pool_probe_.TextureSize(ids[0], width, height) && width == 1 && height == 1,
        "an id from the first block still resolves after two more were chained on");
    check(pool_probe_.TextureSize(ids[wanted - 1], width, height),
        "and so does one from the last");
    check(!pool_probe_.TextureSize(wanted + 1, width, height),
        "an id nobody was handed resolves to nothing, which is what untextured means");
    check(!pool_probe_.TextureSize(0, width, height), "id 0 is untextured, as it always was");

    // One of them drawn with, so there is a real texture to give back.
    pool_probe_.BindGpuTexture(harness::fake_device(), ids[10]);
    check(harness::texture_creates_ == 1, "drawing with one of them makes that one");

    // Releasing frees the entry for reuse, and the id it carries is the id
    // that comes back. The device call it needs is queued.
    {
        harness::MainThread main_thread;
        pool_probe_.ReleaseTexture(ids[10]);
    }
    check(harness::texture_releases_ == 0, "releasing reaches no device on this thread");
    check(!pool_probe_.TextureSize(ids[10], width, height), "and the entry is empty");
    render_thread_visit();
    check(harness::texture_releases_ == 1, "the render thread performs it");

    ffxi::TextureId reused = 0;
    {
        harness::MainThread main_thread;
        reused = pool_probe_.CreateTexture(pixel, 2, 3);
    }
    check(reused == ids[10], "the freed entry is claimed again, under the same id");
    check(pool_probe_.TextureSize(reused, width, height) && width == 2 && height == 3,
        "carrying the size of the texture that replaced it");

    // Teardown gives back every entry in every block and frees the blocks. The
    // one live texture goes on the queue; the rest were never made, so what is
    // queued for them is the pixel copy alone.
    {
        harness::MainThread main_thread;
        pool_probe_.Unload();
    }
    check(harness::texture_releases_ == 1, "teardown releases nothing on this thread either");
    check(!pool_probe_.TextureSize(ids[0], width, height),
        "though the pool is empty afterwards");
    render_thread_visit();
    check(harness::texture_releases_ == 1,
        "and past the drain the only texture that existed is the only one released");
}

// ---- the device-behaviour report ------------------------------------------
//
// The diagnostic. Nothing in this tree knew whether FFXI creates its device
// with D3DCREATE_MULTITHREADED, and the answer decides a whole class of
// design question: with it, the D3D8 runtime serialises device access itself
// and calling a device method from the Lua thread while the game renders is
// defined; without it, it is not, and every buffer this library makes or
// writes on the Lua thread is racing the render thread.
//
// Asked once per image, so each case wants a process of its own -- the same
// reason the daemon refusals do.
void test_device_behavior(bool multithreaded, bool refuse) {
    harness::fake_behavior_flags_ = multithreaded
        ? (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED)
        : D3DCREATE_HARDWARE_VERTEXPROCESSING;
    harness::fake_creation_parameters_fail_ = refuse;
    harness::creation_parameter_calls_ = 0;

    // The registration has to be accepted, or the frame below reports a setup
    // failure of its own and the buffer under test is not the one being read.
    harness::daemon_.register_result = 1;

    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    g_draw.Load(&manager);

    check(harness::creation_parameter_calls_ == 0,
        "acquiring a device does not ask it anything by itself");

    // A frame of the front-end's, which is the main thread. It registers, and
    // it asks the device nothing: GetCreationParameters is a device method,
    // and the answer this diagnostic exists to report is the very reason one
    // may not be called from here.
    {
        harness::MainThread main_thread;
        g_draw.Tick();
    }
    check(harness::creation_parameter_calls_ == 0,
        "and neither does a front-end frame: the main thread asks the device nothing");

    render_thread_visit();
    check(harness::creation_parameter_calls_ == 1,
        "the first RENDER-thread visit asks it for its creation parameters");

    if (refuse) {
        check(std::strcmp(g_module_log.engineering,
                "gpu: device behavior flags unavailable") == 0,
            "a device that will not answer says so, rather than reporting a made-up 0");
    } else if (multithreaded) {
        check(std::strcmp(g_module_log.engineering,
                "gpu: device behavior flags 0x00000044, multithreaded=yes") == 0,
            "a multithreaded device is reported as multithreaded=yes, flags and all");
    } else {
        check(std::strcmp(g_module_log.engineering,
                "gpu: device behavior flags 0x00000040, multithreaded=no") == 0,
            "a device without D3DCREATE_MULTITHREADED is reported as multithreaded=no");
    }

    check(g_module_log.player[0] == '\0',
        "and it is an ENGINEERING string, so no player is ever shown it");
    check(std::strncmp(g_module_log.engineering, "gpu: ", 5) == 0,
        "carrying the gpu prefix that keeps it out of the chat and in last_error");

    // One shot, sticky. A device call thirty times a second to re-read a
    // constant is not a diagnostic, it is a cost.
    for (int i = 0; i < 300; ++i) {
        render_thread_visit();
    }
    check(harness::creation_parameter_calls_ == 1,
        "and it is asked exactly once, however long the client runs");

    g_draw.Detach();
    g_draw.Load(nullptr);
}

// ---- 20. commit and build once there is a device --------------------------
//
// The whole of the fix, seen from the entry points. A commit and a mesh build
// touch no device on this thread; they stage and mark, and the render thread
// performs the upload at the top of its next composite. What used to be a
// state-of-the-world failure at commit time is now a failure at drain time,
// reported through the same engineering channel and into the image's log --
// the render thread may not write g_error_slot, which is the Lua thread's.

// The composite's own first act, on the render thread and under the lock it
// holds for its whole length. drain_pending_uploads is what OnWorldDraw calls
// before it draws anything.
void composite_uploads() {
    Guard guard;
    drain_pending_uploads(harness::fake_device());
}

// The other half of test_described_before_any_device, and the first drain of
// the run: what was described with no device is put on the one that turned up,
// without the addon having asked again.
void test_the_device_arrives() {
    check(g_draw.Device() == harness::fake_device(),
        "the client has a graphics device now, which it did not when these were described");
    if (!survivor_state_ || !survivor_mesh_) {
        check(false, "the described-before-any-device handle is still here");
        return;
    }

    check(survivor_mesh_->buffer == nullptr,
        "and still nothing is on it: no frame of ours has run yet");
    check(survivor_state_->published.buffer == nullptr, "for the mesh or for the commit");

    composite_uploads();

    check(survivor_mesh_->buffer != nullptr,
        "the first composite past the device's arrival makes the mesh's buffer");
    check(survivor_mesh_->count == 3, "from the vertices it was built with");
    check(!survivor_mesh_->pending, "with nothing left pending");
    check_size(survivor_mesh_->staging.size(), 0, "and the staging given back");
    check(survivor_state_->published.buffer != nullptr,
        "and the commit made before there was a device is on it too");
    check(!survivor_state_->published.ranges.empty(), "with the ranges that draw it");

    Slot* const slot = find_slot(survivor_ref_.slot, survivor_ref_.generation);
    check(slot != nullptr, "the handle is still the one that was claimed");
    if (slot) {
        close_slot(*slot);
    }
    render_thread_visit();
    survivor_state_ = nullptr;
    survivor_mesh_ = nullptr;
}

void test_entry_points_with_device() {
    check(g_draw.Device() == harness::fake_device(),
        "the device-armed checks have the device the earlier tests left");

    HandleRef* const handle = open_handle("armed");
    check(handle != nullptr, "a handle is opened with a device present");
    if (!handle) {
        return;
    }
    SlotState* const state = state_of(handle);

    harness::lua_.number_argument = 1.0;
    check(call_entry(l_pillar) == 0, "a pillar is staged");

    // ---- an upload deferred, and then performed on the drain -------------
    const int creates_before = harness::buffer_creates_;
    check(call_entry(l_commit) == 1, "a commit with a device present returns");
    check(harness::lua_.boolean == 1, "true");
    check(harness::buffer_creates_ == creates_before,
        "having made NO buffer: the entry point does not touch the device, device or not");
    check(state->published.buffer == nullptr, "so nothing is published yet");
    check(state->pending.valid, "and the upload is pending");

    composite_uploads();
    check(harness::buffer_creates_ == creates_before + 1,
        "the render thread makes the buffer at the top of its composite");
    check(state->published.buffer != nullptr, "which is what the handle now draws from");
    check(!state->published.ranges.empty(), "described by the ranges the commit built");
    check(!state->pending.valid, "with nothing left pending");

    // ---- two commits before one drain ------------------------------------
    //
    // The second replaces the first, and the array the first built is not
    // leaked and not freed: it comes back as this handle's scratch, which is
    // where the next commit tessellates. The pointers prove it.
    check(call_entry(l_pillar) == 0, "a pillar is staged again");
    check(call_entry(l_commit) == 1, "and committed");
    const ffxi::GpuVertex* const first_array = state->pending.vertices.data();
    check(call_entry(l_pillar) == 0, "two pillars are staged");
    check(call_entry(l_pillar) == 0, "for a second commit before any frame");
    check(call_entry(l_commit) == 1, "which lands too");
    check(state->scratch.data() == first_array,
        "the array the first commit built came back as the scratch, rather than leaking");
    check(state->pending.vertices.size() == 12,
        "and what is pending is the SECOND commit's geometry, not the first's");
    const int creates_two = harness::buffer_creates_;
    composite_uploads();
    check(harness::buffer_creates_ == creates_two + 1,
        "and two commits before one frame cost that frame ONE upload, not two");
    check(state->published.ranges.size() == 1, "with one range covering both pillars");
    check(state->published.ranges[0].primitive_count == 4, "four triangles of them");

    // ---- d:clear() between a commit and the frame that uploads it --------
    //
    // Without cancelling the pending upload, the drain would put back what the
    // clear just took away and the handle would go on drawing what it had been
    // told to stop drawing.
    check(call_entry(l_pillar) == 0, "a pillar is staged");
    check(call_entry(l_commit) == 1, "and committed");
    check(state->pending.valid, "with an upload pending");
    check(call_entry(l_clear) == 0, "d:clear() before the frame that would upload it");
    check(!state->pending.valid, "cancels the pending upload");
    check(state->published.ranges.empty(), "as well as clearing what was published");
    composite_uploads();
    check(state->published.ranges.empty(),
        "and the frame that follows does NOT put the cleared geometry back");

    // Put it back to something drawable for the checks below.
    check(call_entry(l_pillar) == 0, "a pillar is staged again");
    check(call_entry(l_commit) == 1, "and committed");
    composite_uploads();
    check(state->published.ranges.size() == 1, "which draws again");

    // ---- a buffer the device refuses to make -----------------------------
    //
    // The refusal happens where the device call does, on the render thread, so
    // this is a drain-time failure now. The commit itself succeeded.
    IDirect3DVertexBuffer8* const published = state->published.buffer;
    const UINT capacity = state->published.capacity;
    const std::size_t ranges = state->published.ranges.size();

    g_module_log.engineering[0] = '\0';
    bool staged = true;
    for (int i = 0; i < 40; ++i) {
        staged = staged && call_entry(l_pillar) == 0;
    }
    check(staged, "a list too big for the published buffer is staged");
    check(call_entry(l_commit) == 1, "a commit that will not fit the buffer returns true");
    harness::buffer_create_fails_ = true;
    composite_uploads();
    harness::buffer_create_fails_ = false;
    check(std::strcmp(g_module_log.engineering,
            "draw: vertex buffer could not be created, the commit drew nothing") == 0,
        "and the drain that could not grow the buffer reports it");
    check(state->published.buffer == published, "the published buffer is the one it was");
    check(state->published.capacity == capacity, "at the size it was");
    check(state->published.ranges.size() == ranges,
        "still described by the ranges that match what is in it");
    check(!state->pending.valid,
        "and the upload is not retried every frame against a device that refused it");

    // ---- a rewrite in place the device will not lock ----------------------
    g_module_log.engineering[0] = '\0';
    check(call_entry(l_pillar) == 0, "a pillar is staged");
    check(call_entry(l_commit) == 1, "and committed");
    harness::buffer_lock_fails_ = true;
    composite_uploads();
    harness::buffer_lock_fails_ = false;
    check(std::strcmp(g_module_log.engineering,
            "draw: vertex buffer could not be written, the commit drew nothing") == 0,
        "a drain that could not write the buffer reports its own reason");
    check(state->published.buffer == published, "with the published buffer untouched");
    check(state->published.ranges.size() == ranges, "and the ranges that describe it");

    // No sticky refusal: the same handle, a world that works now.
    check(call_entry(l_pillar) == 0, "a pillar is staged once more");
    check(call_entry(l_commit) == 1, "and committed");
    composite_uploads();
    check(state->published.ranges.size() == 1, "the very next frame publishes normally");

    // ---- m:build() with a device -----------------------------------------
    harness::lua_.userdata = handle;
    check(call_entry(l_mesh) == 1, "a mesh is claimed");
    MeshRef* const mesh_ref = static_cast<MeshRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = mesh_ref;
    check(call_entry(l_mesh_tri) == 0, "a triangle is staged");

    Mesh* const mesh = state->meshes[mesh_ref->mesh];
    const int mesh_creates_before = harness::buffer_creates_;
    check(call_entry(l_mesh_build) == 1, "m:build() returns");
    check(harness::lua_.boolean == 1, "true");
    check(mesh->built, "the mesh is built");
    check(mesh->pending, "and pending an upload");
    check(mesh->buffer == nullptr, "with no buffer yet, so it does not draw");
    check(harness::buffer_creates_ == mesh_creates_before,
        "because building made no device call of any kind");
    check_size(mesh->staging.size(), 3, "and kept the vertices the upload will read");

    composite_uploads();
    check(mesh->buffer != nullptr, "the render thread makes its buffer");
    check(mesh->count == 3, "carrying the three vertices it was built from");
    check_size(mesh->staging.size(), 0, "and gives the staging back, past the upload");
    check(!mesh->pending, "with nothing left pending");

    // A mesh the drain could not build reports and does not retry.
    g_module_log.engineering[0] = '\0';
    harness::lua_.userdata = handle;
    check(call_entry(l_mesh) == 1, "a second mesh is claimed");
    MeshRef* const refused_ref = static_cast<MeshRef*>(harness::lua_.last_userdata);
    harness::lua_.userdata = refused_ref;
    check(call_entry(l_mesh_tri) == 0, "and staged");
    check(call_entry(l_mesh_build) == 1, "and built");
    Mesh* const refused = state->meshes[refused_ref->mesh];
    harness::buffer_create_fails_ = true;
    composite_uploads();
    harness::buffer_create_fails_ = false;
    check(std::strcmp(g_module_log.engineering,
            "draw: vertex buffer could not be created, the mesh drew nothing") == 0,
        "a mesh the device refused reports the mesh's own wording");
    check(refused->buffer == nullptr, "with no buffer");
    check(!refused->pending, "and no per-frame retry");

    // ---- the releases are queued, not performed ---------------------------
    harness::lua_.userdata = refused_ref;
    check(call_entry(l_mesh_free) == 0, "freeing a mesh returns");
    harness::lua_.userdata = mesh_ref;
    const int released = harness::buffer_releases_;
    check(call_entry(l_mesh_free) == 0, "and so does freeing the built one");
    check(harness::buffer_releases_ == released,
        "having released NOTHING: a Release is a device call, so it is queued");
    render_thread_visit();
    check(harness::buffer_releases_ == released + 1,
        "and the next render-thread visit performs it");

    harness::lua_.userdata = handle;
    check(call_entry(l_close) == 0, "the handle closes");
    check(harness::buffer_releases_ == released + 1,
        "still releasing nothing on this thread, whatever it is giving back");
    render_thread_visit();
    check(harness::buffer_releases_ == released + 2,
        "and the buffer the commit published goes on the next visit");
}

// ---- 20b. a texture id is usable before its texture exists ----------------
//
// CreateTexture copies the pixels and hands back an id with no device
// involved. The IDirect3DTexture8 is made by the render thread at the first
// bind that wants it; until then the id binds nothing, which is what
// untextured means and what an unknown id has always done.
class BindProbe final : public ffxi::WorldDrawPlugin {
public:
    const char* __stdcall GetPluginName() override { return "bind"; }
    const char* __stdcall GetPluginAuthor() override { return "harness"; }

    using ffxi::WorldDrawPlugin::BindGpuTexture;
    using ffxi::WorldDrawPlugin::CreateTexture;
    using ffxi::WorldDrawPlugin::ReleaseTexture;
    using ffxi::WorldDrawPlugin::TextureSize;

    // Its own copy of whatever the engine reports, because this probe is not
    // the Lua front-end and has no error log behind it. The failure it has to
    // hear about now arrives from the render thread, which is the change.
    char reported[256] {};

private:
    void OnWorldDraw(ffxi::WorldDraw&) override {}
    void OnError(const char* message) override {
        std::snprintf(reported, sizeof(reported), "%s", message ? message : "");
    }
};

BindProbe bind_probe_;

void test_texture_deferred_creation() {
    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    bind_probe_.Load(&manager);

    const std::uint8_t pixels[16] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    const int creates_before = harness::texture_creates_;
    ffxi::TextureId id = 0;
    {
        harness::MainThread main_thread;
        id = bind_probe_.CreateTexture(pixels, 2, 2);
    }
    check(id != 0, "a texture id is handed back at once");
    check(harness::texture_creates_ == creates_before,
        "with no device touched: the id exists before the texture does");

    int width = 0;
    int height = 0;
    check(bind_probe_.TextureSize(id, width, height) && width == 2 && height == 2,
        "and the size is answerable straight away, because it is the image's, not the device's");

    // Drawn with before the texture exists: untextured, and no fault.
    harness::bound_texture_ = reinterpret_cast<IDirect3DBaseTexture8*>(1);
    bind_probe_.BindGpuTexture(nullptr, id);
    check(harness::bound_texture_ == reinterpret_cast<IDirect3DBaseTexture8*>(1),
        "binding an id with no device at all does nothing and does not fault");

    // The first bind on the render thread is where it is made.
    harness::bound_texture_ = nullptr;
    bind_probe_.BindGpuTexture(harness::fake_device(), id);
    check(harness::texture_creates_ == creates_before + 1,
        "the first draw that wants the id makes the texture, on the render thread");
    check(harness::bound_texture_ != nullptr, "and binds it");
    IDirect3DBaseTexture8* const made = harness::bound_texture_;

    bind_probe_.BindGpuTexture(harness::fake_device(), id);
    check(harness::texture_creates_ == creates_before + 1, "a second bind makes nothing further");
    check(harness::bound_texture_ == made, "and binds the same texture");

    // An id nobody was handed still binds nothing.
    bind_probe_.BindGpuTexture(harness::fake_device(), 9999);
    check(harness::bound_texture_ == nullptr, "an id nobody holds is untextured");

    // A device that refuses. Reported once, marked, and never retried: the
    // same rule a shader that could not be made follows.
    bind_probe_.reported[0] = '\0';
    ffxi::TextureId refused = 0;
    {
        harness::MainThread main_thread;
        refused = bind_probe_.CreateTexture(pixels, 2, 2);
    }
    check(refused != 0, "an id is handed back for a texture the device will refuse too");
    harness::texture_create_fails_ = true;
    const int creates_at_refusal = harness::texture_creates_;
    bind_probe_.BindGpuTexture(harness::fake_device(), refused);
    harness::texture_create_fails_ = false;
    check(harness::texture_creates_ == creates_at_refusal + 1, "the device is asked once");
    check(harness::bound_texture_ == nullptr, "the id binds nothing, which is untextured");
    check(std::strcmp(bind_probe_.reported, "texture: creation failed") == 0,
        "and the refusal is reported as an engineering string");

    bind_probe_.BindGpuTexture(harness::fake_device(), refused);
    check(harness::texture_creates_ == creates_at_refusal + 1,
        "and it is never asked again: a refusal is a property of the device");

    // Releasing queues both the texture and the pixel copy.
    const int releases_before = harness::texture_releases_;
    {
        harness::MainThread main_thread;
        bind_probe_.ReleaseTexture(id);
        bind_probe_.ReleaseTexture(refused);
    }
    check(harness::texture_releases_ == releases_before,
        "giving an id back releases nothing on this thread");
    check(!bind_probe_.TextureSize(id, width, height), "though the entry is empty at once");
    render_thread_visit();
    check(harness::texture_releases_ == releases_before + 1,
        "and the render thread performs the one release there was to perform");

    // The freed entry comes back under the same id, which is what `used`
    // buys: an entry holding pixels and no texture, or neither after a
    // refusal, is not free until it is given back.
    ffxi::TextureId reused = 0;
    {
        harness::MainThread main_thread;
        reused = bind_probe_.CreateTexture(pixels, 2, 2);
    }
    check(reused == id, "the freed entry is claimed again, under the same id");

    {
        harness::MainThread main_thread;
        bind_probe_.Unload();
    }
    render_thread_visit();
}

// ---- 21. the last close releases everything the image owns -----------------
//
// The Lua teardown path is close_slot -> Detach -> Close, and Unload() -- the
// PluginBase override where release_all_textures used to be the only caller --
// is never reached by a module loaded through package.loadlib. So the textures,
// the blocks that hold them and the GDI+ instance the first image load started
// were leaked once per addon reload, for the life of the client.
//
// The order is checked as well as the fact: unregister_set drains, and nothing
// may be freed before it returns, because until it does the render thread may
// still be inside BindGpuTexture resolving an id out of the very blocks this
// frees.
class ClosingProbe final : public ffxi::WorldDrawPlugin {
public:
    const char* __stdcall GetPluginName() override { return "closing"; }
    const char* __stdcall GetPluginAuthor() override { return "harness"; }

    using ffxi::WorldDrawPlugin::BindGpuTexture;
    using ffxi::WorldDrawPlugin::Close;
    using ffxi::WorldDrawPlugin::CreateTexture;
    using ffxi::WorldDrawPlugin::LoadTexture;
    using ffxi::WorldDrawPlugin::Open;
    using ffxi::WorldDrawPlugin::TextureSize;

private:
    void OnWorldDraw(ffxi::WorldDraw&) override {}
};

ClosingProbe closing_probe_;

void test_close_releases_everything() {
    constexpr int wanted = 100;

    harness::FakeManager manager;
    manager.set_device(harness::fake_device());
    closing_probe_.Load(&manager);

    harness::gdiplus_starts_ = 0;
    harness::gdiplus_shutdowns_ = 0;
    const int releases_before = harness::texture_releases_;
    const int unregisters_before = harness::daemon_.unregister_set;

    ffxi::TextureId ids[wanted] {};
    int width = 0;
    int height = 0;
    {
        harness::MainThread main_thread;
        closing_probe_.Open();

        // A path that is certainly not an image: what is wanted is the GDI+
        // startup the first load does, not a decoded picture.
        check(closing_probe_.LoadTexture("Z:\\no\\such\\image.png") == 0,
            "an image that is not there fails to load");
        check(harness::gdiplus_starts_ == 1, "having started GDI+ to try");
        check(harness::gdiplus_shutdowns_ == 0, "which nothing has given back yet");

        const std::uint8_t pixel[4] = {1, 2, 3, 4};
        bool made = true;
        for (int i = 0; i < wanted; ++i) {
            ids[i] = closing_probe_.CreateTexture(pixel, 1, 1);
            made = made && ids[i] != 0;
        }
        check(made, "a hundred textures are made through the same image");
    }

    // Half of them drawn with, on the render thread, so half are real textures
    // and half are still a pixel copy waiting for a bind that never came.
    const int creates_before_binding = harness::texture_creates_;
    for (int i = 0; i < wanted / 2; ++i) {
        closing_probe_.BindGpuTexture(harness::fake_device(), ids[i]);
    }
    check(harness::texture_creates_ == creates_before_binding + wanted / 2,
        "and half of them drawn with, which is what makes half of them exist");

    const int creates_at_close = harness::texture_creates_;
    {
        harness::MainThread main_thread;
        closing_probe_.Close();
    }

    check(harness::daemon_.unregister_set == unregisters_before + 1,
        "closing the last consumer unregisters, which is the drain");
    check(harness::daemon_.releases_at_unregister == releases_before,
        "and NOTHING had been released when it ran: the render thread was still in there");

    // And nothing is released by the close either, because the close is on the
    // main thread. Every release the image owes is queued; the storage that
    // held them goes now, which is safe past the unregister drain.
    check(harness::texture_releases_ == releases_before,
        "the close itself releases nothing: a Release is a device call");
    check(harness::texture_creates_ == creates_at_close, "and makes nothing");
    check(!closing_probe_.TextureSize(ids[0], width, height),
        "though the blocks that held them are gone");
    check(!closing_probe_.TextureSize(ids[wanted - 1], width, height),
        "every one of them, not just the first block");
    check(harness::daemon_.shutdowns_at_unregister == 0,
        "GDI+ was still up while the drain ran");
    check(harness::gdiplus_shutdowns_ == 0, "and is not shut down on this thread either");

    // The token is dropped at the close whether or not the shutdown has been
    // performed yet, so the next load starts GDI+ afresh exactly as it did.
    {
        harness::MainThread main_thread;
        check(closing_probe_.LoadTexture("Z:\\no\\such\\image.png") == 0,
            "loading an image again after a close still fails on the file");
    }
    check(harness::gdiplus_starts_ == 2, "having started GDI+ afresh, because the token went");

    // ---- and a later composite performs every one of them ----------------
    render_thread_visit();
    check(harness::texture_releases_ == releases_before + wanted / 2,
        "the next render-thread visit releases every texture that existed");
    check(harness::gdiplus_shutdowns_ == 1, "and shuts GDI+ down");

    // And closing again, with nothing open, is a clean no-op that still
    // queues what the second load started.
    {
        harness::MainThread main_thread;
        closing_probe_.Close();
    }
    render_thread_visit();
    check(harness::gdiplus_shutdowns_ == 2, "a second close gives that one back too");
    check(harness::texture_releases_ == releases_before + wanted / 2,
        "and releases nothing twice");
}

// ---- 22. a pipeline the device refuses ------------------------------------
//
// The third state-of-the-world failure a commit can meet. Run last of
// everything, because forcing it leaves this image's engine with a shader it
// will not try to build again -- which is the header's design, and is why
// nothing after this may want to draw.
void test_pipeline_unavailable() {
    harness::fake_shader_fails_ = true;
    check(!g_draw.EnsurePipeline(harness::fake_replacement_device()),
        "a device that cannot make the shaders refuses the pipeline");
    harness::fake_shader_fails_ = false;

    HandleRef* const handle = open_handle("nopipeline");
    check(handle != nullptr, "a handle opens anyway");
    if (!handle) {
        return;
    }
    SlotState* const state = state_of(handle);

    // And the entry points do not care. A pipeline is a device thing, so
    // whether there is one is a question for the render thread and not for a
    // commit -- which is why neither of these can fail on it any more, and why
    // EnsureGpuPipeline has no caller on this thread left to reach it from.
    harness::lua_.number_argument = 1.0;
    check(call_entry(l_pillar) == 0, "a pillar is staged");
    check(call_entry(l_commit) == 1, "a commit with no pipeline does not raise");
    check(harness::lua_.boolean == 1, "and succeeds, having staged rather than drawn");
    check(state->published.buffer == nullptr, "with nothing published to a device");
    check(state->pending.valid, "and an upload pending for whenever there is one");

    harness::lua_.userdata = handle;
    check(call_entry(l_mesh) == 1, "a mesh is claimed");
    harness::lua_.userdata = harness::lua_.last_userdata;
    check(call_entry(l_mesh_tri) == 0, "and staged");
    check(call_entry(l_mesh_build) == 1, "m:build() with no pipeline does not raise either");
    check(harness::lua_.boolean == 1, "and succeeds for the same reason");

    harness::lua_.userdata = handle;
    check(call_entry(l_close) == 0, "and the handle closes");
}

// ---- 23. The gate ---------------------------------------------------------
//
// The one check the whole change exists for, and the last thing this process
// asks. Every device-shaped object in this harness announces every call it is
// asked for, and call_entry -- the only way an l_* entry point is invoked here
// -- declares the thread it is invoked on. If any of those calls landed inside
// one of those windows, this is where the run fails, and the name of the first
// one has already been printed beside it.
void test_no_device_calls_from_the_main_thread() {
    if (harness::first_main_thread_call_) {
        std::printf("         the first was %s\n", harness::first_main_thread_call_);
    }
    check(harness::main_thread_device_calls_ == 0,
        "NO DEVICE METHOD IS REACHABLE FROM A LUA ENTRY POINT, over every check above");
}

}  // namespace

namespace {

int report_total() {
    std::printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed == 0) {
        std::printf("PASS   : the CPU half of worlddraw.cpp behaves\n");
        return 0;
    }
    std::printf("FAIL\n");
    return 1;
}

}  // namespace

// Refusing a daemon is sticky for the life of the image, by design: a client
// missing the file must not pay for the lookup thirty times a second. Each
// refusal case therefore wants a process of its own, and the run mode says
// which process this one is.
int main(int argc, char** argv) {
    const char* const mode = argc > 1 ? argv[1] : "";

    if (std::strcmp(mode, "daemon-missing") == 0) {
        // The fake device is built here too: the one-shot device-behaviour
        // report asks the device a question on the first frame, and a device
        // with no vtable behind it is not something to ask.
        build_fake_device();
        test_daemon_missing();
        return report_total();
    }
    if (std::strcmp(mode, "daemon-full") == 0) {
        build_fake_device();
        test_daemon_full();
        return report_total();
    }

    // The device-behaviour report is one shot per image by design, so each
    // answer it can give wants a process of its own -- the same reason the
    // daemon refusals do.
    if (std::strcmp(mode, "device-flags") == 0) {
        build_fake_device();
        test_device_behavior(false, false);
        return report_total();
    }
    if (std::strcmp(mode, "device-flags-mt") == 0) {
        build_fake_device();
        test_device_behavior(true, false);
        return report_total();
    }
    if (std::strcmp(mode, "device-flags-refused") == 0) {
        build_fake_device();
        test_device_behavior(false, true);
        return report_total();
    }

    if (mode[0] != '\0') {
        std::printf("FAIL   : unknown run mode %s\n", mode);
        return 1;
    }

    // First, before anything gives this image a device: the Lua entry points
    // in the window an addon actually loads in. Once a test has handed the
    // engine a device it keeps it, so this is the only place these can run.
    test_registration();
    test_lua_entry_points();
    test_error_log();
    test_mesh_collection();

    test_counts();
    test_pillar();
    test_sprite();
    test_panel();
    test_line();
    test_ring();
    test_triangle();
    test_ranges();
    test_growth();
    test_mesh_staging();
    test_world_matrix();
    test_model_space_basis();
    test_multiply();
    test_vertex_layout();
    test_width_floor();
    test_doubled_table();
    test_handle_growth();
    test_mesh_growth();
    test_texture_list_growth();
    test_mesh_past_old_cap();

    // Last of the no-device era, and after the growth checks, because it
    // keeps the slot it claims: what it describes has to still be there when
    // a device turns up further down.
    test_described_before_any_device();

    // Last: they leave a device and a plugin manager on the module's one
    // instance, which the checks above are better off never seeing.
    build_fake_transforms();
    build_fake_device();
    test_daemon_attach();
    test_daemon_handlers();
    test_daemon_stomp();

    // Last of all: it leaves shader handles from a fake device on the module's
    // one instance, and nothing after it should have to know that.
    test_device_identity();

    // Which is what the entry points then draw through -- starting with the
    // handle that described everything it has before there was a device.
    test_the_device_arrives();
    test_entry_points_with_device();

    // And after that, on plugin instances of their own with nothing registered.
    test_texture_deferred_creation();
    test_texture_pool();
    test_close_releases_everything();

    // Dead last: it leaves this image's engine holding a pipeline it will not
    // try to rebuild, which is the header's design and nothing after it could
    // draw through.
    test_pipeline_unavailable();

    // The gate, over everything above it: no l_* entry point reached a device.
    test_no_device_calls_from_the_main_thread();

    return report_total();
}
