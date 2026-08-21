// worlddraw_abi.h - the contract between worlddraw_daemon.dll, the immortal
// hook multiplexer, and any engine image that registers handlers with it.
//
// Both sides compile this one file. Everything in it has plain C shape --
// explicit __stdcall, uint32_t rather than bool, 4-byte packing, no virtuals --
// because the two sides ship in different addons, are built months apart and
// must still agree byte for byte. Three rules make that hold:
//
//   - WdDaemonApi and WdHandlerSet are append-only. Fields are never reordered
//     or removed, and a reader checks `size` before touching anything past it.
//   - The version test is a range. An engine needs resident.abi_version >= its
//     own minimum, never equality: the resident daemon cannot be replaced while
//     the client runs, so equality would kill every addon not shipping the same
//     release.
//   - The vtable indices are generated from the SDK's d3d8.h by tools/gen_slots.
//     A wrong index patches the wrong method, and a passthrough of the wrong
//     arity corrupts the render thread's stack on every frame.
//
// Sizes and offsets are asserted below, so an engine and a daemon that disagree
// about this file fail to build rather than fail in a client.

#ifndef WORLDDRAW_ABI_H_
#define WORLDDRAW_ABI_H_

#include <windows.h>
#include <d3d8.h>
#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#define WD_STATIC_ASSERT(condition, message) static_assert(condition, message)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WD_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#else
#error "worlddraw_abi.h needs C++11 or C11: its size and offset asserts are the ABI"
#endif

// >>> BEGIN GENERATED wd_slots -- do not hand-edit these indices.
// tools/gen_slots derives them by counting the STDMETHOD entries of the
// IDirect3DDevice8 DECLARE_INTERFACE_ block in the SDK's d3d8.h, which is the
// vtable order:
//   tools/gen_slots/build.sh generate   rewrites this span
//   tools/gen_slots/build.sh            proves it still matches the SDK
enum {
    WD_SLOT_RESET = 14,                     /* IDirect3DDevice8::Reset */
    WD_SLOT_SET_RENDER_TARGET = 31,         /* IDirect3DDevice8::SetRenderTarget */
    WD_SLOT_DRAW_PRIMITIVE = 70,            /* IDirect3DDevice8::DrawPrimitive */
    WD_SLOT_DRAW_INDEXED_PRIMITIVE = 71,    /* IDirect3DDevice8::DrawIndexedPrimitive */
    WD_SLOT_DRAW_PRIMITIVE_UP = 72,         /* IDirect3DDevice8::DrawPrimitiveUP */
    WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP = 73, /* IDirect3DDevice8::DrawIndexedPrimitiveUP */
    WD_SLOT_COUNT = 6,
    WD_DEVICE_VTABLE_SLOTS = 97
};
// <<< END GENERATED wd_slots

// The order of WdDaemonStats::forwards, and the order the daemon patches and
// rolls back. Not derived from the SDK -- it is our own layout -- so it lives
// outside the generated span.
enum {
    WD_INDEX_RESET = 0,
    WD_INDEX_SET_RENDER_TARGET = 1,
    WD_INDEX_DRAW_PRIMITIVE = 2,
    WD_INDEX_DRAW_INDEXED_PRIMITIVE = 3,
    WD_INDEX_DRAW_PRIMITIVE_UP = 4,
    WD_INDEX_DRAW_INDEXED_PRIMITIVE_UP = 5
};

// Bumped only when a field is appended. An engine asks for the oldest daemon it
// can work with; a newer resident daemon satisfies it silently.
#define WD_DAEMON_ABI 1u

// Bumped whenever the daemon binary changes, ABI or not. Two addons can ship
// different daemon builds at one ABI and whoever pins first holds the process
// for the session, so without this the resident build cannot be named.
#define WD_DAEMON_BUILD "1.3.0"

// The wire size of WdDaemonRecord::winner_build. Frozen: it is a fixed field in
// a shared structure, so it can never change without changing the record.
#define WD_BUILD_MAX 64

// Written into the shared record last, after a barrier, so a reader that sees
// it sees a complete record. Any other value -- zero included -- means nobody
// has published, which is what makes a mid-election crash recoverable.
#define WD_MAGIC 0x57444B31u

// There is deliberately no cap here on registered handler sets or on retained
// device vtables: the daemon pins itself for the life of the client, so a limit
// it shipped with could only be raised by making every player restart. Both
// grow on demand and memory is the only bound.

// The section the record lives in is always created and opened at this fixed
// size, never at sizeof(WdDaemonRecord): Windows documents ERROR_ACCESS_DENIED
// for opening an existing section with a larger requested size, and a fixed
// page means two daemon builds can never disagree about it.
#define WD_MAPPING_BYTES 4096

// The daemon's one export, and the names of the two objects the election runs
// on. Local\ is session-scoped -- a mapping made in one client was read by
// another -- so both names carry GetCurrentProcessId().
#define WD_DAEMON_ACQUIRE_NAME "wd_daemon_acquire"
#define WD_MAPPING_NAME_FORMAT "Local\\worlddraw_daemon_v1_%08X"
#define WD_MUTEX_NAME_FORMAT "Local\\worlddraw_daemon_lock_v1_%08X"

// Not a policy limit: the only strings that go in one of these buffers are the
// two fixed formats above with an eight-digit pid, and the asserts at the foot
// of this file prove both fit.
#define WD_NAME_MAX 64

#pragma pack(push, 4)

// Lives in the engine image and is owned by it. The daemon holds the pointer
// while registered, copies nothing out of it, and never touches it again once
// unregister_set has returned.
typedef struct WdHandlerSet {
    uint32_t abi_version;
    uint32_t size;
    void* user;
    void (__stdcall* pre_reset)(void*, IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
    void (__stdcall* post_reset)(void*, IDirect3DDevice8*, HRESULT);
    void (__stdcall* pre_set_render_target)(void*, IDirect3DDevice8*, IDirect3DSurface8*, IDirect3DSurface8*);
    void (__stdcall* pre_draw)(void*, IDirect3DDevice8*);
} WdHandlerSet;

// The caller sets `size` to its own sizeof before the call; the daemon writes
// back no more than that and reports what it wrote in the same field.
typedef struct WdDaemonStats {
    uint32_t size;
    uint32_t sets_registered;
    uint32_t sets_live;
    uint32_t forwards[WD_SLOT_COUNT];
    uint32_t stomps_detected;
    uint32_t vtables_dropped;
    uint32_t install_refused_trampoline;
} WdDaemonStats;

// Append-only, never reordered. ensure_hooks returns 1 when all six slots are
// the daemon's and 0 when it refused, having patched nothing. register_set
// returns a 1-based index; 0 is a refusal and never a success.
typedef struct WdDaemonApi {
    uint32_t abi_version;
    uint32_t size;
    uint32_t (__stdcall* ensure_hooks)(IDirect3DDevice8*);
    uint32_t (__stdcall* register_set)(const WdHandlerSet*);
    void (__stdcall* unregister_set)(const WdHandlerSet*);
    uint32_t (__stdcall* check_slots)(void);
    void (__stdcall* stats)(WdDaemonStats*);
    const char* (__stdcall* build_id)(void);   /* the resident daemon's build */
} WdDaemonApi;

// What the election publishes into the pid-scoped mapping. The api travels by
// value, so a reader never dereferences a pointer into another image.
//
// The API comes last and must stay last: appending a function pointer to
// WdDaemonApi then extends this record's tail and moves nothing an older reader
// depends on. With the api in the middle, growing it would shift winner_path --
// which the cross-generation ABI-refusal message names -- under every older
// reader. The prefix through winner_build is frozen; readers accept
// record_size >= their own and api.size >= the prefix they know.
typedef struct WdDaemonRecord {
    uint32_t magic;
    uint32_t record_size;
    uint32_t abi_version;
    char winner_path[MAX_PATH];
    char winner_build[WD_BUILD_MAX];
    WdDaemonApi api;
} WdDaemonRecord;

#pragma pack(pop)

// Called with LoadLibraryW's module, never from DllMain: the election takes a
// lock and may LoadLibrary, which under the loader lock deadlocks. Returns NULL
// when the resident daemon is older than min_abi, or when this image could not
// pin itself.
typedef const WdDaemonApi* (__stdcall* WdDaemonAcquire)(uint32_t min_abi);

WD_STATIC_ASSERT(sizeof(void*) == 4, "worlddraw's ABI is 32-bit; the sizes below assume it");

WD_STATIC_ASSERT(sizeof(WdHandlerSet) == 28, "WdHandlerSet size is ABI");
WD_STATIC_ASSERT(offsetof(WdHandlerSet, abi_version) == 0, "WdHandlerSet layout is ABI");
WD_STATIC_ASSERT(offsetof(WdHandlerSet, size) == 4, "WdHandlerSet layout is ABI");
WD_STATIC_ASSERT(offsetof(WdHandlerSet, user) == 8, "WdHandlerSet layout is ABI");
WD_STATIC_ASSERT(offsetof(WdHandlerSet, pre_reset) == 12, "WdHandlerSet layout is ABI");
WD_STATIC_ASSERT(offsetof(WdHandlerSet, post_reset) == 16, "WdHandlerSet layout is ABI");
WD_STATIC_ASSERT(offsetof(WdHandlerSet, pre_set_render_target) == 20, "WdHandlerSet layout is ABI");
WD_STATIC_ASSERT(offsetof(WdHandlerSet, pre_draw) == 24, "WdHandlerSet layout is ABI");

WD_STATIC_ASSERT(sizeof(WdDaemonStats) == 48, "WdDaemonStats size is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonStats, size) == 0, "WdDaemonStats layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonStats, sets_registered) == 4, "WdDaemonStats layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonStats, sets_live) == 8, "WdDaemonStats layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonStats, forwards) == 12, "WdDaemonStats layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonStats, stomps_detected) == 36, "WdDaemonStats layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonStats, vtables_dropped) == 40, "WdDaemonStats layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonStats, install_refused_trampoline) == 44, "WdDaemonStats layout is ABI");

WD_STATIC_ASSERT(sizeof(WdDaemonApi) == 32, "WdDaemonApi size is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, abi_version) == 0, "WdDaemonApi layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, size) == 4, "WdDaemonApi layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, ensure_hooks) == 8, "WdDaemonApi layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, register_set) == 12, "WdDaemonApi layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, unregister_set) == 16, "WdDaemonApi layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, check_slots) == 20, "WdDaemonApi layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, stats) == 24, "WdDaemonApi layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonApi, build_id) == 28, "WdDaemonApi layout is ABI");

WD_STATIC_ASSERT(sizeof(WdDaemonRecord) == 368, "WdDaemonRecord size is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonRecord, magic) == 0, "WdDaemonRecord layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonRecord, record_size) == 4, "WdDaemonRecord layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonRecord, abi_version) == 8, "WdDaemonRecord layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonRecord, winner_path) == 12, "WdDaemonRecord layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonRecord, winner_build) == 272, "WdDaemonRecord layout is ABI");
WD_STATIC_ASSERT(offsetof(WdDaemonRecord, api) == 336, "the api comes last so appending to it moves nothing");
WD_STATIC_ASSERT(sizeof(WD_DAEMON_BUILD) <= WD_BUILD_MAX, "the build string must fit the record");
WD_STATIC_ASSERT(MAX_PATH == 260, "WdDaemonRecord::winner_path is a fixed 260 bytes on the wire");

WD_STATIC_ASSERT(WD_SLOT_COUNT == 6, "six slots, by name, generated from the SDK");
WD_STATIC_ASSERT(sizeof(WdDaemonRecord) <= WD_MAPPING_BYTES, "the record must fit the fixed section");

// %08X becomes eight characters; sizeof() already counts the four it replaces
// and the terminator, so this is an upper bound on the formatted name.
WD_STATIC_ASSERT(sizeof(WD_MAPPING_NAME_FORMAT) + 8 <= WD_NAME_MAX,
    "the mapping name must fit WD_NAME_MAX for every pid");
WD_STATIC_ASSERT(sizeof(WD_MUTEX_NAME_FORMAT) + 8 <= WD_NAME_MAX,
    "the mutex name must fit WD_NAME_MAX for every pid");

#endif  // WORLDDRAW_ABI_H_
