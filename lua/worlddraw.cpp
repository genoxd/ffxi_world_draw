// worlddraw - Lua front-end for ffxi_world_draw.

#define FFXI_WORLD_DRAW_IMAGE_LOADING
#include "../ffxi_world_draw.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace {

// First sizes, not limits: every table below doubles when it fills.
constexpr int initial_handles_ = 4;
constexpr int initial_commands_ = 256;
constexpr int initial_meshes_ = 16;
constexpr int initial_handle_textures_ = 16;

// The engine's own longest message, so a handle's copy never truncates it.
// Fixed storage, never a heap string: OnError is reached from inside a
// composite, on the render thread, where nothing may allocate.
constexpr std::size_t max_error_ = ffxi::WorldDrawPlugin::max_message_;

// The most vertices one command, or one committed list, may tessellate into:
// the header's own overflow guard, so a size is refused before it wraps.
constexpr std::size_t max_vertices_ =
    static_cast<std::size_t>(ffxi::WorldDrawPlugin::max_gpu_vertices_);

// The mesh upload's wire format: five little-endian float32s a vertex, three
// vertices a triangle.
constexpr std::size_t triangle_bytes_ = 60;
constexpr std::size_t vertex_bytes_ = 20;

const char handle_type_[] = "worlddraw.handle";
const char mesh_type_[] = "worlddraw.mesh";

// The message contract, stated beside report_setup_failure in
// ffxi_world_draw.h and parsed by worlddraw.lua, sorts every message this
// library reports into two kinds: player-facing, beginning "worlddraw ", and
// engineering, beginning with a subsystem prefix ("texture: ", "gpu: ",
// "hook: ", "draw: ", "scan: ").
//
// Two buffers, never one: with a single slot an engineering string displaces
// the player-facing message an addon still has to show, and hides it for the
// life of the handle.
//
// Recency is a serial, not a timestamp and not "whichever buffer is not
// empty": one counter for the image, stamped on every record, so "most recent
// of either kind" stays answerable whichever buffer was written last. A serial
// of zero means the buffer holds nothing to say -- nothing was ever recorded
// there, or the player-facing message that was there has been taken.
//
// The serials are LONG rather than an unsigned count because the player-facing
// one is also the token take_player_message compares and swaps.
struct ErrorLog {
    char player[max_error_] {};
    char engineering[max_error_] {};
    volatile LONG player_serial = 0;       // 0 means the buffer holds nothing
    volatile LONG engineering_serial = 0;
};

// Interlocked because OnError is reached from the render thread as well as the
// Lua one, and two records that raced must still get two different serials.
// The buffers themselves are written unsynchronised: nothing inside OnError
// may allocate or block.
volatile LONG g_error_serial = 0;

LONG next_error_serial() {
    return InterlockedIncrement(&g_error_serial);
}

bool message_is_player_facing(const char* message) {
    return std::strncmp(message, "worlddraw ", 10) == 0;
}

// The text goes down before the serial that publishes it. take_player_message
// depends on that order: a serial it has read is a message that was whole when
// the stamp landed.
void record_error(ErrorLog& log, const char* message) {
    const bool player = message_is_player_facing(message);
    std::snprintf(player ? log.player : log.engineering, max_error_, "%s", message);
    if (player) {
        log.player_serial = next_error_serial();
    } else {
        log.engineering_serial = next_error_serial();
    }
}

// A losing attempt costs nothing but a repeat of the copy, and there is
// nothing to gain from more than a handful: what fails them all is a message
// recorded on the render thread during every one of them, and that message is
// still there for the next poll.
constexpr int take_attempts_ = 4;

// The counterpart of record_error for the player-facing buffer, which is a
// handoff rather than a record: the message is taken, and the buffer has
// nothing to say again until the engine records another.
//
// The take is atomic against the render thread, which records without a lock
// and cannot be given one: it is a compare-and-swap on the serial, and wins
// only if no message landed between reading that serial and taking it. One
// that did land fails the swap and is left where it is, with its own serial,
// for the next attempt or the next poll -- nothing is dropped and nothing is
// chatted twice. Nothing here writes the buffer, so a record in flight cannot
// be truncated by a taker either.
bool take_player_message(ErrorLog& log, char* out, std::size_t size) {
    for (int attempt = 0; attempt < take_attempts_; ++attempt) {
        const LONG seen = log.player_serial;
        if (seen == 0 || log.player[0] == '\0') {
            return false;
        }

        std::snprintf(out, size, "%s", log.player);
        if (InterlockedCompareExchange(&log.player_serial, 0, seen) == seen) {
            return true;
        }
    }
    return false;
}

struct Command {
    enum Kind {
        Pillar,
        Ring,
        Line,
        Panel,
        Sprite,
        Triangle,
    };

    Kind kind = Pillar;
    // The widest command's arguments: a triangle's nine, a line's seven.
    float v[12] {};
    DWORD color = 0xFFFFFFFF;
    ffxi::TextureId texture = 0;
    int segments = 128;
};

// An overflow guard, not a limit: past this a count's size in bytes cannot be
// expressed, nor its index held in the int every table here counts with.
template <typename T>
constexpr std::size_t table_ceiling_ =
    std::numeric_limits<std::size_t>::max() / sizeof(T)
            < static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<std::size_t>::max() / sizeof(T)
        : static_cast<std::size_t>(std::numeric_limits<int>::max());

// Grown on demand while commands are being described. A list never shrinks;
// its buffer is released when the handle closes.
struct CommandList {
    Command* items = nullptr;
    int capacity = 0;
    int count = 0;
};

// One draw out of a handle's committed buffer: a run of vertices that share a
// shader and a texture. Built by commit, walked by the render thread.
struct Range {
    ffxi::GpuShader shader = ffxi::GpuShaderBillboard;
    ffxi::TextureId texture = 0;
    UINT start_vertex = 0;
    UINT primitive_count = 0;
};

// What a handle's committed immediate list draws from. Replaced whole under
// the Guard: the render thread walks it for the length of a composite.
struct Published {
    IDirect3DVertexBuffer8* buffer = nullptr;
    UINT capacity = 0;  // vertices the buffer was made to hold
    std::vector<Range> ranges;
};

// What a commit hands over: vertices and ranges built on the caller's thread,
// waiting for the render thread to put them on the device.
//
// Two arrays, alternating: stage_draw_data swaps the handle's scratch with
// this, so the storage one commit used comes back as the scratch the next
// builds in and a handle drawing the same scene every frame allocates nothing.
struct PendingUpload {
    std::vector<ffxi::GpuVertex> vertices;
    std::vector<Range> ranges;
    bool valid = false;
};

struct Mesh {
    bool used = false;
    std::uint32_t generation = 1;
    bool built = false;
    bool visible = false;
    ffxi::TextureId texture = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float facing = 0.0f;
    float scale = 1.0f;
    IDirect3DVertexBuffer8* buffer = nullptr;
    int count = 0;  // vertices in the buffer
    // Built, and the render thread has not made its buffer yet. `staging` is
    // what it will make it from, so it is kept until then. A mesh in this state
    // does not draw, and appears on the next frame.
    bool pending = false;
    std::vector<ffxi::GpuVertex> staging;
};

// Everything a handle owns. Allocated when the handle is claimed, freed when
// it closes.
struct SlotState {
    CommandList list;
    Published published;

    // Where a commit tessellates before it publishes. Kept on the handle, at
    // their peak size, until it closes, so a repeated commit allocates nothing.
    std::vector<ffxi::GpuVertex> scratch;
    std::vector<Range> scratch_ranges;

    // What the last commit handed to the render thread. See PendingUpload: it
    // and the two above swap, so no commit allocates.
    PendingUpload pending;

    // A table of pointers to meshes, doubled when it fills, every mesh in an
    // allocation of its own: the table moves, the meshes never do, so a
    // MeshRef's index and a Mesh* both stay good across a growth. Entries
    // [0, mesh_count) are live and are what the render thread walks.
    Mesh** meshes = nullptr;
    int mesh_capacity = 0;
    int mesh_count = 0;

    // What this handle loaded and has to release when it closes.
    ffxi::TextureId* textures = nullptr;
    int texture_capacity = 0;
    int texture_count = 0;

    // This handle's own two messages. Everything reported while one of its
    // calls is running lands here, everything else in the module's log, and
    // both are consulted when a handle is asked.
    ErrorLog log;
};

struct Slot {
    bool active = false;
    std::uint32_t generation = 1;
    // The name the caller gave, in full: a heap copy, never a fixed field that
    // would silently shorten it.
    char* name = nullptr;
    SlotState* state = nullptr;
};

struct HandleRef {
    int slot;
    std::uint32_t generation;
};

struct MeshRef {
    int slot;
    std::uint32_t slot_generation;
    int mesh;
    std::uint32_t mesh_generation;
};

// The handle table, doubled when it fills. A HandleRef holds an index into it,
// never a pointer, so a growth that moves the table invalidates no handle, and
// the generation counters move with the slots, so a closed handle stays closed.
Slot* g_slots = nullptr;
int g_slot_capacity = 0;
int g_open_handles = 0;

// Which handle a message reported right now belongs to, or -1 for "the image
// itself". Set around a handle's own calls and never left set across anything
// that can raise: a Lua error leaves through a longjmp and would strand it.
int g_error_slot = -1;

// What the image reports when no handle owns the failure: the daemon missing,
// the device's behaviour flags, a stomped hook slot. Every handle sees it.
ErrorLog g_module_log;

// The render thread composites inside a device hook while Lua runs on the
// game's main thread; state both can see is taken here. Never raise a Lua
// error while it is held -- luaL_error longjmps out and no destructor runs.
//
// No destructor, deliberately: a namespace-scope object with one is registered
// to run at process exit whatever DllMain does, and deleting a critical section
// the render thread may still be inside is a crash with no message. The section
// is leaked into a process that is ending anyway.
struct Section {
    Section() { InitializeCriticalSection(&critical_section); }

    CRITICAL_SECTION critical_section {};
};

Section g_section;

struct Guard {
    Guard() { EnterCriticalSection(&g_section.critical_section); }
    ~Guard() { LeaveCriticalSection(&g_section.critical_section); }
};

// A doubled copy of a table, with the `used` entries at its front carried
// across. The old table is neither freed nor altered: the caller installs the
// new one under the Guard and frees the old, so the allocation -- which the
// render thread must never wait on -- happens outside the lock.
//
// Null when the allocation failed, with nothing touched, and the caller raises
// the Lua error. Nothing here may throw: an exception unwinding through Lua's
// C frames takes the client down.
template <typename T>
T* doubled_table(const T* items, int capacity, int used, int initial, int& out_capacity) {
    static_assert(std::is_trivially_copyable<T>::value,
        "the entries already in the table are moved to the new one with memcpy");

    const std::size_t wanted = capacity > 0
        ? static_cast<std::size_t>(capacity) * 2
        : static_cast<std::size_t>(initial);
    if (wanted > table_ceiling_<T>) {
        return nullptr;
    }

    T* grown = new (std::nothrow) T[wanted];
    if (!grown) {
        return nullptr;
    }

    if (used > 0 && items) {
        std::memcpy(grown, items, static_cast<std::size_t>(used) * sizeof(T));
    }

    out_capacity = static_cast<int>(wanted);
    return grown;
}

// Doubles a list's buffer. No lock: the command list is described, grown and
// tessellated on the Lua thread, and the render thread never reads it -- what
// a composite walks is the vertex buffer and ranges a commit published.
//
// False only when the allocation failed, the list left exactly as it was, and
// the caller raises the Lua error.
bool grow_commands(CommandList& list) {
    int capacity = 0;
    Command* items = doubled_table(list.items, list.capacity, list.count,
        initial_commands_, capacity);
    if (!items) {
        return false;
    }

    delete[] list.items;
    list.items = items;
    list.capacity = capacity;
    return true;
}

// False only when that growth could not allocate; the caller raises the Lua
// error.
bool add_command(SlotState& state, const Command& command) {
    CommandList& list = state.list;
    if (list.count >= list.capacity && !grow_commands(list)) {
        return false;
    }
    list.items[list.count++] = command;
    return true;
}

void free_command_storage(CommandList& list) {
    delete[] list.items;
    list.items = nullptr;
    list.capacity = 0;
    list.count = 0;
}

// All the growth a staged append needs, taken here so the append itself cannot
// throw: an exception unwinding through Lua's C frames takes the client down.
// The caller raises the Lua error, outside the catch.
//
// Allocation is the only thing that bounds a mesh.
bool reserve_staging(Mesh& mesh, std::size_t added) {
    try {
        mesh.staging.reserve(mesh.staging.size() + added);
    } catch (...) {
        return false;
    }
    return true;
}

// The Lua API speaks Windower's coordinates -- x east/west, y north/south,
// z raw game height with negative being up. The GPU speaks the Direct3D axes
// the captured view-projection is in: x east/west, y raw game height,
// z north/south. The swap happens here, once, as a command or mesh vertex is
// tessellated; past this point nothing is in Windower's axes:
//
//   d3d.x = lua.x       east/west
//   d3d.y = lua.z       raw game height, still growing downward
//   d3d.z = lua.y       north/south
//
// so up is (0, -1, 0), and the ground-plane camera right the library reports
// as (east/west, north/south) becomes (right_x, 0, right_y).

struct Point {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Point to_d3d(float x, float y, float z) {
    Point point;
    point.x = x;
    point.y = z;
    point.z = y;
    return point;
}

void put_vertex(ffxi::GpuVertex& out, const Point& anchor, DWORD color,
    float u, float v, float ox, float oy, float px, float py) {
    out.x = anchor.x;
    out.y = anchor.y;
    out.z = anchor.z;
    out.color = color;
    out.u = u;
    out.v = v;
    out.ox = ox;
    out.oy = oy;
    out.px = px;
    out.py = py;
}

// The header's Quad is Triangle(a, b, c) then Triangle(c, b, d), and these six
// vertices must go out in exactly that order to match it.
void put_quad(ffxi::GpuVertex* out, const ffxi::GpuVertex& a, const ffxi::GpuVertex& b,
    const ffxi::GpuVertex& c, const ffxi::GpuVertex& d) {
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = c;
    out[4] = b;
    out[5] = d;
}

// WorldDraw::Ring's own rule, applied where the geometry is made rather than
// where the command is staged, so the staged number stays what Lua said. A
// count, and the angle divides by it; the ceiling is arithmetic alone, a ring
// tessellating into segments * 6 vertices.
int clamp_segments(int segments) {
    constexpr int segment_ceiling = static_cast<int>(max_vertices_ / 6);
    if (segments < 1) {
        return 1;
    }
    if (segments > segment_ceiling) {
        return segment_ceiling;
    }
    return segments;
}

// The unit direction of a line command in Direct3D axes. False when the two
// endpoints coincide or a NaN got in: there is no direction to widen across,
// so the command draws nothing.
bool line_direction(const Command& command, Point& direction) {
    const Point from = to_d3d(command.v[0], command.v[1], command.v[2]);
    const Point to = to_d3d(command.v[3], command.v[4], command.v[5]);
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    const float length_squared = dx * dx + dy * dy + dz * dz;
    if (!(length_squared > 0.0f)) {
        return false;
    }

    const float scale = 1.0f / std::sqrt(length_squared);
    direction.x = dx * scale;
    direction.y = dy * scale;
    direction.z = dz * scale;
    return true;
}

// Vertices this command tessellates into. The counting pass and the writing
// pass must read the same command, or they can disagree about a size.
std::size_t command_vertices(const Command& command) {
    switch (command.kind) {
    case Command::Ring:
        return static_cast<std::size_t>(clamp_segments(command.segments)) * 6;
    case Command::Line: {
        Point direction;
        return line_direction(command, direction) ? 6 : 0;
    }
    case Command::Triangle:
        return 3;
    case Command::Pillar:
    case Command::Panel:
    case Command::Sprite:
    default:
        return 6;
    }
}

ffxi::GpuShader command_shader(const Command& command) {
    return command.kind == Command::Line ? ffxi::GpuShaderLine : ffxi::GpuShaderBillboard;
}

ffxi::TextureId command_texture(const Command& command) {
    return command.kind == Command::Panel || command.kind == Command::Sprite
        ? command.texture : 0;
}

// A camera-facing bar from (x, y, z) rising `height`, `width` across: the
// billboard shader's offsets along the ground-plane camera right, with the two
// ends baked into the anchors. Reproduces WorldDraw::Pillar.
void tessellate_pillar_shape(ffxi::GpuVertex* out, float x, float y, float z,
    float width, float height, DWORD color) {
    const Point bottom = to_d3d(x, y, z);
    const Point top = to_d3d(x, y, z - height);
    const float half = width * 0.5f;

    ffxi::GpuVertex a, b, c, d;
    put_vertex(a, bottom, color, 0.0f, 0.0f, -half, 0.0f, 0.0f, 0.0f);
    put_vertex(b, bottom, color, 0.0f, 0.0f, half, 0.0f, 0.0f, 0.0f);
    put_vertex(c, top, color, 0.0f, 0.0f, -half, 0.0f, 0.0f, 0.0f);
    put_vertex(d, top, color, 0.0f, 0.0f, half, 0.0f, 0.0f, 0.0f);
    put_quad(out, a, b, c, d);
}

// Writes exactly command_vertices(command) vertices at `out`.
void tessellate(const Command& command, ffxi::GpuVertex* out) {
    switch (command.kind) {
    case Command::Pillar:
        tessellate_pillar_shape(out, command.v[0], command.v[1], command.v[2],
            command.v[3], command.v[4], command.color);
        break;

    case Command::Ring: {
        // WorldDraw::Ring: an upright band from z to z - thickness at radius
        // `radius`, one quad per segment, walked anticlockwise from angle 0.
        const int segments = clamp_segments(command.segments);
        const float x = command.v[0];
        const float y = command.v[1];
        const float z = command.v[2];
        const float radius = command.v[3];
        const float top_z = z - command.v[4];

        for (int i = 0; i < segments; ++i) {
            const float previous_angle =
                6.28318530718f * static_cast<float>(i) / static_cast<float>(segments);
            const float angle =
                6.28318530718f * static_cast<float>(i + 1) / static_cast<float>(segments);
            const float previous_x = x + radius * std::cos(previous_angle);
            const float previous_y = y + radius * std::sin(previous_angle);
            const float point_x = x + radius * std::cos(angle);
            const float point_y = y + radius * std::sin(angle);

            ffxi::GpuVertex a, b, c, d;
            put_vertex(a, to_d3d(previous_x, previous_y, z), command.color,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            put_vertex(b, to_d3d(point_x, point_y, z), command.color,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            put_vertex(c, to_d3d(previous_x, previous_y, top_z), command.color,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            put_vertex(d, to_d3d(point_x, point_y, top_z), command.color,
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            put_quad(out + static_cast<std::size_t>(i) * 6, a, b, c, d);
        }
        break;
    }

    case Command::Line: {
        // The line shader's declaration reads ox as the half width and
        // (oy, px, py) as the direction -- see gpu_line_declaration.
        Point direction;
        if (!line_direction(command, direction)) {
            break;
        }

        const Point from = to_d3d(command.v[0], command.v[1], command.v[2]);
        const Point to = to_d3d(command.v[3], command.v[4], command.v[5]);
        const float half = command.v[6] * 0.5f;

        ffxi::GpuVertex a, b, c, d;
        put_vertex(a, from, command.color, 0.0f, 0.0f,
            -half, direction.x, direction.y, direction.z);
        put_vertex(b, from, command.color, 0.0f, 0.0f,
            half, direction.x, direction.y, direction.z);
        put_vertex(c, to, command.color, 0.0f, 0.0f,
            -half, direction.x, direction.y, direction.z);
        put_vertex(d, to, command.color, 0.0f, 0.0f,
            half, direction.x, direction.y, direction.z);
        put_quad(out, a, b, c, d);
        break;
    }

    case Command::Panel: {
        // WorldDraw::Panel: a standing rectangle that does not turn with the
        // camera, so its corners are world points and its offsets are zero.
        // The width runs across the way it faces; 0 faces east and the angle
        // increases anticlockwise.
        const float x = command.v[0];
        const float y = command.v[1];
        const float z = command.v[2];
        const float half_width = command.v[3] * 0.5f;
        const float half_height = command.v[4] * 0.5f;
        const float facing = command.v[5];
        const float offset_x = -std::sin(facing) * half_width;
        const float offset_y = std::cos(facing) * half_width;
        const float bottom = z + half_height;
        const float top = z - half_height;

        ffxi::GpuVertex a, b, c, d;
        put_vertex(a, to_d3d(x - offset_x, y - offset_y, bottom), command.color,
            0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        put_vertex(b, to_d3d(x + offset_x, y + offset_y, bottom), command.color,
            1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        put_vertex(c, to_d3d(x - offset_x, y - offset_y, top), command.color,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        put_vertex(d, to_d3d(x + offset_x, y + offset_y, top), command.color,
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        put_quad(out, a, b, c, d);
        break;
    }

    case Command::Sprite: {
        // WorldDraw::Sprite: centred on the point, width along the ground-plane
        // camera right and height along world up, in yalms. Bottom is
        // z + half height because raw game height grows downward.
        const Point anchor = to_d3d(command.v[0], command.v[1], command.v[2]);
        const float half_width = command.v[3] * 0.5f;
        const float half_height = command.v[4] * 0.5f;

        ffxi::GpuVertex a, b, c, d;
        put_vertex(a, anchor, command.color, 0.0f, 1.0f,
            -half_width, -half_height, 0.0f, 0.0f);
        put_vertex(b, anchor, command.color, 1.0f, 1.0f,
            half_width, -half_height, 0.0f, 0.0f);
        put_vertex(c, anchor, command.color, 0.0f, 0.0f,
            -half_width, half_height, 0.0f, 0.0f);
        put_vertex(d, anchor, command.color, 1.0f, 0.0f,
            half_width, half_height, 0.0f, 0.0f);
        put_quad(out, a, b, c, d);
        break;
    }

    case Command::Triangle:
        for (int i = 0; i < 3; ++i) {
            put_vertex(out[i],
                to_d3d(command.v[i * 3], command.v[i * 3 + 1], command.v[i * 3 + 2]),
                command.color, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        break;
    }
}

// A mesh's triangle: model space, straight through with zero offsets.
void stage_mesh_triangle(Mesh& mesh, const float* xyzuv, DWORD color) {
    for (int i = 0; i < 3; ++i) {
        const float* v = xyzuv + i * 5;
        ffxi::GpuVertex vertex;
        put_vertex(vertex, to_d3d(v[0], v[1], v[2]), color, v[3], v[4],
            0.0f, 0.0f, 0.0f, 0.0f);
        mesh.staging.push_back(vertex);
    }
}

// A mesh's marker: a pillar's shape in model space. Its offsets ride the camera
// right that the render thread rotates into this mesh's model space, so a
// marker inside a turned mesh still faces the camera.
void stage_mesh_mark(Mesh& mesh, float x, float y, float z, float width, float height,
    DWORD color) {
    ffxi::GpuVertex vertices[6];
    tessellate_pillar_shape(vertices, x, y, z, width, height, color);
    for (int i = 0; i < 6; ++i) {
        mesh.staging.push_back(vertices[i]);
    }
}

// Model space to world, in Direct3D axes, as the row-vector matrix c0-c3 want
// (p_world = p_model * out).
//
// The ground-plane turn is a rotation about the Direct3D y axis with the
// opposite sign to D3DXMatrixRotationY: swapping two axes and leaving height
// pointing down reverses the sense of the angle. `facing` therefore means what
// Panel means by it -- 0 faces east, increasing anticlockwise from above.
void mesh_world_matrix(const Mesh& mesh, D3DMATRIX& out) {
    const float cosine = std::cos(mesh.facing) * mesh.scale;
    const float sine = std::sin(mesh.facing) * mesh.scale;

    out.m[0][0] = cosine; out.m[0][1] = 0.0f;       out.m[0][2] = sine;   out.m[0][3] = 0.0f;
    out.m[1][0] = 0.0f;   out.m[1][1] = mesh.scale; out.m[1][2] = 0.0f;   out.m[1][3] = 0.0f;
    out.m[2][0] = -sine;  out.m[2][1] = 0.0f;       out.m[2][2] = cosine; out.m[2][3] = 0.0f;
    out.m[3][0] = mesh.x; out.m[3][1] = mesh.z;     out.m[3][2] = mesh.y; out.m[3][3] = 1.0f;
}

// out = a * b, row-vector order: a point goes through a and then through b.
void multiply_matrix(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out) {
    D3DMATRIX result {};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.m[row][column] = a.m[row][0] * b.m[0][column]
                + a.m[row][1] * b.m[1][column]
                + a.m[row][2] * b.m[2][column]
                + a.m[row][3] * b.m[3][column];
        }
    }
    out = result;
}

// A world direction expressed in a mesh's model space. The shader adds the
// offsets to the anchor before the world matrix, so a direction meant to be the
// camera right in the world must arrive already turned back by the mesh's
// rotation and divided by its scale. A zero scale flattens the offsets rather
// than producing infinities.
void to_model_space(const Mesh& mesh, const float* world3, float* out3) {
    const float cosine = std::cos(mesh.facing);
    const float sine = std::sin(mesh.facing);
    const float inverse_scale =
        std::fabs(mesh.scale) > 1.0e-6f ? 1.0f / mesh.scale : 0.0f;

    out3[0] = (world3[0] * cosine + world3[2] * sine) * inverse_scale;
    out3[1] = world3[1] * inverse_scale;
    out3[2] = (-world3[0] * sine + world3[2] * cosine) * inverse_scale;
}

// Render thread only, and defined past g_draw because it drives it. Declared
// here because OnWorldDraw calls it and a member body sees only the names that
// were visible when its class was defined.
void drain_pending_uploads(IDirect3DDevice8* device);

class LuaWorldDraw final : public ffxi::WorldDrawPlugin {
public:
    const char* __stdcall GetPluginName() override { return "worlddraw"; }
    const char* __stdcall GetPluginAuthor() override { return "worlddraw"; }

    void Attach() { Open(); }
    void Detach() { ffxi::WorldDrawPlugin::Close(); }
    void Tick() { PostRender(); }

    // The daemon resident in this client, for wd.version(). Two addons can ship
    // different daemon builds at one ABI and whichever pinned first holds the
    // client for the session.
    std::uint32_t ResidentDaemonAbi() const { return DaemonAbi(); }
    const char* ResidentDaemonBuild() const { return DaemonBuild(); }

    ffxi::TextureId LoadImage(const char* path) { return LoadTexture(path); }
    void ReleaseImage(ffxi::TextureId id) { ReleaseTexture(id); }

    // The shader-driven pipeline. Render thread only, every one of them: they
    // are device methods. The Lua thread reaches none of these -- a commit and
    // a mesh build stage vertices and mark them pending, and
    // drain_pending_uploads performs the upload at the top of the composite.
    bool EnsurePipeline(IDirect3DDevice8* device) { return EnsureGpuPipeline(device); }
    IDirect3DVertexBuffer8* CreateBuffer(IDirect3DDevice8* device,
        const ffxi::GpuVertex* vertices, UINT count) {
        return CreateGpuBuffer(device, vertices, count);
    }
    bool UpdateBuffer(IDirect3DVertexBuffer8* buffer, const ffxi::GpuVertex* vertices,
        UINT count) {
        return UpdateGpuBuffer(buffer, vertices, count);
    }

    // Which device the image has, for a caller that needs to know whether there
    // is one at all. Not a device call: it asks Windower's PluginManager or
    // reads the game's renderer global.
    IDirect3DDevice8* Device() { return GpuDevice(); }

    // A buffer this front-end made and is giving back. Queued for the render
    // thread; see the thread rule in ffxi_world_draw.h.
    void DeferBuffer(IDirect3DVertexBuffer8* buffer) { DeferVertexBufferRelease(buffer); }

    // Where the character's model is drawn, which trails the position the game
    // reports while moving. Resolved on the first call, on the Lua thread.
    bool PlayerDrawPosition(float& x, float& y, float& z) {
        if (!scan_attempted_) {
            scan_attempted_ = true;
            resolve_player_globals();
        }

        return player_position(x, y, z);
    }

public:
    // The engine's own channel, for failures the header cannot see. Same
    // contract, same two buffers, same serial.
    void Report(const char* message) { ReportError(message); }

private:
    // Whatever the engine reports, filed against the thing it is about. A
    // player-facing message is never one handle's -- every one of them is a
    // property of the client -- so it goes in the image's log, where every
    // handle sees it. An engineering string is filed against the handle whose
    // call produced it, and against the image when there is none.
    //
    // Reached from both threads, and it takes no lock: nothing in here may
    // block, and a torn message is a worse-formatted string rather than a
    // crash. Known narrow race: a render-thread report landing during a
    // handle-table growth reads g_slots without the Guard.
    void OnError(const char* message) override {
        if (!message) {
            return;
        }

        ErrorLog* log = &g_module_log;
        if (!message_is_player_facing(message) && g_error_slot >= 0
            && g_error_slot < g_slot_capacity && g_slots[g_error_slot].state) {
            log = &g_slots[g_error_slot].state->log;
        }
        record_error(*log, message);
    }

    // The composite. Nothing here transforms a vertex: every handle's geometry
    // is already on the device in the axes the shader wants. The WorldDraw the
    // header hands in is the screen-space path's and goes unused.
    void OnWorldDraw(ffxi::WorldDraw&) override {
        Guard guard;

        IDirect3DDevice8* device = FrameDevice();
        if (!device) {
            return;
        }

        // First, before anything is drawn, and ahead of the pipeline check on
        // purpose: a device that cannot make the shaders still must not leave
        // uploads queued for ever. Every buffer this library owns is made here,
        // this being the thread the game's device belongs to.
        drain_pending_uploads(device);

        if (!EnsureGpuPipeline(device)) {
            // A shader that cannot be made is a property of the device, and
            // the header has already reported it once. Nothing of ours draws.
            return;
        }

        D3DVIEWPORT8 viewport {};
        if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0
            || viewport.Height == 0) {
            return;
        }

        // Clip units per screen pixel, negative on y because clip space rises
        // where the screen falls. Nothing in the Lua API carries a pixel offset,
        // and the constant is still loaded with the value one would need.
        const float pixel_scale_x = 2.0f / static_cast<float>(viewport.Width);
        const float pixel_scale_y = -2.0f / static_cast<float>(viewport.Height);

        const D3DMATRIX& view_projection = FrameViewProjection();
        const float right3[3] = {FrameGroundRightX(), 0.0f, FrameGroundRightY()};
        const float up3[3] = {0.0f, -1.0f, 0.0f};
        const float* forward3 = FrameViewForward();

        // Culling stays off: nothing here asks a mesh for a winding order, and
        // double-sided models are legitimate.
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

        // Meshes first, with depth writing on, so a closed model hides its own
        // far faces and the flat overlays that follow sort against it.
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        for (int i = 0; i < g_slot_capacity; ++i) {
            const Slot& slot = g_slots[i];
            if (slot.active && slot.state) {
                draw_meshes(device, *slot.state, view_projection, right3, up3, forward3,
                    pixel_scale_x, pixel_scale_y);
            }
        }

        // Overlays never occlude each other, which is what overlays are for.
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        LoadGpuConstants(device, view_projection, right3, up3, forward3,
            pixel_scale_x, pixel_scale_y);
        for (int i = 0; i < g_slot_capacity; ++i) {
            const Slot& slot = g_slots[i];
            if (slot.active && slot.state) {
                draw_ranges(device, *slot.state);
            }
        }

        EndGpuDraws(device);
    }

    void draw_meshes(IDirect3DDevice8* device, const SlotState& state,
        const D3DMATRIX& view_projection, const float* right3, const float* up3,
        const float* forward3, float pixel_scale_x, float pixel_scale_y) {
        for (int i = 0; i < state.mesh_count; ++i) {
            const Mesh& mesh = *state.meshes[i];
            if (!mesh.used || !mesh.built || !mesh.visible || !mesh.buffer
                || mesh.count < 3) {
                continue;
            }

            D3DMATRIX world {};
            mesh_world_matrix(mesh, world);
            D3DMATRIX transform {};
            multiply_matrix(world, view_projection, transform);

            // The billboard basis turned back into this mesh's model space, so
            // a marker inside a rotated or scaled mesh still faces the camera.
            // The forward is not turned: no mesh can hold line geometry, so c8
            // goes unread by every mesh draw.
            float mesh_right[3];
            float mesh_up[3];
            to_model_space(mesh, right3, mesh_right);
            to_model_space(mesh, up3, mesh_up);

            LoadGpuConstants(device, transform, mesh_right, mesh_up, forward3,
                pixel_scale_x, pixel_scale_y);
            BindGpuTexture(device, mesh.texture);
            DrawGpuRange(device, mesh.buffer, 0,
                static_cast<UINT>(mesh.count / 3), ffxi::GpuShaderBillboard);
        }
    }

    void draw_ranges(IDirect3DDevice8* device, const SlotState& state) {
        if (!state.published.buffer) {
            return;
        }

        for (std::size_t i = 0; i < state.published.ranges.size(); ++i) {
            const Range& range = state.published.ranges[i];
            BindGpuTexture(device, range.texture);
            DrawGpuRange(device, state.published.buffer, range.start_vertex,
                range.primitive_count, range.shader);
        }
    }

    // The entity table and the player's index into it, resolved the way
    // Windower's Lua layer resolves them. The index pattern must keep its
    // +0x40DA8 offset bytes: several near-identical accessors read neighbouring
    // fields off the same global, and a shorter pattern matches all of them.

    void resolve_player_globals() {
        std::uintptr_t hit = scan_game(player_ctx_pattern_, player_ctx_mask_,
            sizeof(player_ctx_pattern_));
        if (hit) {
            std::uint32_t value = 0;
            if (read_memory(hit + 1, value)) {
                player_ctx_global_ = value;
            }
        }

        hit = scan_game(mob_array_pattern_, mob_array_mask_, sizeof(mob_array_pattern_));
        if (hit) {
            std::uint32_t value = 0;
            if (read_memory(hit + 9, value)) {
                mob_array_ = value;
            }
        }
    }

    bool player_position(float& x, float& y, float& z) const {
        if (!player_ctx_global_ || !mob_array_) {
            return false;
        }

        std::uint32_t context = 0;
        if (!read_memory(static_cast<std::uintptr_t>(player_ctx_global_), context) || !context) {
            return false;
        }

        std::uint16_t index = 0;
        if (!read_memory(static_cast<std::uintptr_t>(context) + player_index_offset_, index)
            || index == 0 || index > max_entity_index_) {
            return false;
        }

        std::uint32_t entity = 0;
        if (!read_memory(static_cast<std::uintptr_t>(mob_array_)
                + static_cast<std::uintptr_t>(index) * 4, entity) || !entity) {
            return false;
        }

        // The model is drawn from the render actor's root, which trails the
        // entity's own position while moving; anchoring to the entity puts
        // whatever is drawn ahead of the character.
        std::uint32_t actor = 0;
        if (read_memory(static_cast<std::uintptr_t>(entity) + 0xA0, actor) && actor) {
            if (read_position(static_cast<std::uintptr_t>(actor) + 0x678, x, y, z)) {
                return true;
            }
        }

        return read_position(static_cast<std::uintptr_t>(entity) + 0x04, x, y, z);
    }

    bool read_position(std::uintptr_t base, float& x, float& y, float& z) const {
        float px = 0.0f;
        float pz = 0.0f;
        float py = 0.0f;
        if (!read_memory(base + 0x0, px)
            || !read_memory(base + 0x4, pz)
            || !read_memory(base + 0x8, py)) {
            return false;
        }

        // A validity test on the pointer chase, not a limit: no FFXI zone is a
        // thousand yalms across, so a coordinate ten times that means the chase
        // landed on something that is not a position.
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)
            || std::fabs(px) > 10000.0f || std::fabs(py) > 10000.0f || std::fabs(pz) > 10000.0f) {
            return false;
        }

        x = px;
        y = py;
        z = pz;
        return true;
    }

    std::uintptr_t scan_game(const std::uint8_t* pattern, const char* mask, std::size_t length) {
        std::uintptr_t image_base = 0;
        std::size_t image_size = 0;
        if (!get_module_image("FFXiMain.dll", image_base, image_size)) {
            return 0;
        }
        return scan_module(image_base, image_size, pattern, mask, length);
    }

    // The game's own entity table: 0x900 entries, of which zero is not an
    // entity. Read out of the client, not chosen here.
    static constexpr std::uint16_t max_entity_index_ = 0x8FF;
    static constexpr std::uintptr_t player_index_offset_ = 0x40DA8;

    static constexpr std::uint8_t player_ctx_pattern_[21] = {
        0xA1, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x04, 0x66, 0x33,
        0xC0, 0xC3, 0x66, 0x8B, 0x80, 0xA8, 0x0D, 0x04, 0x00, 0xC3};
    static constexpr char player_ctx_mask_[22] = "x????xxxxxxxxxxxxxxxx";
    static constexpr std::uint8_t mob_array_pattern_[9] = {
        0x8B, 0x56, 0x0C, 0x8B, 0x04, 0x2A, 0x8B, 0x04, 0x85};
    static constexpr char mob_array_mask_[10] = "xxxxxxxxx";

    bool scan_attempted_ = false;
    std::uint32_t player_ctx_global_ = 0;
    std::uint32_t mob_array_ = 0;
};

LuaWorldDraw g_draw;

// Tessellates the staged commands into one vertex array and the ranges that
// draw it, on the Lua thread, touching nothing the render thread reads.
//
// Only adjacent commands may share a range: this geometry is alpha-blended and
// unsorted by depth, so draw order is the order the caller described it in, and
// gathering a scene's panels into one range would silently repaint it.
//
// False means the sizes could not be allocated, both outputs left empty, and
// the caller raises the Lua error.
bool build_draw_data(const CommandList& list, std::vector<ffxi::GpuVertex>& vertices,
    std::vector<Range>& ranges) {
    std::size_t total = 0;
    for (int i = 0; i < list.count; ++i) {
        const std::size_t added = command_vertices(list.items[i]);
        if (total > max_vertices_ - added) {
            return false;
        }
        total += added;
    }

    try {
        vertices.resize(total);
        ranges.clear();
        ranges.reserve(static_cast<std::size_t>(list.count));
    } catch (...) {
        return false;
    }

    std::size_t written = 0;
    for (int i = 0; i < list.count; ++i) {
        const Command& command = list.items[i];
        const std::size_t added = command_vertices(command);
        if (added == 0) {
            continue;
        }

        tessellate(command, vertices.data() + written);

        const ffxi::GpuShader shader = command_shader(command);
        const ffxi::TextureId texture = command_texture(command);
        if (!ranges.empty() && ranges.back().shader == shader
            && ranges.back().texture == texture) {
            ranges.back().primitive_count += static_cast<UINT>(added / 3);
        } else {
            Range range;
            range.shader = shader;
            range.texture = texture;
            range.start_vertex = static_cast<UINT>(written);
            range.primitive_count = static_cast<UINT>(added / 3);
            ranges.push_back(range);
        }

        written += added;
    }

    return true;
}

// Hands what build_draw_data made to the render thread. No device call: this
// runs on the Lua thread, so it only swaps the finished arrays into the
// handle's pending slot under the Guard.
//
// Cannot fail and cannot raise: a vector swap allocates nothing. A second
// commit before a frame overwrites the pending upload, and the arrays it
// replaces come back as this handle's scratch rather than being freed.
void stage_draw_data(SlotState& state) {
    Guard guard;
    state.scratch.swap(state.pending.vertices);
    state.scratch_ranges.swap(state.pending.ranges);
    state.pending.valid = true;
}

// Render thread only, with the Guard already held by the composite.
//
// Puts what the last commit staged on the device and swaps it in. Null on
// success, an engineering message otherwise. Every failure is checked before
// anything published is touched, so there is no half-published state, and a
// failure clears the pending mark rather than retrying it every frame.
const char* upload_draw_data(SlotState& state, IDirect3DDevice8* device) {
    if (!state.pending.valid) {
        return nullptr;
    }
    state.pending.valid = false;

    const std::size_t count = state.pending.vertices.size();
    if (count == 0) {
        // An empty commit draws nothing: the buffer is kept, grown and never
        // shrunk, and the ranges go -- which is what makes d:begin() with
        // nothing after it clear the handle.
        state.published.ranges.clear();
        return nullptr;
    }

    if (!state.published.buffer || count > static_cast<std::size_t>(state.published.capacity)) {
        IDirect3DVertexBuffer8* buffer =
            g_draw.CreateBuffer(device, state.pending.vertices.data(), static_cast<UINT>(count));
        if (!buffer) {
            return "draw: vertex buffer could not be created, the commit drew nothing";
        }

        IDirect3DVertexBuffer8* replaced = state.published.buffer;
        state.published.buffer = buffer;
        state.published.capacity = static_cast<UINT>(count);
        state.published.ranges.swap(state.pending.ranges);
        // Inside the same lock the composite walks under, so a walk already in
        // progress has finished with the old buffer -- and this is the one
        // thread that may call Release at all.
        if (replaced) {
            replaced->Release();
        }
        return nullptr;
    }

    // It fits: rewrite in place. Under the Guard the composite holds, so the
    // rewrite cannot land in the middle of a draw out of the same buffer.
    if (!g_draw.UpdateBuffer(state.published.buffer, state.pending.vertices.data(),
            static_cast<UINT>(count))) {
        return "draw: vertex buffer could not be written, the commit drew nothing";
    }
    state.published.ranges.swap(state.pending.ranges);
    return nullptr;
}

// Render thread only, with the Guard already held by the composite.
//
// The buffer a built mesh is waiting for. Its staged vertices are given back
// the moment they are on the device.
const char* upload_mesh(Mesh& mesh, IDirect3DDevice8* device) {
    if (!mesh.pending) {
        return nullptr;
    }
    mesh.pending = false;

    if (mesh.staging.empty()) {
        return nullptr;
    }

    IDirect3DVertexBuffer8* buffer = g_draw.CreateBuffer(device,
        mesh.staging.data(), static_cast<UINT>(mesh.staging.size()));
    if (!buffer) {
        std::vector<ffxi::GpuVertex>().swap(mesh.staging);
        return "draw: vertex buffer could not be created, the mesh drew nothing";
    }

    mesh.buffer = buffer;
    mesh.count = static_cast<int>(mesh.staging.size());
    std::vector<ffxi::GpuVertex>().swap(mesh.staging);
    return nullptr;
}

// Render thread only, with the Guard already held by the composite.
//
// Everything every handle staged since the last frame, put on the device before
// anything is drawn. Nothing here raises and nothing reaches Lua.
//
// A message lands in the image's log, never the committing handle's:
// g_error_slot belongs to the Lua thread and this thread may not touch it.
// Every handle's last_error consults both logs, so nothing is lost.
void drain_pending_uploads(IDirect3DDevice8* device) {
    for (int i = 0; i < g_slot_capacity; ++i) {
        const Slot& slot = g_slots[i];
        if (!slot.active || !slot.state) {
            continue;
        }

        SlotState& state = *slot.state;
        const char* failure = upload_draw_data(state, device);
        if (failure) {
            g_draw.Report(failure);
        }

        for (int m = 0; m < state.mesh_count; ++m) {
            Mesh& mesh = *state.meshes[m];
            if (!mesh.used) {
                continue;
            }
            failure = upload_mesh(mesh, device);
            if (failure) {
                g_draw.Report(failure);
            }
        }
    }
}

// The vertex buffer is queued, not released: every caller is on the Lua thread
// and a Release is a device call. See the thread rule in ffxi_world_draw.h.
void free_mesh_storage(Mesh& mesh) {
    if (mesh.buffer) {
        g_draw.DeferBuffer(mesh.buffer);
        mesh.buffer = nullptr;
    }
    mesh.count = 0;
    mesh.built = false;
    mesh.visible = false;
    mesh.pending = false;
    std::vector<ffxi::GpuVertex>().swap(mesh.staging);
}

// Everything a handle's state owns: the meshes, one allocation each, and the
// table of pointers to them; the textures it loaded; its command buffer and its
// published vertex buffer.
void free_slot_storage(SlotState& state) {
    free_command_storage(state.list);
    // Queued, like every other release on this thread.
    if (state.published.buffer) {
        g_draw.DeferBuffer(state.published.buffer);
        state.published.buffer = nullptr;
    }
    state.published.capacity = 0;
    state.pending.valid = false;

    for (int i = 0; i < state.mesh_count; ++i) {
        if (state.meshes[i]) {
            free_mesh_storage(*state.meshes[i]);
            delete state.meshes[i];
        }
    }
    delete[] state.meshes;
    state.meshes = nullptr;
    state.mesh_capacity = 0;
    state.mesh_count = 0;

    for (int i = 0; i < state.texture_count; ++i) {
        g_draw.ReleaseImage(state.textures[i]);
    }
    delete[] state.textures;
    state.textures = nullptr;
    state.texture_capacity = 0;
    state.texture_count = 0;
}

// Records a texture as this handle's, to be released when it closes. False only
// when that could not allocate; the caller raises the Lua error.
//
// No Guard: this list is the handle's own record and the render thread never
// walks it.
bool record_texture(SlotState& state, ffxi::TextureId id) {
    if (state.texture_count >= state.texture_capacity) {
        int capacity = 0;
        ffxi::TextureId* grown = doubled_table(state.textures, state.texture_capacity,
            state.texture_count, initial_handle_textures_, capacity);
        if (!grown) {
            return false;
        }

        delete[] state.textures;
        state.textures = grown;
        state.texture_capacity = capacity;
    }

    state.textures[state.texture_count++] = id;
    return true;
}

// A free mesh on this handle, reset and ready to be described. Its index, or -1
// when nothing could be allocated. Nothing here raises, because it takes the
// Guard; the caller raises the Lua error instead.
int claim_mesh(SlotState& state) {
    int index = -1;
    for (int i = 0; i < state.mesh_count; ++i) {
        if (!state.meshes[i]->used) {
            index = i;
            break;
        }
    }

    // Nothing is free, so the handle gets another mesh. Allocated outside the
    // Guard, so the render thread never waits on an allocation.
    Mesh* fresh = nullptr;
    Mesh** grown = nullptr;
    int grown_capacity = 0;
    if (index < 0) {
        fresh = new (std::nothrow) Mesh();
        if (!fresh) {
            return -1;
        }

        if (state.mesh_count >= state.mesh_capacity) {
            grown = doubled_table(state.meshes, state.mesh_capacity, state.mesh_count,
                initial_meshes_, grown_capacity);
            if (!grown) {
                delete fresh;
                return -1;
            }
        }

        index = state.mesh_count;
    }

    if (fresh) {
        // Under the Guard because mesh_count is how far the render thread walks
        // and the table may be about to be replaced. The new table is filled
        // before the swap, so the next walk sees the same meshes at the same
        // indices with one more entry behind them.
        Guard guard;
        if (grown) {
            Mesh** const replaced = state.meshes;
            state.meshes = grown;
            state.mesh_capacity = grown_capacity;
            delete[] replaced;
        }
        state.meshes[index] = fresh;
        state.mesh_count = index + 1;
    }

    // A mesh being reused is not drawn while `used` is false, so its fields are
    // reset without the lock and `used` is set last.
    Mesh& mesh = *state.meshes[index];
    free_mesh_storage(mesh);
    mesh.texture = 0;
    mesh.x = 0.0f;
    mesh.y = 0.0f;
    mesh.z = 0.0f;
    mesh.facing = 0.0f;
    mesh.scale = 1.0f;
    mesh.used = true;
    return index;
}

void close_slot(Slot& slot) {
    if (!slot.active) {
        return;
    }

    {
        Guard guard;

        SlotState* state = slot.state;
        char* name = slot.name;
        slot.active = false;
        slot.state = nullptr;
        slot.name = nullptr;
        ++slot.generation;

        if (state) {
            free_slot_storage(*state);
            delete state;
        }
        delete[] name;
    }

    if (g_open_handles > 0) {
        --g_open_handles;
    }

    // Closing the last handle drains: it waits for the render thread to leave
    // this image's handlers, so it must not run while the lock above is held.
    g_draw.Detach();
}

Slot* find_slot(int index, std::uint32_t generation) {
    if (index < 0 || index >= g_slot_capacity) {
        return nullptr;
    }

    Slot& slot = g_slots[index];
    if (!slot.active || slot.generation != generation || !slot.state) {
        return nullptr;
    }
    return &slot;
}

HandleRef* handle_ref(lua_State* L, int index) {
    return static_cast<HandleRef*>(luaL_checkudata(L, index, handle_type_));
}

Slot* checked_slot(lua_State* L, int index) {
    const HandleRef* ref = handle_ref(L, index);
    Slot* slot = find_slot(ref->slot, ref->generation);
    if (!slot) {
        luaL_error(L, "worlddraw: handle is closed");
    }
    return slot;
}

SlotState& checked_state(lua_State* L, int index) {
    return *checked_slot(L, index)->state;
}

// The mesh this reference names.
Mesh* checked_mesh(lua_State* L, int index) {
    const MeshRef* ref = static_cast<MeshRef*>(luaL_checkudata(L, index, mesh_type_));
    Slot* slot = find_slot(ref->slot, ref->slot_generation);
    if (!slot) {
        luaL_error(L, "worlddraw: handle is closed");
    }

    Mesh* mesh = nullptr;
    if (ref->mesh >= 0 && ref->mesh < slot->state->mesh_count) {
        Mesh& candidate = *slot->state->meshes[ref->mesh];
        if (candidate.used && candidate.generation == ref->mesh_generation) {
            mesh = &candidate;
        }
    }

    if (!mesh) {
        luaL_error(L, "worlddraw: mesh is freed");
    }
    return mesh;
}

float number(lua_State* L, int index) {
    return static_cast<float>(luaL_checknumber(L, index));
}

DWORD colour(lua_State* L, int index, DWORD fallback = 0xFFFFFFFF) {
    if (lua_isnoneornil(L, index)) {
        return fallback;
    }
    return static_cast<DWORD>(luaL_checknumber(L, index));
}

int l_begin(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    state.list.count = 0;
    return 0;
}

// Where the line is, for this function and for m:build():
//
//   A programmer error -- wrong arguments, a closed handle, a freed mesh,
//   building a mesh twice or with nothing staged -- raises. It is a bug in the
//   addon and the traceback is the point.
//
//   A state-of-the-world failure -- a correct call that cannot be honoured
//   right now -- reports and returns false. Raising here would mean an uncaught
//   error thirty times a second from a prerender handler, burying the one
//   player-facing line that explains why nothing is drawn.
//
// Nothing is sticky: the staged list is kept, published state is untouched, and
// the next commit publishes normally.
//
// This touches no device -- it runs on Windower's Lua thread -- so the geometry
// appears on the next frame, and a commit made before the game has a device is
// simply drawn once it has one. Memory is the only failure left.
int l_commit(lua_State* L) {
    Slot* const slot = checked_slot(L, 1);
    SlotState& state = *slot->state;

    // Named while the failure is being found, so the reason lands on this
    // handle, and cleared before anything can raise -- nothing between these
    // two lines does.
    g_error_slot = static_cast<int>(slot - g_slots);
    const char* failure = nullptr;
    if (!build_draw_data(state.list, state.scratch, state.scratch_ranges)) {
        failure = "draw: out of memory, the commit published nothing";
    } else {
        stage_draw_data(state);
    }
    if (failure) {
        g_draw.Report(failure);
    }
    g_error_slot = -1;

    if (failure) {
        lua_pushboolean(L, 0);
        return 1;
    }

    state.list.count = 0;
    lua_pushboolean(L, 1);
    return 1;
}

// The pending upload is cancelled as well as the published ranges: otherwise a
// clear landing between a commit and the frame that uploads it would be undone
// by the upload.
int l_clear(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    state.list.count = 0;

    Guard guard;
    state.published.ranges.clear();
    state.pending.valid = false;
    return 0;
}

int l_pillar(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    Command c;
    c.kind = Command::Pillar;
    for (int i = 0; i < 5; ++i) {
        c.v[i] = number(L, i + 2);
    }
    c.color = colour(L, 7);
    if (!add_command(state, c)) {
        return luaL_error(L, "worlddraw: out of memory");
    }
    return 0;
}

int l_ring(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    Command c;
    c.kind = Command::Ring;
    for (int i = 0; i < 5; ++i) {
        c.v[i] = number(L, i + 2);
    }
    c.color = colour(L, 7);
    if (!lua_isnoneornil(L, 8)) {
        c.segments = static_cast<int>(luaL_checkinteger(L, 8));
    }
    if (!add_command(state, c)) {
        return luaL_error(L, "worlddraw: out of memory");
    }
    return 0;
}

int l_line(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    Command c;
    c.kind = Command::Line;
    for (int i = 0; i < 7; ++i) {
        c.v[i] = number(L, i + 2);
    }
    c.color = colour(L, 9);
    if (!add_command(state, c)) {
        return luaL_error(L, "worlddraw: out of memory");
    }
    return 0;
}

int l_panel(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    Command c;
    c.kind = Command::Panel;
    for (int i = 0; i < 6; ++i) {
        c.v[i] = number(L, i + 2);
    }
    c.texture = static_cast<ffxi::TextureId>(luaL_checkinteger(L, 8));
    c.color = colour(L, 9);
    if (!add_command(state, c)) {
        return luaL_error(L, "worlddraw: out of memory");
    }
    return 0;
}

int l_sprite(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    Command c;
    c.kind = Command::Sprite;
    for (int i = 0; i < 5; ++i) {
        c.v[i] = number(L, i + 2);
    }
    c.texture = static_cast<ffxi::TextureId>(luaL_checkinteger(L, 7));
    c.color = colour(L, 8);
    if (!add_command(state, c)) {
        return luaL_error(L, "worlddraw: out of memory");
    }
    return 0;
}

int l_triangle(lua_State* L) {
    SlotState& state = checked_state(L, 1);
    Command c;
    c.kind = Command::Triangle;
    for (int i = 0; i < 9; ++i) {
        c.v[i] = number(L, i + 2);
    }
    c.color = colour(L, 11);
    if (!add_command(state, c)) {
        return luaL_error(L, "worlddraw: out of memory");
    }
    return 0;
}

int l_load_texture(lua_State* L) {
    Slot* slot = checked_slot(L, 1);
    const char* path = luaL_checkstring(L, 2);
    SlotState& state = *slot->state;

    // Nothing is cleared here. A failed load records and displaces nothing:
    // the serial says which message is newer.
    const LONG before = state.log.engineering_serial;
    g_error_slot = static_cast<int>(slot - g_slots);
    const ffxi::TextureId id = g_draw.LoadImage(path);
    if (id == 0 && state.log.engineering_serial == before) {
        // Two of LoadTexture's failure paths report nothing themselves: an
        // image with no width or height, and a repack that could not allocate.
        g_draw.Report("texture: the image could not be loaded");
    }
    g_error_slot = -1;

    if (id == 0) {
        lua_pushnil(L);
        return 1;
    }

    if (!record_texture(state, id)) {
        // Nothing would own this texture if it could not be recorded, and an
        // unowned texture lives until the client exits. It goes back.
        g_draw.ReleaseImage(id);
        return luaL_error(L, "worlddraw: out of memory");
    }

    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

// The semantics, which worlddraw.lua and the README both rely on:
//
//   d:last_error()          the most recent message of either kind, one value.
//                           A player-facing one is there until worlddraw.lua's
//                           poll takes it, and past that the buffer is empty
//                           and the player has already been told.
//   d:player_error()        the player-facing message, taken: it is answered
//                           once and gone, and the next call answers nil until
//                           the engine records another. worlddraw.lua's poll
//                           is the only intended caller -- anything else that
//                           asks takes the message out of the chat path.
//   d:engineering_error()   the most recent engineering string. Reading one
//                           takes nothing: an author may ask as often as they
//                           like and get the same answer.
//
// All of them consult both logs, this handle's and the image's, because a
// failure can belong to either. One serial counter stamps every record in
// either log, so "most recent" is a comparison.
struct ErrorPick {
    const char* text = nullptr;
    LONG serial = 0;
};

void consider(ErrorPick& best, const char* text, LONG serial) {
    if (serial == 0 || serial <= best.serial || text[0] == '\0') {
        return;
    }
    best.text = text;
    best.serial = serial;
}

// A closed handle answers nothing: whatever the image has to say, there is no
// handle left to say it to.
int push_error(lua_State* L, const HandleRef* ref, bool want_player, bool want_engineering) {
    const Slot* slot = find_slot(ref->slot, ref->generation);
    ErrorPick best;
    if (slot) {
        const ErrorLog& own = slot->state->log;
        if (want_player) {
            consider(best, own.player, own.player_serial);
            consider(best, g_module_log.player, g_module_log.player_serial);
        }
        if (want_engineering) {
            consider(best, own.engineering, own.engineering_serial);
            consider(best, g_module_log.engineering, g_module_log.engineering_serial);
        }
    }

    if (!best.text) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, best.text);
    return 1;
}

int l_last_error(lua_State* L) {
    return push_error(L, handle_ref(L, 1), true, true);
}

// Consumed by the read, which is why worlddraw.lua's poll is the only intended
// caller: the message it takes is the one nothing else will be shown. Taken
// off the image's log first, that being where a player-facing message is
// filed; a handle's own is asked in case a future one is filed there instead.
//
// No lock is held here and none may be: the render thread records without one.
// The atomicity is inside take_player_message.
int l_player_error(lua_State* L) {
    const HandleRef* const ref = handle_ref(L, 1);
    const Slot* const slot = find_slot(ref->slot, ref->generation);
    if (!slot) {
        lua_pushnil(L);
        return 1;
    }

    char message[max_error_];
    if (!take_player_message(g_module_log, message, sizeof(message))
        && !take_player_message(slot->state->log, message, sizeof(message))) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, message);
    return 1;
}

int l_engineering_error(lua_State* L) {
    return push_error(L, handle_ref(L, 1), false, true);
}

int l_player_draw_position(lua_State* L) {
    checked_slot(L, 1);

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!g_draw.PlayerDrawPosition(x, y, z)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, z);
    return 3;
}

int l_close(lua_State* L) {
    const HandleRef* ref = handle_ref(L, 1);
    Slot* slot = find_slot(ref->slot, ref->generation);
    if (slot) {
        close_slot(*slot);
    }
    return 0;
}

int l_mesh(lua_State* L) {
    Slot* slot = checked_slot(L, 1);
    SlotState& state = *slot->state;

    // The userdata is made before anything is allocated: lua_newuserdata can
    // raise a memory error and leave through this frame.
    MeshRef* ref = static_cast<MeshRef*>(lua_newuserdata(L, sizeof(MeshRef)));
    ref->slot = -1;
    ref->slot_generation = 0;
    ref->mesh = -1;
    ref->mesh_generation = 0;
    luaL_getmetatable(L, mesh_type_);
    lua_setmetatable(L, -2);

    const int index = claim_mesh(state);
    if (index < 0) {
        return luaL_error(L, "worlddraw: out of memory");
    }

    ref->slot = static_cast<int>(slot - g_slots);
    ref->slot_generation = slot->generation;
    ref->mesh = index;
    ref->mesh_generation = state.meshes[index]->generation;
    return 1;
}

int l_mesh_tri(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);
    if (mesh->built) {
        return luaL_error(L, "worlddraw: mesh is already built");
    }
    if (!reserve_staging(*mesh, 3)) {
        return luaL_error(L, "worlddraw: out of memory");
    }

    // Read the whole triangle before any of it is staged, so a bad argument
    // cannot leave a stray vertex behind.
    const DWORD color = colour(L, 17);
    float xyzuv[15];
    for (int i = 0; i < 15; ++i) {
        xyzuv[i] = number(L, i + 2);
    }

    stage_mesh_triangle(*mesh, xyzuv, color);
    return 0;
}

int l_mesh_mark(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);
    if (mesh->built) {
        return luaL_error(L, "worlddraw: mesh is already built");
    }
    if (!reserve_staging(*mesh, 6)) {
        return luaL_error(L, "worlddraw: out of memory");
    }

    const float x = number(L, 2);
    const float y = number(L, 3);
    const float z = number(L, 4);
    const float width = number(L, 5);
    const float height = number(L, 6);
    const DWORD color = colour(L, 7);

    stage_mesh_mark(*mesh, x, y, z, width, height, color);
    return 0;
}

int l_mesh_vertices(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);
    if (mesh->built) {
        return luaL_error(L, "worlddraw: mesh is already built");
    }

    std::size_t length = 0;
    const char* data = luaL_checklstring(L, 2, &length);
    const DWORD color = colour(L, 3);
    if (length % triangle_bytes_ != 0) {
        return luaL_error(L, "worlddraw: vertices needs 60 bytes per triangle");
    }

    const std::size_t added = length / vertex_bytes_;
    if (!reserve_staging(*mesh, added)) {
        return luaL_error(L, "worlddraw: out of memory");
    }

    // A Lua string owes nobody an alignment, so the floats are copied out
    // rather than read in place.
    static_assert(sizeof(float) * 5 == vertex_bytes_,
        "the packed upload is five little-endian float32s per vertex");
    for (std::size_t i = 0; i < added; i += 3) {
        float xyzuv[15];
        std::memcpy(xyzuv, data + i * vertex_bytes_, triangle_bytes_);
        stage_mesh_triangle(*mesh, xyzuv, color);
    }
    return 0;
}

int l_mesh_texture(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);
    const ffxi::TextureId texture = static_cast<ffxi::TextureId>(luaL_checkinteger(L, 2));

    Guard guard;
    mesh->texture = texture;
    return 0;
}

// The other half of the line drawn above l_commit: building twice and building
// nothing are programmer errors and raise. Nothing else here can fail.
//
// Building needs no device. The vertices are marked pending and the render
// thread makes the buffer at the top of the next composite, so a mesh described
// at addon load does not draw until then and needs nothing further from the
// addon. It still returns a boolean, with nothing left to return false for.
int l_mesh_build(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);
    if (mesh->built) {
        return luaL_error(L, "worlddraw: mesh is already built");
    }
    if (mesh->staging.empty()) {
        return luaL_error(L, "worlddraw: mesh has no vertices");
    }

    {
        // The count goes in with the mark, so the render thread never sees a
        // mesh that says it is built and not how big it is. Nothing in here
        // raises: the Guard may not be left through a longjmp.
        Guard guard;
        mesh->count = static_cast<int>(mesh->staging.size());
        mesh->pending = true;
        mesh->built = true;
    }

    lua_pushboolean(L, 1);
    return 1;
}

int l_mesh_at(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);
    if (!mesh->built) {
        return luaL_error(L, "worlddraw: mesh is not built");
    }

    const float x = number(L, 2);
    const float y = number(L, 3);
    const float z = number(L, 4);
    const float facing = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    const float scale = static_cast<float>(luaL_optnumber(L, 6, 1.0));

    Guard guard;
    mesh->x = x;
    mesh->y = y;
    mesh->z = z;
    mesh->facing = facing;
    mesh->scale = scale;
    mesh->visible = true;
    return 0;
}

int l_mesh_show(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);
    const bool visible = lua_toboolean(L, 2) != 0;

    Guard guard;
    mesh->visible = visible;
    return 0;
}

// Gives the mesh, its staging and its vertex buffer back, and marks the entry
// free for the next d:mesh(). Raises on a mesh that is already freed.
int l_mesh_free(lua_State* L) {
    Mesh* mesh = checked_mesh(L, 1);

    Guard guard;
    free_mesh_storage(*mesh);
    ++mesh->generation;
    mesh->used = false;
    return 0;
}

// The same, for the mesh nobody freed. m:free() is the documented way to say so
// early; this is what happens when nobody does. Three rules make it safe, and
// they are the handle's __gc's rules too:
//
// Generation-checked, so collection after an explicit free is a silent no-op
// and cannot double-free. Nothing raises under the Guard -- every check happens
// before it is taken, and luaL_error inside would longjmp out with the critical
// section held. Nothing is freed while the render thread could be walking it,
// the Guard being the lock the composite holds for its whole length.
int l_mesh_gc(lua_State* L) {
    const MeshRef* ref = static_cast<MeshRef*>(luaL_checkudata(L, 1, mesh_type_));
    const Slot* slot = find_slot(ref->slot, ref->slot_generation);
    if (!slot || ref->mesh < 0 || ref->mesh >= slot->state->mesh_count) {
        return 0;
    }

    Mesh& mesh = *slot->state->meshes[ref->mesh];
    if (!mesh.used || mesh.generation != ref->mesh_generation) {
        return 0;
    }

    Guard guard;
    free_mesh_storage(mesh);
    ++mesh.generation;
    mesh.used = false;
    return 0;
}

// Claims a free slot for `state` and `name`, doubling the handle table when
// every slot is taken. The index, or -1 when that could not allocate. Nothing
// here raises, because the Guard is held inside; the caller raises instead.
int claim_slot(SlotState* state, char* name) {
    int index = -1;
    for (int i = 0; i < g_slot_capacity; ++i) {
        if (!g_slots[i].active) {
            index = i;
            break;
        }
    }

    // Every handle is taken, so the table takes another. Allocated outside the
    // Guard, so the render thread never waits on an allocation.
    Slot* grown = nullptr;
    int grown_capacity = 0;
    if (index < 0) {
        grown = doubled_table(g_slots, g_slot_capacity, g_slot_capacity,
            initial_handles_, grown_capacity);
        if (!grown) {
            return -1;
        }

        index = g_slot_capacity;
    }

    // The render thread reads active and state together, so the claim and the
    // table swap are published under the same lock the composite walks under.
    // Every slot's generation counter is carried across, so a handle closed
    // before a growth is still closed after it.
    Guard guard;
    if (grown) {
        Slot* const replaced = g_slots;
        g_slots = grown;
        g_slot_capacity = grown_capacity;
        delete[] replaced;
    }

    Slot& slot = g_slots[index];
    slot.name = name;
    slot.state = state;
    slot.active = true;
    return index;
}

// The caller's own string, however long, in an allocation of its own. Null only
// when that allocation failed.
char* copy_name(const char* name) {
    const std::size_t length = std::strlen(name);
    if (length + 1 < length) {
        return nullptr;
    }

    char* copy = new (std::nothrow) char[length + 1];
    if (copy) {
        std::memcpy(copy, name, length + 1);
    }
    return copy;
}

int l_new(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    // The userdata is made before the slot is claimed, so a failure here cannot
    // leave a live slot with nothing referring to it.
    HandleRef* ref = static_cast<HandleRef*>(lua_newuserdata(L, sizeof(HandleRef)));
    ref->slot = -1;
    ref->generation = 0;
    luaL_getmetatable(L, handle_type_);
    lua_setmetatable(L, -2);

    SlotState* state = new (std::nothrow) SlotState();
    if (!state) {
        return luaL_error(L, "worlddraw: out of memory");
    }

    // The list takes its first buffer while the slot is still private and a Lua
    // error is still allowed: the Guard below may not raise one, and past it the
    // slot is visible to the render thread.
    if (!grow_commands(state->list)) {
        free_command_storage(state->list);
        delete state;
        return luaL_error(L, "worlddraw: out of memory");
    }

    char* stored_name = copy_name(name);
    if (!stored_name) {
        free_command_storage(state->list);
        delete state;
        return luaL_error(L, "worlddraw: out of memory");
    }

    const int index = claim_slot(state, stored_name);
    if (index < 0) {
        free_command_storage(state->list);
        delete state;
        delete[] stored_name;
        return luaL_error(L, "worlddraw: out of memory");
    }

    ref->slot = index;
    ref->generation = g_slots[index].generation;

    ++g_open_handles;
    g_draw.Attach();
    return 1;
}

int l_tick(lua_State*) {
#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
    // The other side of the measurement: the thread Windower runs Lua on.
    ffxi::thread_probe::record(ffxi::thread_probe::site_lua_tick);
#endif

    // PostRender reinstalls hooks, so with nothing to draw it would put them
    // back every frame after the last handle closed.
    if (g_open_handles > 0) {
        g_draw.Tick();
    }
    return 0;
}

int l_version(lua_State* L) {
    // Which hook daemon this client ended up with, which is not necessarily the
    // copy this addon shipped.
    const std::uint32_t abi = g_draw.ResidentDaemonAbi();
    if (abi != 0) {
        const char* const build = g_draw.ResidentDaemonBuild();
        // Sized so nothing is cut: the fixed text, a 32-bit number in full and
        // a build id, which the ABI record caps at WD_BUILD_MAX.
        char text[64 + WD_BUILD_MAX] {};
        std::snprintf(text, sizeof(text), "worlddraw 0.3, daemon abi %u build %s",
            static_cast<unsigned>(abi), build ? build : "unknown");
        lua_pushstring(L, text);
        return 1;
    }

    lua_pushstring(L, "worlddraw 0.3");
    return 1;
}

#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
// The whole measurement as one string. Only the diagnostic build has this, and
// worlddraw.lua checks for it rather than assuming it, so one worlddraw.lua
// drives both the kit and the probe image.
int l_thread_probe(lua_State* L) {
    // What thread_probe::report can produce and no more: a header, a line per
    // row of its table, and the verdict.
    char text[2048] {};
    const int used = std::snprintf(text, sizeof(text),
        "image: worlddraw 0.3 THREAD PROBE build, daemon abi %u\n",
        static_cast<unsigned>(g_draw.ResidentDaemonAbi()));
    const std::size_t at = used > 0 && static_cast<std::size_t>(used) < sizeof(text)
        ? static_cast<std::size_t>(used)
        : 0;
    ffxi::thread_probe::report(text + at, sizeof(text) - at);
    lua_pushstring(L, text);
    return 1;
}
#endif

const luaL_Reg kHandleMethods[] = {
    {"begin", l_begin},
    {"commit", l_commit},
    {"clear", l_clear},
    {"pillar", l_pillar},
    {"ring", l_ring},
    {"line", l_line},
    {"panel", l_panel},
    {"sprite", l_sprite},
    {"triangle", l_triangle},
    {"load_texture", l_load_texture},
    {"last_error", l_last_error},
    {"player_error", l_player_error},
    {"engineering_error", l_engineering_error},
    {"mesh", l_mesh},
    {"player_draw_position", l_player_draw_position},
    {"close", l_close},
    {nullptr, nullptr},
};

const luaL_Reg kMeshMethods[] = {
    {"tri", l_mesh_tri},
    {"mark", l_mesh_mark},
    {"vertices", l_mesh_vertices},
    {"texture", l_mesh_texture},
    {"build", l_mesh_build},
    {"at", l_mesh_at},
    {"show", l_mesh_show},
    {"free", l_mesh_free},
    {nullptr, nullptr},
};

const luaL_Reg kFunctions[] = {
    {"new", l_new},
    {"tick", l_tick},
    {"version", l_version},
#ifdef FFXI_WORLD_DRAW_THREAD_PROBE
    {"thread_probe", l_thread_probe},
#endif
    {nullptr, nullptr},
};

void register_type(lua_State* L, const char* name, const luaL_Reg* methods, lua_CFunction gc) {
    luaL_newmetatable(L, name);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    if (gc) {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
    }
    luaL_register(L, nullptr, methods);
    lua_pop(L, 1);
}

}  // namespace

extern "C" __declspec(dllexport) int luaopen_worlddraw(lua_State* L) {
    register_type(L, handle_type_, kHandleMethods, l_close);
    register_type(L, mesh_type_, kMeshMethods, l_mesh_gc);

    // Returned to the loader in worlddraw.lua rather than published as a
    // global: the name belongs to the Lua file that wraps this module.
    lua_newtable(L);
    luaL_register(L, nullptr, kFunctions);
    return 1;
}
