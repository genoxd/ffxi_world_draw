// worlddraw_daemon - the immortal hook multiplexer.
//
// This image never unloads. Foreign tools chain through whatever they find in
// the device vtable, so an image whose address sits in a slot they saved can
// never be allowed to unmap. Everything that changes lives in the engine image,
// which registers a handler set here and stays swappable.
//
// Because it can never be updated without the player restarting the client,
// four rules bind every change to this file:
//
//   - the ABI it publishes is a range and append-only (see worlddraw_abi.h).
//   - it copies nothing out of a handler set and touches none after unregister
//     returns, so it survives every engine it serves being unloaded.
//   - It never re-chains over a foreign hook: once one cleanly departs, "it
//     left" and "it is still installed beneath us" are the same bytes, so no
//     recovery can be correct. Stomps are counted and reported.
//   - It calls no method of the device, on any thread. Its entry points run on
//     whatever thread the engine is on, and FFXI's device is created without
//     D3DCREATE_MULTITHREADED, so the only device calls it takes part in are
//     the forwards -- made from the thread already inside the method.
//   - it runs no code at process exit: no namespace-scope object here has a
//     non-trivial destructor and DllMain does nothing.

#define WIN32_LEAN_AND_MEAN
#include "worlddraw_abi.h"

#include <cstdio>
#include <cstring>

namespace {

typedef void (*DeviceMethod)();
typedef HRESULT (__stdcall* ResetMethod)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (__stdcall* SetRenderTargetMethod)(IDirect3DDevice8*, IDirect3DSurface8*, IDirect3DSurface8*);
typedef HRESULT (__stdcall* DrawPrimitiveMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (__stdcall* DrawIndexedPrimitiveMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
typedef HRESULT (__stdcall* DrawPrimitiveUPMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (__stdcall* DrawIndexedPrimitiveUPMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
    const void*, D3DFORMAT, const void*, UINT);

const int slot_numbers_[WD_SLOT_COUNT] = {
    WD_SLOT_RESET,
    WD_SLOT_SET_RENDER_TARGET,
    WD_SLOT_DRAW_PRIMITIVE,
    WD_SLOT_DRAW_INDEXED_PRIMITIVE,
    WD_SLOT_DRAW_PRIMITIVE_UP,
    WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP,
};

const DWORD election_wait_ms_ = 5000;

// One registered engine. The pointer is the engine's; the count is ours.
struct HandlerSlot {
    const WdHandlerSet* volatile set;
    volatile LONG inflight;
};

// One patched device vtable. `vtable` is the published field, written last
// behind a barrier, so a thunk that reads a non-zero one sees the originals
// beside it.
//
// A record is identified by its vtable and deliberately holds no device
// pointer: this image keeps no reference on a device, and the only use for one
// would be to call a method through it.
struct VtableRecord {
    DeviceMethod* volatile vtable;
    DeviceMethod originals[WD_SLOT_COUNT];
    uint32_t complete;
};

// Neither registered sets nor retained vtables are capped: this image pins
// itself for the life of the client, so a limit it shipped with could only be
// raised by making every player restart.
//
// The render thread walks both tables with no lock, using increment-before-read
// on a per-slot counter, so growth must never move anything a reader may be
// mid-increment on. Four rules keep that true:
//
//   - a HandlerSlot and a VtableRecord are each one allocation, are never freed
//     and never move. Unregistering clears the slot's `set` and a failed
//     liveness check clears the record's `vtable`; the object stays put and is
//     reused in place, so no address a reader holds becomes something else's.
//   - the table is an immutable snapshot published as one pointer. Growing
//     allocates a new array, copies, appends, barriers, and only then publishes.
//   - The old table is never freed: a reader may still be walking it and there
//     is no way to learn that it is not. The leak is bounded by the number of
//     doublings in a process lifetime.
//   - a reader takes the table pointer once into a local and walks that
//     snapshot; a registration landing mid-walk is simply not seen by that call.
//
// Every allocation happens in register_set or ensure_hooks, on the Lua thread.
// No thunk and no dispatch loop allocates. A failed allocation is a refusal and
// publishes nothing, half or otherwise.

struct SlotTable {
    HandlerSlot** entries;
    LONG count;
};

struct VtableTable {
    VtableRecord** entries;
    LONG count;
};

// Plain objects with no constructor and no destructor, zero-initialised into
// .bss by the loader: nothing here may run at image load or at process exit.
SlotTable* volatile g_slot_table;
volatile LONG g_high_water;
VtableTable* volatile g_vtable_table;

// The first table is this big and every later one is twice its predecessor.
const LONG initial_capacity_ = 4;

CRITICAL_SECTION g_lock;
volatile LONG g_lock_state;

// g_api_valid is set last by publish() and by adopt() and is the only record of
// whether an election succeeded. There must be no second flag for "an election
// was attempted": latching on the attempt makes every transient failure
// permanent in an image that can never be reloaded.
WdDaemonApi g_public_api;
uint32_t g_resident_abi;
uint32_t g_api_valid;

uint32_t g_sets_registered;
uint32_t g_sets_live;
// Advisory, and deliberately not interlocked: the render thread is not paying
// for an atomic on every draw call to make a support counter exact.
uint32_t g_forwards[WD_SLOT_COUNT];
uint32_t g_stomps_detected;
uint32_t g_vtables_dropped;
uint32_t g_install_refused_trampoline;

char g_winner_path[MAX_PATH];

// An address certain to be inside this image, for the FROM_ADDRESS lookups. A
// data anchor, not a function's address: casting a function pointer to a data
// pointer is not portable C++.
const char module_anchor_ = 'w';

// Created on first use and never deleted: deleting it would need code at
// process exit, and there is none here. The one-time initialisation is written
// out rather than left to a function-local static, so nothing depends on how
// the compiler implements static-init guards.
void lock() {
    for (;;) {
        const LONG previous = InterlockedCompareExchange(&g_lock_state, 1, 0);
        if (previous == 2) {
            break;
        }
        if (previous == 0) {
            InitializeCriticalSection(&g_lock);
            InterlockedExchange(&g_lock_state, 2);
            break;
        }
        SwitchToThread();
    }
    EnterCriticalSection(&g_lock);
}

void unlock() {
    LeaveCriticalSection(&g_lock);
}

// The two growers below run with the lock held and only ever on the Lua thread.

// The process heap rather than the CRT: this image takes nothing from the C++
// runtime that could register work to run at process exit. Zeroed, so a fresh
// object is a free one without anybody writing its fields.
void* allocate(size_t bytes) {
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
}

// Only for something this thread allocated and has not published: a published
// table or object is never freed. This undoes a growth that failed part way.
void release_unpublished(void* block) {
    if (block) {
        HeapFree(GetProcessHeap(), 0, block);
    }
}

LONG first_free_slot(const SlotTable* table) {
    for (LONG i = 0; table && i < table->count; ++i) {
        if (!table->entries[i]->set) {
            return i;
        }
    }
    return -1;
}

LONG first_free_vtable(const VtableTable* table) {
    for (LONG i = 0; table && i < table->count; ++i) {
        if (!table->entries[i]->vtable) {
            return i;
        }
    }
    return -1;
}

// The header and its pointer array are one allocation, so the snapshot a reader
// takes is a single object.
bool grow_slots() {
    const SlotTable* const current = g_slot_table;
    const LONG count = current ? current->count : 0;
    const LONG capacity = count ? count * 2 : initial_capacity_;

    unsigned char* const block = static_cast<unsigned char*>(
        allocate(sizeof(SlotTable) + sizeof(HandlerSlot*) * static_cast<size_t>(capacity)));
    if (!block) {
        return false;
    }

    SlotTable* const table = reinterpret_cast<SlotTable*>(block);
    HandlerSlot** const entries = reinterpret_cast<HandlerSlot**>(block + sizeof(SlotTable));
    for (LONG i = 0; i < count; ++i) {
        entries[i] = current->entries[i];
    }

    for (LONG i = count; i < capacity; ++i) {
        HandlerSlot* const slot = static_cast<HandlerSlot*>(allocate(sizeof(HandlerSlot)));
        if (!slot) {
            for (LONG j = count; j < i; ++j) {
                release_unpublished(entries[j]);
            }
            release_unpublished(block);
            return false;
        }
        entries[i] = slot;
    }

    table->entries = entries;
    table->count = capacity;

    // Everything the new table points at is written before the barrier; the
    // pointer that makes it reachable is written after it.
    MemoryBarrier();
    g_slot_table = table;
    return true;
}

bool grow_vtables() {
    const VtableTable* const current = g_vtable_table;
    const LONG count = current ? current->count : 0;
    const LONG capacity = count ? count * 2 : initial_capacity_;

    unsigned char* const block = static_cast<unsigned char*>(
        allocate(sizeof(VtableTable) + sizeof(VtableRecord*) * static_cast<size_t>(capacity)));
    if (!block) {
        return false;
    }

    VtableTable* const table = reinterpret_cast<VtableTable*>(block);
    VtableRecord** const entries = reinterpret_cast<VtableRecord**>(block + sizeof(VtableTable));
    for (LONG i = 0; i < count; ++i) {
        entries[i] = current->entries[i];
    }

    for (LONG i = count; i < capacity; ++i) {
        VtableRecord* const record = static_cast<VtableRecord*>(allocate(sizeof(VtableRecord)));
        if (!record) {
            for (LONG j = count; j < i; ++j) {
                release_unpublished(entries[j]);
            }
            release_unpublished(block);
            return false;
        }
        entries[i] = record;
    }

    table->entries = entries;
    table->count = capacity;

    MemoryBarrier();
    g_vtable_table = table;
    return true;
}

// Guarded reads of memory we do not own, the same tests ffxi_world_draw.h
// installs with. Nothing here writes game memory except the vtable slot itself,
// under VirtualProtect.

bool is_readable_page(DWORD protect) {
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

bool is_readable_span(uintptr_t address, size_t size) {
    if (address == 0 || size == 0) {
        return false;
    }

    const uintptr_t end = address + size;
    if (end < address) {
        return false;
    }

    uintptr_t cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi;
        std::memset(&mbi, 0, sizeof(mbi));
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) {
            return false;
        }

        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor || mbi.State != MEM_COMMIT || !is_readable_page(mbi.Protect)) {
            return false;
        }

        cursor = region_end;
    }

    return true;
}

bool is_executable_code(uintptr_t address) {
    if (address == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi;
    std::memset(&mbi, 0, sizeof(mbi));
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) {
        return false;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }

    const DWORD base = mbi.Protect & 0xFF;
    return base == PAGE_EXECUTE ||
        base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE ||
        base == PAGE_EXECUTE_WRITECOPY;
}

// Function addresses reach the module lookups as integers: a cast straight from
// a function pointer to a data pointer is not portable C++.
LPCSTR address_of(DeviceMethod method) {
    return reinterpret_cast<LPCSTR>(reinterpret_cast<uintptr_t>(method));
}

// The value we chain atop must belong to a module we can pin, or our saved
// original could unmap under us. An address in no module is an allocated
// trampoline -- MinHook, Detours -- and there is nothing to pin.
bool pin_module_containing(DeviceMethod method) {
    HMODULE module = NULL;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        address_of(method), &module) != FALSE;
}

bool patch_vtable_slot(DeviceMethod* vtable, int slot, DeviceMethod replacement, DeviceMethod& previous) {
    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(DeviceMethod), PAGE_READWRITE, &old_protect)) {
        return false;
    }

    previous = vtable[slot];
    MemoryBarrier();
    vtable[slot] = replacement;

    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(DeviceMethod), old_protect, &ignored);
    return true;
}

DeviceMethod thunk_for(int index);

// A thunk knows which vtable it was called through by reading the object's own
// vtable pointer. Two devices sharing one vtable share one record; devices with
// their own -- a wrapper hands out one per device -- get their own originals.
DeviceMethod original_for(IDirect3DDevice8* device, int index) {
    if (!device) {
        return 0;
    }

    // Taken once. A vtable retained after this read belongs to a device this
    // call is not being made through, so missing it is not a miss.
    const VtableTable* const table = g_vtable_table;
    if (!table) {
        return 0;
    }

    DeviceMethod* const vtable = *reinterpret_cast<DeviceMethod* const volatile*>(device);
    // Zero is what a dropped record carries, so without this a device whose
    // vtable pointer is zero would match one and be handed the originals of a
    // vtable that no longer exists.
    if (!vtable) {
        return 0;
    }

    for (LONG i = 0; i < table->count; ++i) {
        VtableRecord* const record = table->entries[i];
        if (record->vtable == vtable) {
            return record->originals[index];
        }
    }

    return 0;
}

// What a dispatch loop walks: the table pointer taken once, and how far into it
// to go. The slot objects never move, so an increment on one is an increment on
// the object the drain is watching whatever table was published meanwhile.
// g_high_water is clamped by the snapshot's own count, so a high-water raised
// for a table this call never saw cannot walk off the end of the one it did.
struct Walk {
    const SlotTable* table;
    LONG count;
};

Walk begin_walk() {
    Walk walk;
    walk.table = g_slot_table;
    walk.count = walk.table ? g_high_water : 0;
    if (walk.table && walk.count > walk.table->count) {
        walk.count = walk.table->count;
    }
    return walk;
}

// The increment comes before the pointer is read, and that ordering is the
// whole safety argument: unregister_set clears the pointer and then waits for
// this count to reach zero, so either the read sees the pointer and the drain
// is already obliged to wait for us, or it sees NULL. Nothing is ever copied
// out of a set -- the pointer is re-read every time.
void dispatch_pre_draw(IDirect3DDevice8* device) {
    const Walk walk = begin_walk();
    for (LONG i = 0; i < walk.count; ++i) {
        HandlerSlot* const slot = walk.table->entries[i];
        InterlockedIncrement(&slot->inflight);
        const WdHandlerSet* const set = slot->set;
        if (set && set->pre_draw) {
            set->pre_draw(set->user, device);
        }
        InterlockedDecrement(&slot->inflight);
    }
}

void dispatch_pre_set_render_target(IDirect3DDevice8* device, IDirect3DSurface8* render_target,
    IDirect3DSurface8* depth_stencil) {
    const Walk walk = begin_walk();
    for (LONG i = 0; i < walk.count; ++i) {
        HandlerSlot* const slot = walk.table->entries[i];
        InterlockedIncrement(&slot->inflight);
        const WdHandlerSet* const set = slot->set;
        if (set && set->pre_set_render_target) {
            set->pre_set_render_target(set->user, device, render_target, depth_stencil);
        }
        InterlockedDecrement(&slot->inflight);
    }
}

void dispatch_pre_reset(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* parameters) {
    const Walk walk = begin_walk();
    for (LONG i = 0; i < walk.count; ++i) {
        HandlerSlot* const slot = walk.table->entries[i];
        InterlockedIncrement(&slot->inflight);
        const WdHandlerSet* const set = slot->set;
        if (set && set->pre_reset) {
            set->pre_reset(set->user, device, parameters);
        }
        InterlockedDecrement(&slot->inflight);
    }
}

void dispatch_post_reset(IDirect3DDevice8* device, HRESULT result) {
    const Walk walk = begin_walk();
    for (LONG i = 0; i < walk.count; ++i) {
        HandlerSlot* const slot = walk.table->entries[i];
        InterlockedIncrement(&slot->inflight);
        const WdHandlerSet* const set = slot->set;
        if (set && set->post_reset) {
            set->post_reset(set->user, device, result);
        }
        InterlockedDecrement(&slot->inflight);
    }
}

HRESULT __stdcall thunk_reset(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* parameters) {
    // Two independent loops with the real Reset between them: holding the set
    // pointers across that call would mean dereferencing an image that may have
    // gone meanwhile. A set registering in between gets a post_reset with no
    // pre_reset, which only re-arms it.
    dispatch_pre_reset(device, parameters);

    const ResetMethod original = reinterpret_cast<ResetMethod>(original_for(device, WD_INDEX_RESET));
    if (!original) {
        return D3DERR_INVALIDCALL;
    }

    ++g_forwards[WD_INDEX_RESET];
    const HRESULT result = original(device, parameters);

    dispatch_post_reset(device, result);
    return result;
}

HRESULT __stdcall thunk_set_render_target(IDirect3DDevice8* device, IDirect3DSurface8* render_target,
    IDirect3DSurface8* depth_stencil) {
    dispatch_pre_set_render_target(device, render_target, depth_stencil);

    const SetRenderTargetMethod original =
        reinterpret_cast<SetRenderTargetMethod>(original_for(device, WD_INDEX_SET_RENDER_TARGET));
    if (!original) {
        return D3DERR_INVALIDCALL;
    }

    ++g_forwards[WD_INDEX_SET_RENDER_TARGET];
    return original(device, render_target, depth_stencil);
}

HRESULT __stdcall thunk_draw_primitive(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive_type,
    UINT start_vertex, UINT primitive_count) {
    dispatch_pre_draw(device);

    const DrawPrimitiveMethod original =
        reinterpret_cast<DrawPrimitiveMethod>(original_for(device, WD_INDEX_DRAW_PRIMITIVE));
    if (!original) {
        return D3DERR_INVALIDCALL;
    }

    ++g_forwards[WD_INDEX_DRAW_PRIMITIVE];
    return original(device, primitive_type, start_vertex, primitive_count);
}

HRESULT __stdcall thunk_draw_indexed_primitive(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive_type,
    UINT minimum_index, UINT vertex_count, UINT start_index, UINT primitive_count) {
    dispatch_pre_draw(device);

    const DrawIndexedPrimitiveMethod original =
        reinterpret_cast<DrawIndexedPrimitiveMethod>(original_for(device, WD_INDEX_DRAW_INDEXED_PRIMITIVE));
    if (!original) {
        return D3DERR_INVALIDCALL;
    }

    ++g_forwards[WD_INDEX_DRAW_INDEXED_PRIMITIVE];
    return original(device, primitive_type, minimum_index, vertex_count, start_index, primitive_count);
}

HRESULT __stdcall thunk_draw_primitive_up(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive_type,
    UINT primitive_count, const void* vertex_data, UINT stride) {
    dispatch_pre_draw(device);

    const DrawPrimitiveUPMethod original =
        reinterpret_cast<DrawPrimitiveUPMethod>(original_for(device, WD_INDEX_DRAW_PRIMITIVE_UP));
    if (!original) {
        return D3DERR_INVALIDCALL;
    }

    ++g_forwards[WD_INDEX_DRAW_PRIMITIVE_UP];
    return original(device, primitive_type, primitive_count, vertex_data, stride);
}

HRESULT __stdcall thunk_draw_indexed_primitive_up(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive_type,
    UINT minimum_vertex_index, UINT vertex_count, UINT primitive_count, const void* index_data,
    D3DFORMAT index_format, const void* vertex_data, UINT stride) {
    dispatch_pre_draw(device);

    const DrawIndexedPrimitiveUPMethod original =
        reinterpret_cast<DrawIndexedPrimitiveUPMethod>(original_for(device, WD_INDEX_DRAW_INDEXED_PRIMITIVE_UP));
    if (!original) {
        return D3DERR_INVALIDCALL;
    }

    ++g_forwards[WD_INDEX_DRAW_INDEXED_PRIMITIVE_UP];
    return original(device, primitive_type, minimum_vertex_index, vertex_count, primitive_count,
        index_data, index_format, vertex_data, stride);
}

// A switch rather than a table: a table of these needs a cast the compiler
// cannot fold, which is a dynamic initialiser -- code at load, and this image
// runs none.
DeviceMethod thunk_for(int index) {
    switch (index) {
    case WD_INDEX_RESET:
        return reinterpret_cast<DeviceMethod>(&thunk_reset);
    case WD_INDEX_SET_RENDER_TARGET:
        return reinterpret_cast<DeviceMethod>(&thunk_set_render_target);
    case WD_INDEX_DRAW_PRIMITIVE:
        return reinterpret_cast<DeviceMethod>(&thunk_draw_primitive);
    case WD_INDEX_DRAW_INDEXED_PRIMITIVE:
        return reinterpret_cast<DeviceMethod>(&thunk_draw_indexed_primitive);
    case WD_INDEX_DRAW_PRIMITIVE_UP:
        return reinterpret_cast<DeviceMethod>(&thunk_draw_primitive_up);
    case WD_INDEX_DRAW_INDEXED_PRIMITIVE_UP:
        return reinterpret_cast<DeviceMethod>(&thunk_draw_indexed_primitive_up);
    default:
        break;
    }

    return 0;
}

// Put back what we found, in the slots we changed. A slot taken over by someone
// else since is left alone: writing our saved original into it would silently
// unhook them.
int roll_back(DeviceMethod* vtable, int patched, const DeviceMethod* originals) {
    int restored = 0;
    for (int index = 0; index < patched; ++index) {
        const int slot = slot_numbers_[index];
        if (vtable[slot] != thunk_for(index)) {
            continue;
        }

        DeviceMethod previous = 0;
        if (patch_vtable_slot(vtable, slot, originals[index], previous)) {
            ++restored;
        }
    }
    return restored;
}

uint32_t install(IDirect3DDevice8* device) {
    if (!is_readable_span(reinterpret_cast<uintptr_t>(device), sizeof(DeviceMethod*))) {
        return 0;
    }

    DeviceMethod* const vtable = *reinterpret_cast<DeviceMethod* const*>(device);
    const VtableTable* table = g_vtable_table;
    for (LONG i = 0; table && i < table->count; ++i) {
        if (table->entries[i]->vtable == vtable) {
            // Re-adoption, and the only form of it that can be correct: this
            // record holds the originals really saved for this vtable, so it is
            // answered from rather than re-reading the slots. A record retained
            // from an install that could not be rolled back has `complete` 0,
            // which is a refusal.
            return table->entries[i]->complete;
        }
    }

    if (!is_readable_span(reinterpret_cast<uintptr_t>(vtable),
            sizeof(DeviceMethod) * WD_DEVICE_VTABLE_SLOTS)) {
        return 0;
    }

    for (int index = 0; index < WD_SLOT_COUNT; ++index) {
        const DeviceMethod occupant = vtable[slot_numbers_[index]];

        // Our own thunk, in a vtable no record claims. Saving it as the
        // original is unrecoverable: the thunk would forward to itself and the
        // first draw call would recurse until the render thread's stack ended.
        // Checked before the executable test, because our thunks are executable
        // code and sail through it.
        //
        // Refusal, never adoption: our code is in the slots and no record holds
        // originals for them, so nothing says what to forward to. Refusing
        // leaves the vtable exactly as found.
        if (occupant == thunk_for(index)) {
            return 0;
        }

        if (!is_executable_code(reinterpret_cast<uintptr_t>(occupant))) {
            return 0;
        }
    }

    // A record dropped by check_slots is free again and is reused in place. The
    // allocation happens here, on the Lua thread and before anything is patched,
    // so running out of memory leaves the vtable exactly as it was found.
    LONG record_index = first_free_vtable(table);
    if (record_index < 0) {
        if (!grow_vtables()) {
            return 0;
        }
        table = g_vtable_table;
        record_index = first_free_vtable(table);
    }
    if (record_index < 0) {
        return 0;
    }

    // The record is published before the first slot is patched. The other order
    // leaves a window in which a thunk is reachable but its original is not
    // findable, and that window is on the render thread.
    VtableRecord& record = *table->entries[record_index];
    for (int index = 0; index < WD_SLOT_COUNT; ++index) {
        record.originals[index] = vtable[slot_numbers_[index]];
    }
    record.complete = 0;
    MemoryBarrier();
    record.vtable = vtable;

    int patched = 0;
    bool refused = false;
    for (int index = 0; index < WD_SLOT_COUNT; ++index) {
        // This image depends on the original's address forever, so its module
        // is pinned. An address in no module cannot be pinned or trusted.
        if (!pin_module_containing(record.originals[index])) {
            ++g_install_refused_trampoline;
            refused = true;
            break;
        }

        DeviceMethod previous = 0;
        if (!patch_vtable_slot(vtable, slot_numbers_[index], thunk_for(index), previous)) {
            refused = true;
            break;
        }
        ++patched;

        // What the write replaced must be what the read pinned: a foreign
        // hooker landing in the slot in between would be skipped by the address
        // we saved, and its module is not the one we pinned.
        if (previous != record.originals[index]) {
            record.originals[index] = previous;
            refused = true;
            break;
        }
    }

    if (refused) {
        const int restored = roll_back(vtable, patched, record.originals);
        if (restored == patched) {
            record.vtable = 0;
        }
        // A slot that could not be put back still holds our thunk, which must
        // keep finding its original, so the record stays.
        return 0;
    }

    record.complete = 1;

    // No method of the device is called here -- not even AddRef, which is a
    // method like any other. This function runs on the caller's thread, not the
    // render thread, and FFXI's device is created without
    // D3DCREATE_MULTITHREADED, so any method entered from here races the render
    // thread inside the same device.
    //
    // Nothing needs a reference. A record is never freed, so its address is
    // stable for the life of the process; what keeps a forward's target mapped
    // is pin_module_containing, not a refcount; and `vtable` is compared, never
    // dereferenced, on the render path.
    //
    // The accepted residual: a per-device heap vtable could be freed and its
    // address recycled, and this record would then claim a vtable it does not
    // hold the originals for. install() would answer the re-adoption scan with
    // `complete` and patch nothing, so an engine is told it is hooked when it is
    // not. It cannot forward wrongly and cannot stop the client drawing.
    return 1;
}

uint32_t __stdcall api_ensure_hooks(IDirect3DDevice8* device) {
    if (!device) {
        return 0;
    }

    lock();
    const uint32_t installed = install(device);
    unlock();
    return installed;
}

uint32_t __stdcall api_register_set(const WdHandlerSet* set) {
    if (!set) {
        return 0;
    }

    // A set built against a newer ABI is longer than ours and still starts with
    // our fields, which is what append-only buys.
    if (set->abi_version < 1 || set->size < sizeof(WdHandlerSet)) {
        return 0;
    }

    uint32_t index = 0;
    lock();
    const SlotTable* table = g_slot_table;
    for (LONG i = 0; table && i < table->count; ++i) {
        if (table->entries[i]->set == set) {
            index = static_cast<uint32_t>(i) + 1;
            break;
        }
    }

    if (index == 0) {
        // An unregistered slot is free again and is reused in place; the table
        // only grows when every slot is taken.
        LONG free_index = first_free_slot(table);
        if (free_index < 0 && grow_slots()) {
            table = g_slot_table;
            free_index = first_free_slot(table);
        }

        if (free_index >= 0) {
            table->entries[free_index]->set = set;
            MemoryBarrier();
            if (g_high_water < free_index + 1) {
                g_high_water = free_index + 1;
            }
            ++g_sets_registered;
            ++g_sets_live;
            index = static_cast<uint32_t>(free_index) + 1;
        }
    }
    unlock();

    return index;
}

void __stdcall api_unregister_set(const WdHandlerSet* set) {
    if (!set) {
        return;
    }

    // The slot, not its index: the slot never moves, so this stays the object
    // every reader is counting on however the table grows during the drain.
    HandlerSlot* found = 0;
    lock();
    const SlotTable* const table = g_slot_table;
    for (LONG i = 0; table && i < table->count; ++i) {
        if (table->entries[i]->set == set) {
            table->entries[i]->set = 0;
            found = table->entries[i];
            break;
        }
    }

    if (found) {
        // Recomputed, so one transient last set does not tax every draw call
        // for the rest of the process.
        LONG high = 0;
        for (LONG i = 0; i < table->count; ++i) {
            if (table->entries[i]->set) {
                high = i + 1;
            }
        }
        g_high_water = high;
        if (g_sets_live > 0) {
            --g_sets_live;
        }
    }
    unlock();

    // Drained outside the lock: handlers can take locks of their own, and a
    // drain holding ours would deadlock against a thunk waiting for it. Once
    // this returns, no thunk can be inside this set and the caller may free it.
    if (found) {
        while (InterlockedCompareExchange(&found->inflight, 0, 0) != 0) {
            SwitchToThread();
        }
    }
}

uint32_t __stdcall api_check_slots(void) {
    uint32_t stomped = 0;

    lock();
    const VtableTable* const table = g_vtable_table;
    for (LONG i = 0; table && i < table->count; ++i) {
        VtableRecord* const record = table->entries[i];
        DeviceMethod* const vtable = record->vtable;
        if (!vtable) {
            continue;
        }

        // Readability is the only question that may drop a record, and it is
        // asked first because reading a vtable whose wrapper image has gone is
        // a fault rather than a mismatch. It is safe because memory that cannot
        // be read cannot be the vtable a call is dispatched through, so no thunk
        // of ours is still reachable and there is nothing left to forward.
        //
        // What the slots contain must never decide it. A slot holding a foreign
        // hook that has since unmapped is not executable while the vtable it
        // sits in is very much alive with our other five thunks in it. Dropping
        // on that reading leaves every remaining thunk unable to find its
        // original -- every forward returning D3DERR_INVALIDCALL for the rest of
        // the process, Reset never reaching d3d8 -- and frees the record for an
        // install that would then save our own thunk as the original.
        if (!is_readable_span(reinterpret_cast<uintptr_t>(vtable),
                sizeof(DeviceMethod) * WD_DEVICE_VTABLE_SLOTS)) {
            // Cleared in place, never compacted, so no address a reader may be
            // walking moves. The originals stay beside it unread: a reader
            // matches on `vtable`, and install rewrites all six before it
            // publishes the record again.
            record->vtable = 0;
            ++g_vtables_dropped;
            continue;
        }

        // Readable, so the slots are a report and never a verdict. A slot that
        // is not ours is a stomp however it got that way, and the record is
        // retained either way: our thunks may still be reachable through the
        // slots we do hold, or through a foreign hook chaining to one.
        for (int index = 0; index < WD_SLOT_COUNT; ++index) {
            if (vtable[slot_numbers_[index]] != thunk_for(index)) {
                ++stomped;
            }
        }
    }

    // Reported, never repaired. Re-chaining atop a stomper is undecidable: once
    // a foreign hook writes back what it saved, "it left" and "it is still
    // there" are the same bytes.
    if (stomped > g_stomps_detected) {
        g_stomps_detected = stomped;
    }
    unlock();

    return stomped;
}

// The resident daemon's build, not the caller's: a loser hands out the winner's
// function pointer, so an engine asking a loser still learns whose copy won.
const char* __stdcall api_build_id(void) {
    return WD_DAEMON_BUILD;
}

void __stdcall api_stats(WdDaemonStats* out) {
    if (!out) {
        return;
    }

    // The caller declares its own sizeof in `size`; write no more than that,
    // and report back how much was written.
    const uint32_t capacity = out->size;
    if (capacity < sizeof(uint32_t)) {
        return;
    }

    WdDaemonStats snapshot;
    std::memset(&snapshot, 0, sizeof(snapshot));

    lock();
    snapshot.sets_registered = g_sets_registered;
    snapshot.sets_live = g_sets_live;
    for (int index = 0; index < WD_SLOT_COUNT; ++index) {
        snapshot.forwards[index] = g_forwards[index];
    }
    snapshot.stomps_detected = g_stomps_detected;
    snapshot.vtables_dropped = g_vtables_dropped;
    snapshot.install_refused_trampoline = g_install_refused_trampoline;
    unlock();

    const uint32_t writable = capacity < sizeof(WdDaemonStats)
        ? capacity : static_cast<uint32_t>(sizeof(WdDaemonStats));
    snapshot.size = writable;
    std::memcpy(out, &snapshot, writable);
}

// Pinned by two independent means, because hooking from an image that can unmap
// is the one thing this design cannot survive. Pin defeats FreeLibrary; the
// extra LoadLibraryW covers a loader that refused the pin. Either alone is
// enough, and neither leaves anything to balance.
bool pin_self() {
    HMODULE self = NULL;
    const bool pinned = GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        &module_anchor_, &self) != FALSE;

    if (!self) {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            &module_anchor_, &self);
    }
    if (!self) {
        return false;
    }

    const DWORD copied = GetModuleFileNameA(self, g_winner_path, MAX_PATH);
    if (copied == 0 || copied >= MAX_PATH) {
        g_winner_path[0] = '\0';
    }
    g_winner_path[MAX_PATH - 1] = '\0';

    WCHAR wide_path[MAX_PATH];
    const DWORD wide = GetModuleFileNameW(self, wide_path, MAX_PATH);
    bool loaded = false;
    if (wide != 0 && wide < MAX_PATH) {
        loaded = LoadLibraryW(wide_path) != NULL;
    }

    return pinned || loaded;
}

void fill_api(WdDaemonApi* api) {
    std::memset(api, 0, sizeof(*api));
    api->abi_version = WD_DAEMON_ABI;
    api->size = sizeof(WdDaemonApi);
    api->ensure_hooks = &api_ensure_hooks;
    api->register_set = &api_register_set;
    api->unregister_set = &api_unregister_set;
    api->check_slots = &api_check_slots;
    api->stats = &api_stats;
    api->build_id = &api_build_id;
}

void publish(WdDaemonRecord* record) {
    std::memset(record, 0, sizeof(*record));

    WdDaemonApi api;
    fill_api(&api);

    record->record_size = sizeof(WdDaemonRecord);
    record->abi_version = WD_DAEMON_ABI;
    record->api = api;
    std::memcpy(record->winner_path, g_winner_path, sizeof(record->winner_path));
    record->winner_path[MAX_PATH - 1] = '\0';
    std::memcpy(record->winner_build, WD_DAEMON_BUILD, sizeof(WD_DAEMON_BUILD));
    record->winner_build[WD_BUILD_MAX - 1] = '\0';

    // Written last, behind a barrier: magic is the only thing that says the
    // rest of the record is there, including to an image that arrives after a
    // crash mid-publish and finds it still zero.
    MemoryBarrier();
    record->magic = WD_MAGIC;

    g_public_api = api;
    g_resident_abi = WD_DAEMON_ABI;
    g_api_valid = 1;
}

// A loser copies the api out of the record by value and hands out a pointer to
// that copy, so nothing it returns points into the record or the winner's data.
void adopt(const WdDaemonRecord* record) {
    if (record->magic != WD_MAGIC || record->record_size < sizeof(WdDaemonRecord)) {
        return;
    }
    if (record->api.size < sizeof(WdDaemonApi) || record->abi_version < 1) {
        return;
    }

    WdDaemonApi api;
    std::memset(&api, 0, sizeof(api));
    std::memcpy(&api, &record->api, sizeof(api));

    // Never advertise more than was copied: a longer api in the record is a
    // newer daemon, and the fields past ours were not brought across.
    api.size = sizeof(WdDaemonApi);

    if (!api.ensure_hooks || !api.register_set || !api.unregister_set
        || !api.check_slots || !api.stats || !api.build_id) {
        return;
    }

    g_public_api = api;
    g_resident_abi = record->abi_version;
    g_api_valid = 1;
}

void run_election() {
    // Pin before anything else. An image that cannot pin itself publishes
    // nothing and hooks nothing: refusing here is the only way not to leave an
    // address in somebody's saved original that can later unmap.
    if (!pin_self()) {
        return;
    }

    const DWORD pid = GetCurrentProcessId();
    char mapping_name[WD_NAME_MAX];
    char mutex_name[WD_NAME_MAX];
    std::snprintf(mapping_name, sizeof(mapping_name), WD_MAPPING_NAME_FORMAT, static_cast<unsigned>(pid));
    std::snprintf(mutex_name, sizeof(mutex_name), WD_MUTEX_NAME_FORMAT, static_cast<unsigned>(pid));

    HANDLE mutex = CreateMutexA(NULL, FALSE, mutex_name);
    if (!mutex) {
        return;
    }

    // WAIT_ABANDONED means the previous holder died holding it, which is the
    // mid-election crash the publish predicate below recovers. Anything else
    // means we do not hold it, and proceeding would race two images through the
    // publish.
    const DWORD wait = WaitForSingleObject(mutex, election_wait_ms_);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return;
    }

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
        WD_MAPPING_BYTES, mapping_name);
    // Read immediately: any call in between overwrites it.
    const DWORD create_error = GetLastError();
    if (!mapping) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return;
    }

    const bool created = create_error != ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, WD_MAPPING_BYTES);
    if (!view) {
        CloseHandle(mapping);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return;
    }

    WdDaemonRecord* const record = static_cast<WdDaemonRecord*>(view);

    // Evaluated under the mutex. A magic that is not ours proves nobody finished
    // publishing, so taking over is safe -- without this, one image dying
    // mid-election poisons the mapping for the life of the process.
    if (created || record->magic != WD_MAGIC) {
        publish(record);
    } else {
        adopt(record);
    }

    ReleaseMutex(mutex);

    // The view, the section handle and the mutex handle are kept for the life of
    // the process: the record has to outlive every image that reads it.
}

}  // namespace

// Called after LoadLibraryW returns, never from DllMain: the election takes a
// lock and may LoadLibrary, which deadlocks under the loader lock. The version
// test is a range -- an engine works with any daemon at least as new as the one
// it was built for.
//
// The predicate is the election's result, never the fact that it was attempted.
// Every step can fail transiently -- the mutex wait timing out against another
// image mid-election, handle or address-space pressure, a loader refusing the
// pin -- and latching on the attempt makes all of them permanent, because an
// image that pins itself can never be reloaded to try again.
//
// g_api_valid is that result and the whole state, set last by the only two
// writers. Success is idempotent and failure is retryable by the next caller.
// Both tests happen under the lock the election runs under, so several engine
// images cannot race each other into it. One attempt per call; nothing loops.
extern "C" const WdDaemonApi* __stdcall wd_daemon_acquire(uint32_t min_abi) {
    lock();
    if (!g_api_valid) {
        run_election();
    }
    unlock();

    if (!g_api_valid || g_resident_abi < min_abi) {
        return NULL;
    }

    return &g_public_api;
}

// Nothing, for every reason code. Work here runs under the loader lock, and the
// only work this image could want to do -- draining, unhooking, freeing -- is
// exactly what must never happen there.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
