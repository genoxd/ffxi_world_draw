// daemon_harness - the daemon exercised across real DLL boundaries, under wine.
//
// The single-exe harness cannot test any of what the daemon is for: an election
// between two loaded images, a pin that outlives FreeLibrary, a drain that must
// block a caller until another thread leaves a handler, or an engine image that
// unmaps while the daemon keeps running. So this builds the real
// worlddraw_daemon.dll, three fake-engine images under one basename in three
// directories, and drives them from here.
//
// The device is a synthetic 97-entry vtable in memory of our own: the daemon
// never needs a real one, and a synthetic one lets a slot be freed, stomped or
// pointed at an allocated trampoline on demand. What it does NOT model is
// d3d8's real page or a wrapper like ReShade, which usually wraps the device
// rather than stomping a vtable -- this models that topology, it does not
// reproduce it.
//
//   ./build.sh      builds everything and runs it; non-zero if a check fails

#define WIN32_LEAN_AND_MEAN
#include "fake_engine.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

// ---- checks --------------------------------------------------------------

int checks_run = 0;
int checks_failed = 0;

void check(bool ok, const char* what) {
    ++checks_run;
    if (!ok) {
        ++checks_failed;
        std::printf("FAIL   : %s\n", what);
    }
}

void check_u(unsigned long got, unsigned long want, const char* what) {
    const bool ok = got == want;
    if (!ok) {
        std::printf("         %s: got %lu want %lu\n", what, got, want);
    }
    check(ok, what);
}

void check_p(const void* got, const void* want, const char* what) {
    const bool ok = got == want;
    if (!ok) {
        std::printf("         %s: got %p want %p\n", what, got, want);
    }
    check(ok, what);
}

// ---- the synthetic device ------------------------------------------------

typedef void (*Method)();
typedef HRESULT (__stdcall* ResetMethod)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (__stdcall* SetRenderTargetMethod)(IDirect3DDevice8*, IDirect3DSurface8*, IDirect3DSurface8*);
typedef HRESULT (__stdcall* DrawPrimitiveMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (__stdcall* DrawIndexedPrimitiveMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
typedef HRESULT (__stdcall* DrawPrimitiveUPMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (__stdcall* DrawIndexedPrimitiveUPMethod)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
    const void*, D3DFORMAT, const void*, UINT);

struct FakeDevice {
    Method* vtable;             // first member: this is what the daemon reads
    LONG refcount;
    LONG calls[WD_SLOT_COUNT];
    int id;
};

const HRESULT original_result_ = static_cast<HRESULT>(0x08150000);

// ---- which thread touched the device -------------------------------------
// THE RULE THIS EXISTS FOR. FFXI creates its device without
// D3DCREATE_MULTITHREADED -- measured in the live client, behaviour flags
// 0x00000040 -- and the D3D hooks run on a different OS thread from Windower's
// Lua. The D3D8 runtime therefore serialises nothing, and a device method
// entered from the main thread races the game's render thread inside the same
// device. The engine half of that rule is checked statically by
// tools/offline_harness/gate_device_calls.py. THE DAEMON'S HALF IS CHECKED
// HERE, and it is checked by watching the DEVICE rather than by reading the
// daemon's source: every method of the synthetic device records the thread it
// was called on, a window is armed around a full attach / ensure_hooks /
// register / draw / unregister cycle whose device work runs on a thread of its
// own, and every call recorded in that window has to have come from that
// thread. phase_thread_gate then makes the violation itself, from the main
// thread, and requires the gate to see it: a gate that cannot fail proves
// nothing.
//
// AddRef and Release are counted for the WHOLE run, armed or not. The daemon
// takes no reference on any device it patches -- there is no reference to take
// once, to take twice, or to defer -- and the cheapest way to say that is a
// counter that must be zero when every phase has finished.

enum {
    DEVICE_METHOD_QUERY_INTERFACE = WD_SLOT_COUNT,
    DEVICE_METHOD_ADD_REF,
    DEVICE_METHOD_RELEASE,
    DEVICE_METHOD_FILLER
};

struct DeviceCall {
    DWORD thread;
    int method;
};

// Harness bookkeeping, not a daemon limit: the armed windows below make a
// known, small number of calls, and an overflow is a check of its own rather
// than something to grow for.
const LONG device_log_max_ = 512;
DeviceCall device_log_[device_log_max_];
volatile LONG device_log_count_ = 0;
volatile LONG device_log_lost_ = 0;
volatile LONG device_watch_ = 0;
volatile LONG add_ref_calls_ = 0;
volatile LONG release_calls_ = 0;

void note_device_call(int method) {
    if (method == DEVICE_METHOD_ADD_REF) {
        InterlockedIncrement(&add_ref_calls_);
    } else if (method == DEVICE_METHOD_RELEASE) {
        InterlockedIncrement(&release_calls_);
    }

    if (!device_watch_) {
        return;
    }

    const LONG at = InterlockedIncrement(&device_log_count_) - 1;
    if (at < 0 || at >= device_log_max_) {
        InterlockedIncrement(&device_log_lost_);
        return;
    }

    device_log_[at].thread = GetCurrentThreadId();
    device_log_[at].method = method;
}

void arm_device_watch() {
    InterlockedExchange(&device_log_count_, 0);
    InterlockedExchange(&device_log_lost_, 0);
    MemoryBarrier();
    InterlockedExchange(&device_watch_, 1);
}

// Answers with how many entries the log really holds, so a caller never reads
// past what was written.
LONG disarm_device_watch() {
    InterlockedExchange(&device_watch_, 0);
    MemoryBarrier();
    const LONG count = device_log_count_;
    return count < device_log_max_ ? count : device_log_max_;
}

int device_calls_from(DWORD thread, LONG count) {
    int found = 0;
    for (LONG i = 0; i < count; ++i) {
        if (device_log_[i].thread == thread) {
            ++found;
        }
    }
    return found;
}

bool device_method_seen(int method, LONG count) {
    for (LONG i = 0; i < count; ++i) {
        if (device_log_[i].method == method) {
            return true;
        }
    }
    return false;
}

// Two independent families of originals, so a second vtable can be shown to
// forward to ITS originals and not to the first one's.
LONG family_calls_[2][WD_SLOT_COUNT];
void (*on_reset_)(void) = 0;

HRESULT record_call(int family, int index, FakeDevice* device) {
    note_device_call(index);
    ++family_calls_[family][index];
    if (device) {
        ++device->calls[index];
    }
    return static_cast<HRESULT>(original_result_ + index);
}

HRESULT __stdcall a_reset(FakeDevice* device, D3DPRESENT_PARAMETERS*) {
    // The place a set can appear between the daemon's two Reset loops.
    if (on_reset_) {
        on_reset_();
    }
    return record_call(0, WD_INDEX_RESET, device);
}
HRESULT __stdcall a_set_render_target(FakeDevice* device, IDirect3DSurface8*, IDirect3DSurface8*) {
    return record_call(0, WD_INDEX_SET_RENDER_TARGET, device);
}
HRESULT __stdcall a_draw_primitive(FakeDevice* device, D3DPRIMITIVETYPE, UINT, UINT) {
    return record_call(0, WD_INDEX_DRAW_PRIMITIVE, device);
}
HRESULT __stdcall a_draw_indexed_primitive(FakeDevice* device, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT) {
    return record_call(0, WD_INDEX_DRAW_INDEXED_PRIMITIVE, device);
}
HRESULT __stdcall a_draw_primitive_up(FakeDevice* device, D3DPRIMITIVETYPE, UINT, const void*, UINT) {
    return record_call(0, WD_INDEX_DRAW_PRIMITIVE_UP, device);
}
HRESULT __stdcall a_draw_indexed_primitive_up(FakeDevice* device, D3DPRIMITIVETYPE, UINT, UINT, UINT,
    const void*, D3DFORMAT, const void*, UINT) {
    return record_call(0, WD_INDEX_DRAW_INDEXED_PRIMITIVE_UP, device);
}

HRESULT __stdcall b_reset(FakeDevice* device, D3DPRESENT_PARAMETERS*) {
    return record_call(1, WD_INDEX_RESET, device);
}
HRESULT __stdcall b_set_render_target(FakeDevice* device, IDirect3DSurface8*, IDirect3DSurface8*) {
    return record_call(1, WD_INDEX_SET_RENDER_TARGET, device);
}
HRESULT __stdcall b_draw_primitive(FakeDevice* device, D3DPRIMITIVETYPE, UINT, UINT) {
    return record_call(1, WD_INDEX_DRAW_PRIMITIVE, device);
}
HRESULT __stdcall b_draw_indexed_primitive(FakeDevice* device, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT) {
    return record_call(1, WD_INDEX_DRAW_INDEXED_PRIMITIVE, device);
}
HRESULT __stdcall b_draw_primitive_up(FakeDevice* device, D3DPRIMITIVETYPE, UINT, const void*, UINT) {
    return record_call(1, WD_INDEX_DRAW_PRIMITIVE_UP, device);
}
HRESULT __stdcall b_draw_indexed_primitive_up(FakeDevice* device, D3DPRIMITIVETYPE, UINT, UINT, UINT,
    const void*, D3DFORMAT, const void*, UINT) {
    return record_call(1, WD_INDEX_DRAW_INDEXED_PRIMITIVE_UP, device);
}

ULONG __stdcall fake_add_ref(FakeDevice* device) {
    note_device_call(DEVICE_METHOD_ADD_REF);
    return static_cast<ULONG>(InterlockedIncrement(&device->refcount));
}
ULONG __stdcall fake_release(FakeDevice* device) {
    note_device_call(DEVICE_METHOD_RELEASE);
    return static_cast<ULONG>(InterlockedDecrement(&device->refcount));
}
HRESULT __stdcall fake_query_interface(FakeDevice*, const IID*, void**) {
    note_device_call(DEVICE_METHOD_QUERY_INTERFACE);
    return E_NOINTERFACE;
}
// The other 91 slots. Nothing in the daemon has any business in one, and a
// call that landed here would be recorded like any other.
HRESULT __stdcall fake_filler(FakeDevice*) {
    note_device_call(DEVICE_METHOD_FILLER);
    return S_OK;
}

// A stomper: executable, in a module, and not ours.
HRESULT __stdcall foreign_draw_primitive(FakeDevice* device, D3DPRIMITIVETYPE, UINT, UINT) {
    return record_call(0, WD_INDEX_DRAW_PRIMITIVE, device);
}

Method* make_vtable(int family) {
    Method* const vtable = static_cast<Method*>(VirtualAlloc(NULL,
        sizeof(Method) * WD_DEVICE_VTABLE_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!vtable) {
        return 0;
    }

    for (int i = 0; i < WD_DEVICE_VTABLE_SLOTS; ++i) {
        vtable[i] = reinterpret_cast<Method>(&fake_filler);
    }
    vtable[0] = reinterpret_cast<Method>(&fake_query_interface);
    vtable[1] = reinterpret_cast<Method>(&fake_add_ref);
    vtable[2] = reinterpret_cast<Method>(&fake_release);

    if (family == 0) {
        vtable[WD_SLOT_RESET] = reinterpret_cast<Method>(&a_reset);
        vtable[WD_SLOT_SET_RENDER_TARGET] = reinterpret_cast<Method>(&a_set_render_target);
        vtable[WD_SLOT_DRAW_PRIMITIVE] = reinterpret_cast<Method>(&a_draw_primitive);
        vtable[WD_SLOT_DRAW_INDEXED_PRIMITIVE] = reinterpret_cast<Method>(&a_draw_indexed_primitive);
        vtable[WD_SLOT_DRAW_PRIMITIVE_UP] = reinterpret_cast<Method>(&a_draw_primitive_up);
        vtable[WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP] = reinterpret_cast<Method>(&a_draw_indexed_primitive_up);
    } else {
        vtable[WD_SLOT_RESET] = reinterpret_cast<Method>(&b_reset);
        vtable[WD_SLOT_SET_RENDER_TARGET] = reinterpret_cast<Method>(&b_set_render_target);
        vtable[WD_SLOT_DRAW_PRIMITIVE] = reinterpret_cast<Method>(&b_draw_primitive);
        vtable[WD_SLOT_DRAW_INDEXED_PRIMITIVE] = reinterpret_cast<Method>(&b_draw_indexed_primitive);
        vtable[WD_SLOT_DRAW_PRIMITIVE_UP] = reinterpret_cast<Method>(&b_draw_primitive_up);
        vtable[WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP] = reinterpret_cast<Method>(&b_draw_indexed_primitive_up);
    }

    return vtable;
}

// Harness bookkeeping, not a daemon limit: one entry per synthetic device this
// run ever builds. make_device refuses past the end rather than running off it.
FakeDevice devices_[128];
int device_count_ = 0;

FakeDevice* make_device(Method* vtable) {
    if (device_count_ >= static_cast<int>(sizeof(devices_) / sizeof(devices_[0]))) {
        return 0;
    }

    FakeDevice* const device = &devices_[device_count_];
    std::memset(device, 0, sizeof(*device));
    device->vtable = vtable;
    device->id = device_count_;
    ++device_count_;
    return device;
}

IDirect3DDevice8* as_device(FakeDevice* device) {
    return reinterpret_cast<IDirect3DDevice8*>(device);
}

HRESULT call_reset(FakeDevice* device) {
    D3DPRESENT_PARAMETERS parameters;
    std::memset(&parameters, 0, sizeof(parameters));
    return reinterpret_cast<ResetMethod>(device->vtable[WD_SLOT_RESET])(as_device(device), &parameters);
}
HRESULT call_set_render_target(FakeDevice* device) {
    return reinterpret_cast<SetRenderTargetMethod>(device->vtable[WD_SLOT_SET_RENDER_TARGET])(
        as_device(device), 0, 0);
}
HRESULT call_draw_primitive(FakeDevice* device) {
    return reinterpret_cast<DrawPrimitiveMethod>(device->vtable[WD_SLOT_DRAW_PRIMITIVE])(
        as_device(device), D3DPT_TRIANGLELIST, 0, 1);
}
HRESULT call_draw_indexed_primitive(FakeDevice* device) {
    return reinterpret_cast<DrawIndexedPrimitiveMethod>(device->vtable[WD_SLOT_DRAW_INDEXED_PRIMITIVE])(
        as_device(device), D3DPT_TRIANGLELIST, 0, 3, 0, 1);
}
HRESULT call_draw_primitive_up(FakeDevice* device) {
    return reinterpret_cast<DrawPrimitiveUPMethod>(device->vtable[WD_SLOT_DRAW_PRIMITIVE_UP])(
        as_device(device), D3DPT_TRIANGLELIST, 1, 0, 0);
}
HRESULT call_draw_indexed_primitive_up(FakeDevice* device) {
    return reinterpret_cast<DrawIndexedPrimitiveUPMethod>(device->vtable[WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP])(
        as_device(device), D3DPT_TRIANGLELIST, 0, 3, 1, 0, D3DFMT_INDEX16, 0, 0);
}

// ---- watching the daemon's VirtualProtect --------------------------------
// Patching the daemon's import is how the harness sees the patch sequence from
// outside: it proves that a refused install really did patch earlier slots and
// really did put them back, which is not observable from the vtable afterwards.

typedef BOOL (WINAPI* VirtualProtectMethod)(LPVOID, SIZE_T, DWORD, PDWORD);

VirtualProtectMethod real_virtual_protect_ = 0;
const void* protect_log_[64];
int protect_log_count_ = 0;
const void* protect_fail_address_ = 0;

BOOL WINAPI watching_virtual_protect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect) {
    if (protect_log_count_ < 64) {
        protect_log_[protect_log_count_] = address;
        ++protect_log_count_;
    }
    if (address == protect_fail_address_) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return real_virtual_protect_(address, size, new_protect, old_protect);
}

// ---- watching the daemon's HeapAlloc -------------------------------------
// The daemon's tables grow by allocating, so its allocations are the only
// outside view of a growth: a registration that allocates grew the table and
// one that does not reused a slot. The same seam refuses an allocation, which
// is the only way to reach the out-of-memory refusal paths from here.

typedef LPVOID (WINAPI* HeapAllocMethod)(HANDLE, DWORD, SIZE_T);

HeapAllocMethod real_heap_alloc_ = 0;
uintptr_t* heap_alloc_import_ = 0;
uintptr_t heap_alloc_original_ = 0;
volatile LONG alloc_count_ = 0;
volatile LONG alloc_fail_at_ = 0;   /* fail this allocation, 1-based; 0 = none */
volatile LONG alloc_fail_all_ = 0;

LPVOID WINAPI watching_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes) {
    const LONG at = InterlockedIncrement(&alloc_count_);
    if (alloc_fail_all_ || (alloc_fail_at_ != 0 && at == alloc_fail_at_)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return real_heap_alloc_(heap, flags, bytes);
}

uintptr_t* find_import(HMODULE module, const char* library, const char* function) {
    BYTE* const base = reinterpret_cast<BYTE*>(module);
    const IMAGE_DOS_HEADER* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    const IMAGE_NT_HEADERS* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) {
        return 0;
    }

    const IMAGE_IMPORT_DESCRIPTOR* descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* const name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (lstrcmpiA(name, library) != 0) {
            continue;
        }

        const DWORD names = descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
            : descriptor->FirstThunk;
        const IMAGE_THUNK_DATA* thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + names);
        IMAGE_THUNK_DATA* entry = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; thunk->u1.AddressOfData; ++thunk, ++entry) {
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) {
                continue;
            }

            const IMAGE_IMPORT_BY_NAME* const imported =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(imported->Name), function) == 0) {
                return reinterpret_cast<uintptr_t*>(&entry->u1.Function);
            }
        }
    }

    return 0;
}

bool write_import(uintptr_t* entry, uintptr_t value, uintptr_t* previous) {
    DWORD old_protect = 0;
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &old_protect)) {
        return false;
    }

    if (previous) {
        *previous = *entry;
    }
    *entry = value;

    DWORD ignored = 0;
    VirtualProtect(entry, sizeof(*entry), old_protect, &ignored);
    return true;
}

bool install_alloc_watcher(HMODULE daemon) {
    heap_alloc_import_ = find_import(daemon, "KERNEL32.dll", "HeapAlloc");
    if (!heap_alloc_import_) {
        return false;
    }
    if (!write_import(heap_alloc_import_, reinterpret_cast<uintptr_t>(&watching_heap_alloc),
            &heap_alloc_original_)) {
        heap_alloc_import_ = 0;
        return false;
    }
    real_heap_alloc_ = reinterpret_cast<HeapAllocMethod>(heap_alloc_original_);
    InterlockedExchange(&alloc_count_, 0);
    InterlockedExchange(&alloc_fail_at_, 0);
    InterlockedExchange(&alloc_fail_all_, 0);
    return true;
}

void remove_alloc_watcher() {
    InterlockedExchange(&alloc_fail_at_, 0);
    InterlockedExchange(&alloc_fail_all_, 0);
    if (heap_alloc_import_) {
        uintptr_t ignored = 0;
        write_import(heap_alloc_import_, heap_alloc_original_, &ignored);
        heap_alloc_import_ = 0;
    }
}

LONG take_allocations() {
    return InterlockedExchange(&alloc_count_, 0);
}

// ---- loading --------------------------------------------------------------

// GetProcAddress returns a function pointer of a type nothing here has, and a
// cast straight to the real one is a cast between incompatible function types.
// The address is what we want, so the address is what we take.
uintptr_t proc_address(HMODULE module, const char* name) {
    return reinterpret_cast<uintptr_t>(GetProcAddress(module, name));
}

struct Engine {
    HMODULE module;
    FeCreateSet create_set;
    FeDestroySet destroy_set;
    FeSealSet seal_set;
    FeModule self;
};

bool load_engine(const char* path, Engine* engine) {
    std::memset(engine, 0, sizeof(*engine));
    engine->module = LoadLibraryA(path);
    if (!engine->module) {
        return false;
    }

    engine->create_set = reinterpret_cast<FeCreateSet>(proc_address(engine->module, "fe_create_set"));
    engine->destroy_set = reinterpret_cast<FeDestroySet>(proc_address(engine->module, "fe_destroy_set"));
    engine->seal_set = reinterpret_cast<FeSealSet>(proc_address(engine->module, "fe_seal_set"));
    engine->self = reinterpret_cast<FeModule>(proc_address(engine->module, "fe_module"));
    return engine->create_set && engine->destroy_set && engine->seal_set && engine->self;
}

HMODULE module_of(const void* address) {
    HMODULE module = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(address), &module);
    return module;
}

HMODULE module_of_method(Method method) {
    return module_of(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(method)));
}

WdDaemonStats read_stats(const WdDaemonApi* api) {
    WdDaemonStats stats;
    std::memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    api->stats(&stats);
    return stats;
}

bool open_record(DWORD pid, HANDLE* mapping, WdDaemonRecord** record) {
    char name[WD_NAME_MAX];
    std::snprintf(name, sizeof(name), WD_MAPPING_NAME_FORMAT, static_cast<unsigned>(pid));
    *mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!*mapping) {
        *record = 0;
        return false;
    }

    *record = static_cast<WdDaemonRecord*>(MapViewOfFile(*mapping, FILE_MAP_ALL_ACCESS, 0, 0, WD_MAPPING_BYTES));
    return *record != 0;
}

bool ends_with(const char* text, const char* tail) {
    const size_t text_length = std::strlen(text);
    const size_t tail_length = std::strlen(tail);
    return text_length >= tail_length && lstrcmpiA(text + text_length - tail_length, tail) == 0;
}

// Written under another name and moved into place: a reader polling for one of
// these can otherwise catch it half-written.
bool announce(const char* path, const char* temporary, unsigned long value) {
    FILE* const file = std::fopen(temporary, "wb");
    if (!file) {
        return false;
    }
    std::fprintf(file, "%lu\n", value);
    std::fclose(file);
    return MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING) != FALSE;
}

unsigned long await_announcement(const char* path, int timeout_ms) {
    unsigned long value = 0;
    for (int waited = 0; waited < timeout_ms && value == 0; waited += 20) {
        FILE* const file = std::fopen(path, "rb");
        if (file) {
            if (std::fscanf(file, "%lu", &value) != 1) {
                value = 0;
            }
            std::fclose(file);
        }
        if (value == 0) {
            Sleep(20);
        }
    }
    return value;
}

void write_result(const char* path) {
    FILE* const file = std::fopen(path, "wb");
    if (file) {
        std::fprintf(file, "%d %d\n", checks_run, checks_failed);
        std::fclose(file);
    }
}

}  // namespace

namespace {

// ---- shared harness state ------------------------------------------------

char base_dir_[MAX_PATH];
char exe_path_[MAX_PATH];

const WdDaemonApi* api_ = 0;
HMODULE daemon_a_ = NULL;
HMODULE daemon_b_ = NULL;
WdDaemonAcquire acquire_a_ = 0;
WdDaemonAcquire acquire_b_ = 0;

Engine engine_a_;
Engine engine_b_;

FeOrderLog order_log_;
FeControl controls_[12];

FakeDevice* device_one_ = 0;
Method* vtable_one_ = 0;

WdHandlerSet* late_set_ = 0;

struct VtableSnapshot {
    Method entries[WD_DEVICE_VTABLE_SLOTS];
};

void path_in(char* out, size_t size, const char* relative) {
    std::snprintf(out, size, "%s\\%s", base_dir_, relative);
}

void snapshot_vtable(const Method* vtable, VtableSnapshot* out) {
    std::memcpy(out->entries, vtable, sizeof(out->entries));
}

int changed_slots(const Method* vtable, const VtableSnapshot& before) {
    int changed = 0;
    for (int i = 0; i < WD_DEVICE_VTABLE_SLOTS; ++i) {
        if (vtable[i] != before.entries[i]) {
            ++changed;
        }
    }
    return changed;
}

int slots_from(const Method* vtable, HMODULE module) {
    const int numbers[WD_SLOT_COUNT] = {
        WD_SLOT_RESET, WD_SLOT_SET_RENDER_TARGET, WD_SLOT_DRAW_PRIMITIVE,
        WD_SLOT_DRAW_INDEXED_PRIMITIVE, WD_SLOT_DRAW_PRIMITIVE_UP,
        WD_SLOT_DRAW_INDEXED_PRIMITIVE_UP};
    int owned = 0;
    for (int i = 0; i < WD_SLOT_COUNT; ++i) {
        if (module_of_method(vtable[numbers[i]]) == module) {
            ++owned;
        }
    }
    return owned;
}

// ---- election, ABI range, pin --------------------------------------------

void phase_election() {
    char path_a[MAX_PATH];
    char path_b[MAX_PATH];
    path_in(path_a, sizeof(path_a), "a\\worlddraw_daemon.dll");
    path_in(path_b, sizeof(path_b), "b\\worlddraw_daemon.dll");

    daemon_a_ = LoadLibraryA(path_a);
    daemon_b_ = LoadLibraryA(path_b);
    check(daemon_a_ != NULL, "daemon image a loads");
    check(daemon_b_ != NULL, "daemon image b loads");
    check(daemon_a_ != daemon_b_, "one basename in two directories is two separate images");
    if (!daemon_a_ || !daemon_b_) {
        return;
    }

    acquire_a_ = reinterpret_cast<WdDaemonAcquire>(proc_address(daemon_a_, WD_DAEMON_ACQUIRE_NAME));
    acquire_b_ = reinterpret_cast<WdDaemonAcquire>(proc_address(daemon_b_, WD_DAEMON_ACQUIRE_NAME));
    check(acquire_a_ != 0, "wd_daemon_acquire resolves by its undecorated name in a");
    check(acquire_b_ != 0, "wd_daemon_acquire resolves by its undecorated name in b");
    if (!acquire_a_ || !acquire_b_) {
        return;
    }

    api_ = acquire_a_(WD_DAEMON_ABI);
    check(api_ != 0, "the first image to be asked elects itself");
    if (!api_) {
        return;
    }
    check_u(api_->abi_version, WD_DAEMON_ABI, "the resident daemon reports its ABI");
    check_u(api_->size, sizeof(WdDaemonApi), "the api reports its own size");
    check(api_->build_id() != 0 && std::strcmp(api_->build_id(), WD_DAEMON_BUILD) == 0,
        "and the build of the binary that is really resident");

    HANDLE mapping = NULL;
    WdDaemonRecord* record = 0;
    check(open_record(GetCurrentProcessId(), &mapping, &record), "the record exists under this process's name");
    if (record) {
        check_u(record->magic, WD_MAGIC, "magic is written");
        check_u(record->record_size, sizeof(WdDaemonRecord), "the record declares its size");
        check_u(record->abi_version, WD_DAEMON_ABI, "the record declares the ABI");
        check(ends_with(record->winner_path, "a\\worlddraw_daemon.dll"),
            "winner_path names the image that won");
        check(std::strcmp(record->winner_build, WD_DAEMON_BUILD) == 0,
            "winner_build names the build that won");
        check_p(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(record->api.ensure_hooks)),
            reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(api_->ensure_hooks)),
            "the api handed out is the api published");
    }

    const WdDaemonApi* const api_b = acquire_b_(WD_DAEMON_ABI);
    check(api_b != 0, "the second image loses and hands out the winner's api");
    if (api_b) {
        check(api_b != api_, "each image hands out its own copy of the record's api");
        check(std::memcmp(api_b, api_, sizeof(WdDaemonApi)) == 0, "the copies are identical");
        check_p(module_of(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(api_->ensure_hooks))),
            daemon_a_, "the api's functions live in the winning image");
        check_p(module_of(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(api_b->ensure_hooks))),
            daemon_a_, "the loser points at the winner's functions, not its own");
        // Both images carry the same literal, so the string alone proves
        // nothing: what proves it is which image the string lives in.
        check(std::strcmp(api_b->build_id(), WD_DAEMON_BUILD) == 0, "the loser answers build_id");
        check_p(module_of(api_b->build_id()), daemon_a_,
            "with the WINNER's build string, out of the winner's image");
    }

    check_p(acquire_a_(WD_DAEMON_ABI), api_, "acquire is idempotent");
    check(acquire_a_(WD_DAEMON_ABI + 1) == 0, "a caller needing a newer ABI than the resident one is refused");
    check(acquire_b_(WD_DAEMON_ABI + 1) == 0, "the loser refuses that caller too");
    check_p(acquire_a_(0), api_, "a caller needing an older ABI is served: the test is a range");

    // Pin: our reference is the only one either image was given, and both must
    // survive giving it back.
    const void* const winner_code =
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(api_->ensure_hooks));
    const void* const loser_code =
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(acquire_b_));
    FreeLibrary(daemon_a_);
    FreeLibrary(daemon_b_);
    check_p(module_of(winner_code), daemon_a_, "the winner is still mapped after FreeLibrary");
    check_p(module_of(loser_code), daemon_b_, "the loser pinned itself too and is still mapped");
    check_p(acquire_a_(WD_DAEMON_ABI), api_, "the winner's export is still callable");
    check(acquire_b_(WD_DAEMON_ABI) == api_b, "the loser's export is still callable");
}

// ---- installation --------------------------------------------------------

void phase_install() {
    vtable_one_ = make_vtable(0);
    device_one_ = make_device(vtable_one_);
    check(vtable_one_ != 0, "a synthetic 97-entry vtable is available");
    if (!vtable_one_) {
        return;
    }

    VtableSnapshot before;
    snapshot_vtable(vtable_one_, &before);

    check_u(api_->ensure_hooks(as_device(device_one_)), 1, "ensure_hooks takes a clean vtable");
    check_u(slots_from(vtable_one_, daemon_a_), WD_SLOT_COUNT, "all six slots hold daemon code");
    check_u(changed_slots(vtable_one_, before), WD_SLOT_COUNT, "and nothing else in the vtable moved");
    check_u(device_one_->refcount, 0,
        "the daemon patched the vtable and took NO reference on the device: AddRef is a device method, "
        "and install runs on the caller's thread");

    VtableSnapshot installed;
    snapshot_vtable(vtable_one_, &installed);
    check_u(api_->ensure_hooks(as_device(device_one_)), 1, "a second ensure_hooks is idempotent");
    check_u(changed_slots(vtable_one_, installed), 0, "it re-patched nothing");
    check_u(device_one_->refcount, 0, "and touched the device no more the second time either");

    // Zero registered sets: the forwarding path with no handlers at all.
    const LONG draws_before = family_calls_[0][WD_INDEX_DRAW_PRIMITIVE];
    const HRESULT drew = call_draw_primitive(device_one_);
    check_u(family_calls_[0][WD_INDEX_DRAW_PRIMITIVE] - draws_before, 1,
        "DrawPrimitive reaches the original exactly once");
    check_u(static_cast<unsigned long>(drew),
        static_cast<unsigned long>(original_result_ + WD_INDEX_DRAW_PRIMITIVE),
        "the original's HRESULT is what comes back");

    check_u(static_cast<unsigned long>(call_reset(device_one_)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_RESET), "Reset passes its HRESULT through");
    check_u(static_cast<unsigned long>(call_set_render_target(device_one_)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_SET_RENDER_TARGET),
        "SetRenderTarget passes its HRESULT through");
    check_u(static_cast<unsigned long>(call_draw_indexed_primitive(device_one_)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_DRAW_INDEXED_PRIMITIVE),
        "DrawIndexedPrimitive passes its HRESULT through");
    check_u(static_cast<unsigned long>(call_draw_primitive_up(device_one_)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_DRAW_PRIMITIVE_UP),
        "DrawPrimitiveUP passes its HRESULT through");
    check_u(static_cast<unsigned long>(call_draw_indexed_primitive_up(device_one_)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_DRAW_INDEXED_PRIMITIVE_UP),
        "DrawIndexedPrimitiveUP passes its HRESULT through");

    const WdDaemonStats stats = read_stats(api_);
    check_u(stats.size, sizeof(WdDaemonStats), "stats reports how much it wrote");
    check_u(stats.forwards[WD_INDEX_DRAW_PRIMITIVE], 1, "the forward counters count");
    check_u(stats.forwards[WD_INDEX_RESET], 1, "one Reset forwarded");

    // A size-gated reader from an older build: it must get its prefix and no
    // more than it asked for.
    unsigned char buffer[sizeof(WdDaemonStats) + 8];
    std::memset(buffer, 0xCD, sizeof(buffer));
    WdDaemonStats* const small = reinterpret_cast<WdDaemonStats*>(buffer);
    small->size = 12;
    api_->stats(small);
    check_u(small->size, 12, "a short stats buffer is told what it got");
    check_u(buffer[12], 0xCD, "and nothing past it was written");

    // A second device on the same vtable is the same install.
    FakeDevice* const sibling = make_device(vtable_one_);
    check_u(api_->ensure_hooks(as_device(sibling)), 1, "a second device on one vtable is already hooked");
    check_u(sibling->refcount, 0, "and its device is untouched too");

    // A device with its OWN vtable and its own originals.
    Method* const vtable_two = make_vtable(1);
    FakeDevice* const device_two = make_device(vtable_two);
    check_u(api_->ensure_hooks(as_device(device_two)), 1, "a second vtable is patched too");
    const LONG family_b_before = family_calls_[1][WD_INDEX_DRAW_PRIMITIVE];
    const LONG family_a_before = family_calls_[0][WD_INDEX_DRAW_PRIMITIVE];
    call_draw_primitive(device_two);
    check_u(family_calls_[1][WD_INDEX_DRAW_PRIMITIVE] - family_b_before, 1,
        "it forwards to ITS originals");
    check_u(family_calls_[0][WD_INDEX_DRAW_PRIMITIVE] - family_a_before, 0,
        "not to the first vtable's");

    // A fifth vtable is where the first draft stopped, at a cap of four.
    Method* const vtable_three = make_vtable(0);
    Method* const vtable_four = make_vtable(0);
    Method* const vtable_five = make_vtable(1);
    check_u(api_->ensure_hooks(as_device(make_device(vtable_three))), 1, "a third vtable fits");
    check_u(api_->ensure_hooks(as_device(make_device(vtable_four))), 1, "a fourth vtable fits");
    FakeDevice* const device_five = make_device(vtable_five);
    check_u(api_->ensure_hooks(as_device(device_five)), 1, "and so does a fifth: vtables are not capped");
    check_u(slots_from(vtable_five, daemon_a_), WD_SLOT_COUNT, "all six of its slots hold daemon code");
    const LONG five_before = family_calls_[1][WD_INDEX_DRAW_PRIMITIVE];
    call_draw_primitive(device_five);
    check_u(family_calls_[1][WD_INDEX_DRAW_PRIMITIVE] - five_before, 1,
        "and it forwards to its own originals");

    // Free four of them and let the liveness check reclaim the records.
    const WdDaemonStats before_drop = read_stats(api_);
    VirtualFree(vtable_two, 0, MEM_RELEASE);
    VirtualFree(vtable_three, 0, MEM_RELEASE);
    VirtualFree(vtable_four, 0, MEM_RELEASE);
    VirtualFree(vtable_five, 0, MEM_RELEASE);
    check_u(api_->check_slots(), 0, "check_slots reports no stomps");
    const WdDaemonStats after_drop = read_stats(api_);
    check_u(after_drop.vtables_dropped - before_drop.vtables_dropped, 4,
        "four vtables whose memory went away were dropped, not read");
}

// ---- registered sets -----------------------------------------------------

void phase_sets() {
    char path_a[MAX_PATH];
    char path_b[MAX_PATH];
    path_in(path_a, sizeof(path_a), "a\\fake_engine.dll");
    path_in(path_b, sizeof(path_b), "b\\fake_engine.dll");
    check(load_engine(path_a, &engine_a_), "fake engine a loads and exports its entry points");
    check(load_engine(path_b, &engine_b_), "fake engine b loads and exports its entry points");
    check(engine_a_.module != engine_b_.module, "two engine images under one basename stay separate");
    if (!engine_a_.module || !engine_b_.module) {
        return;
    }
    check_p(engine_a_.self(), engine_a_.module, "an image knows itself by address");

    for (int i = 0; i < 12; ++i) {
        std::memset(&controls_[i], 0, sizeof(controls_[i]));
        controls_[i].id = static_cast<uint32_t>(i + 1);
        controls_[i].log = &order_log_;
    }

    WdHandlerSet* const first = engine_a_.create_set(&controls_[0], WD_DAEMON_ABI, sizeof(WdHandlerSet));
    // A set from a LATER ABI: longer, and its first fields are still ours.
    WdHandlerSet* const second = engine_b_.create_set(&controls_[1], WD_DAEMON_ABI + 1,
        sizeof(WdHandlerSet) + 8);
    check(first != 0 && second != 0, "both engines build a handler set");
    if (!first || !second) {
        return;
    }

    check_u(api_->register_set(first), 1, "the first registration is index 1, never 0");
    check_u(api_->register_set(second), 2, "the second is index 2");
    check_u(api_->register_set(first), 1, "registering the same set again returns the same index");
    check_u(read_stats(api_).sets_live, 2, "two sets are live");

    WdHandlerSet stack_set;
    std::memset(&stack_set, 0, sizeof(stack_set));
    stack_set.abi_version = 0;
    stack_set.size = sizeof(WdHandlerSet);
    check_u(api_->register_set(&stack_set), 0, "a set with no ABI is refused");
    stack_set.abi_version = WD_DAEMON_ABI;
    stack_set.size = sizeof(WdHandlerSet) - 1;
    check_u(api_->register_set(&stack_set), 0, "a set shorter than the ABI's is refused");
    check_u(api_->register_set(0), 0, "a null set is refused");

    order_log_.count = 0;
    call_draw_primitive(device_one_);
    check_u(order_log_.count, 2, "both sets saw the draw");
    check_u(order_log_.ids[0], controls_[0].id, "sets run in registration order");
    check_u(order_log_.ids[1], controls_[1].id, "the later registration runs second");
    check_p(controls_[1].last_device, as_device(device_one_), "a handler is given the device");

    call_draw_indexed_primitive(device_one_);
    call_draw_primitive_up(device_one_);
    call_draw_indexed_primitive_up(device_one_);
    check_u(controls_[0].pre_draw, 4, "all four Draw slots reach pre_draw");
    check_u(controls_[1].pre_draw, 4, "for every registered set");

    call_set_render_target(device_one_);
    check_u(controls_[0].pre_set_render_target, 1, "SetRenderTarget reaches pre_set_render_target");
    check_u(controls_[1].pre_set_render_target, 1, "for every registered set");

    const HRESULT reset_result = call_reset(device_one_);
    check_u(controls_[0].pre_reset, 1, "Reset reaches pre_reset");
    check_u(controls_[0].post_reset, 1, "and post_reset");
    check_u(static_cast<unsigned long>(controls_[0].last_reset_result),
        static_cast<unsigned long>(reset_result), "post_reset carries the original's result");

    // Eight, and then the ninth -- where the first draft refused.
    WdHandlerSet* extra[7];
    for (int i = 0; i < 6; ++i) {
        extra[i] = engine_a_.create_set(&controls_[2 + i], WD_DAEMON_ABI, sizeof(WdHandlerSet));
        check_u(api_->register_set(extra[i]), static_cast<unsigned long>(3 + i),
            "each registration gets the next free 1-based index");
    }
    check_u(read_stats(api_).sets_live, 8, "eight sets are live");

    extra[6] = engine_a_.create_set(&controls_[8], WD_DAEMON_ABI, sizeof(WdHandlerSet));
    check_u(api_->register_set(extra[6]), 9, "the ninth concurrent set registers: sets are not capped");
    check_u(read_stats(api_).sets_live, 9, "nine sets are live");

    order_log_.count = 0;
    call_draw_primitive(device_one_);
    check_u(order_log_.count, 9, "all nine sets see a draw");

    for (int i = 0; i < 7; ++i) {
        api_->unregister_set(extra[i]);
        engine_a_.destroy_set(extra[i]);
    }
    const WdDaemonStats stats = read_stats(api_);
    check_u(stats.sets_live, 2, "unregistering leaves the rest alone");
    check_u(stats.sets_registered, 9, "the cumulative count keeps counting");

    order_log_.count = 0;
    call_draw_primitive(device_one_);
    check_u(order_log_.count, 2, "and the freed slots are not visited any more");
}

// ---- Reset's two independent loops ---------------------------------------

void register_late_set() {
    if (late_set_) {
        api_->register_set(late_set_);
    }
}

void phase_reset_loops() {
    late_set_ = engine_a_.create_set(&controls_[9], WD_DAEMON_ABI, sizeof(WdHandlerSet));
    check(late_set_ != 0, "a set to register mid-Reset");
    if (!late_set_) {
        return;
    }

    const LONG pre_before = controls_[0].pre_reset;
    const LONG post_before = controls_[0].post_reset;

    on_reset_ = &register_late_set;
    const HRESULT result = call_reset(device_one_);
    on_reset_ = 0;

    check_u(controls_[9].pre_reset, 0, "a set that registers inside Reset gets no pre_reset");
    check_u(controls_[9].post_reset, 1, "it gets post_reset: the two loops are independent");
    check_u(static_cast<unsigned long>(controls_[9].last_reset_result),
        static_cast<unsigned long>(result), "with the real result");
    check_u(controls_[0].pre_reset - pre_before, 1, "the sets already registered got their pre_reset");
    check_u(controls_[0].post_reset - post_before, 1, "and their post_reset");

    api_->unregister_set(late_set_);
    engine_a_.destroy_set(late_set_);
    late_set_ = 0;
}

// ---- the drain -----------------------------------------------------------

volatile LONG drain_release_tick_ = 0;
FeControl* drain_control_ = 0;

DWORD WINAPI drain_draw_thread(LPVOID) {
    call_draw_primitive(device_one_);
    return 0;
}

DWORD WINAPI drain_release_thread(LPVOID) {
    Sleep(250);
    InterlockedExchange(&drain_release_tick_, static_cast<LONG>(GetTickCount()));
    InterlockedExchange(&drain_control_->park, 0);
    return 0;
}

void phase_drain() {
    WdHandlerSet* const parked = engine_b_.create_set(&controls_[10], WD_DAEMON_ABI, sizeof(WdHandlerSet));
    check(parked != 0, "a set whose handler can be parked");
    if (!parked) {
        return;
    }

    drain_control_ = &controls_[10];
    check(api_->register_set(parked) != 0, "it registers");
    InterlockedExchange(&drain_control_->park, 1);

    HANDLE drawer = CreateThread(NULL, 0, &drain_draw_thread, 0, 0, NULL);
    check(drawer != NULL, "a second thread can drive the vtable");
    if (!drawer) {
        return;
    }

    DWORD waited = 0;
    while (!drain_control_->parked && waited < 5000) {
        Sleep(5);
        waited += 5;
    }
    check(drain_control_->parked != 0, "the second thread is inside the handler");

    HANDLE releaser = CreateThread(NULL, 0, &drain_release_thread, 0, 0, NULL);
    const DWORD started = GetTickCount();
    api_->unregister_set(parked);
    const DWORD returned = GetTickCount();

    check(drain_control_->park_left != 0, "the handler had left before unregister_set returned");
    check(returned - started >= 150, "unregister_set waited for it rather than returning early");
    check(static_cast<LONG>(returned) >= drain_release_tick_, "it returned after the handler was released");

    WaitForSingleObject(drawer, 5000);
    CloseHandle(drawer);
    if (releaser) {
        WaitForSingleObject(releaser, 5000);
        CloseHandle(releaser);
    }
    engine_b_.destroy_set(parked);
    drain_control_ = 0;
}

// ---- unregister, then take the image away --------------------------------

void phase_unregister_then_free() {
    char path_c[MAX_PATH];
    path_in(path_c, sizeof(path_c), "c\\fake_engine.dll");

    Engine engine_c;
    check(load_engine(path_c, &engine_c), "a third engine image loads");
    if (!engine_c.module) {
        return;
    }
    check(engine_c.module != engine_a_.module && engine_c.module != engine_b_.module,
        "three copies of one file in three directories are three images");

    WdHandlerSet* const set = engine_c.create_set(&controls_[11], WD_DAEMON_ABI, sizeof(WdHandlerSet));
    check(set != 0, "it builds a set");
    if (!set) {
        return;
    }

    const void* const handler_code =
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(set->pre_draw));
    check(api_->register_set(set) != 0, "it registers");
    call_draw_primitive(device_one_);
    check_u(controls_[11].pre_draw, 1, "its handler ran");

    api_->unregister_set(set);

    // The set's page is made unreadable and the image it points into is
    // unmapped. Anything the daemon still held would fault on the next call.
    engine_c.seal_set(set);
    check(FreeLibrary(engine_c.module) != FALSE, "the engine image unloads");
    check(module_of(handler_code) == NULL, "it really unmapped: no module owns its handlers now");

    for (int i = 0; i < 200; ++i) {
        call_draw_primitive(device_one_);
        call_draw_indexed_primitive(device_one_);
        call_draw_primitive_up(device_one_);
        call_draw_indexed_primitive_up(device_one_);
        call_set_render_target(device_one_);
        call_reset(device_one_);
    }

    check(true, "1200 calls after the unregister touched neither the sealed set nor the freed image");
    check_u(controls_[11].pre_draw, 1, "the unregistered set never ran again");
}

// ---- refusals: the trampoline and the unpatchable slot -------------------

Method make_trampoline() {
    unsigned char* const code = static_cast<unsigned char*>(VirtualAlloc(NULL, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) {
        return 0;
    }
    code[0] = 0xC3;
    return reinterpret_cast<Method>(reinterpret_cast<uintptr_t>(code));
}

void phase_refusals() {
    uintptr_t* const import = find_import(daemon_a_, "KERNEL32.dll", "VirtualProtect");
    check(import != 0, "the daemon's VirtualProtect import can be watched");
    uintptr_t original_import = 0;
    if (import) {
        check(write_import(import, reinterpret_cast<uintptr_t>(&watching_virtual_protect), &original_import),
            "the watcher is installed");
        real_virtual_protect_ = reinterpret_cast<VirtualProtectMethod>(original_import);
    }

    // A slot pointing at code in no module at all: an allocated trampoline.
    Method* const trampolined = make_vtable(0);
    trampolined[WD_SLOT_DRAW_PRIMITIVE] = make_trampoline();
    FakeDevice* const device = make_device(trampolined);
    VtableSnapshot before;
    snapshot_vtable(trampolined, &before);

    const WdDaemonStats before_refusal = read_stats(api_);
    protect_log_count_ = 0;
    check_u(api_->ensure_hooks(as_device(device)), 0, "a slot held by an allocated trampoline is refused");
    const WdDaemonStats after_refusal = read_stats(api_);
    check_u(after_refusal.install_refused_trampoline - before_refusal.install_refused_trampoline, 1,
        "and counted");
    check_u(changed_slots(trampolined, before), 0, "the rollback left zero slots patched");
    check_u(device->refcount, 0, "a refused install takes no reference");

    // The two slots before the trampoline were patched and put back: the vtable
    // cannot show that afterwards, but the daemon's own VirtualProtect calls can.
    check_u(protect_log_count_, 8, "two slots were patched, then both were unpatched");
    if (protect_log_count_ == 8) {
        const void* const reset_slot = &trampolined[WD_SLOT_RESET];
        const void* const target_slot = &trampolined[WD_SLOT_SET_RENDER_TARGET];
        const bool ordered = protect_log_[0] == reset_slot && protect_log_[1] == reset_slot
            && protect_log_[2] == target_slot && protect_log_[3] == target_slot
            && protect_log_[4] == reset_slot && protect_log_[5] == reset_slot
            && protect_log_[6] == target_slot && protect_log_[7] == target_slot;
        check(ordered, "in slot order, and rolled back in the same order");
    }

    // The record was not consumed by the refusal.
    trampolined[WD_SLOT_DRAW_PRIMITIVE] = reinterpret_cast<Method>(&a_draw_primitive);
    check_u(api_->ensure_hooks(as_device(device)), 1, "with the trampoline gone the same vtable installs");

    // A slot that cannot be written at all, mid-sequence.
    Method* const unpatchable = make_vtable(0);
    FakeDevice* const stubborn = make_device(unpatchable);
    VtableSnapshot stubborn_before;
    snapshot_vtable(unpatchable, &stubborn_before);
    protect_fail_address_ = &unpatchable[WD_SLOT_DRAW_PRIMITIVE];
    protect_log_count_ = 0;
    const WdDaemonStats before_unpatchable = read_stats(api_);
    check_u(api_->ensure_hooks(as_device(stubborn)), 0, "a slot that cannot be protected refuses the install");
    protect_fail_address_ = 0;
    check_u(changed_slots(unpatchable, stubborn_before), 0, "and leaves zero slots patched");
    check_u(read_stats(api_).install_refused_trampoline
        - before_unpatchable.install_refused_trampoline, 0, "which is not a trampoline refusal");
    check_u(api_->ensure_hooks(as_device(stubborn)), 1, "that vtable installs once the write succeeds");

    if (import) {
        uintptr_t ignored = 0;
        write_import(import, original_import, &ignored);
    }
}

// ---- stomps and liveness -------------------------------------------------

void phase_stomps() {
    check_u(api_->check_slots(), 0, "nothing is stomped yet");

    Method previous = 0;
    DWORD old_protect = 0;
    VirtualProtect(&vtable_one_[WD_SLOT_DRAW_PRIMITIVE], sizeof(Method), PAGE_READWRITE, &old_protect);
    previous = vtable_one_[WD_SLOT_DRAW_PRIMITIVE];
    vtable_one_[WD_SLOT_DRAW_PRIMITIVE] = reinterpret_cast<Method>(&foreign_draw_primitive);
    DWORD ignored = 0;
    VirtualProtect(&vtable_one_[WD_SLOT_DRAW_PRIMITIVE], sizeof(Method), old_protect, &ignored);

    check_u(api_->check_slots(), 1, "a slot taken by a foreign hook is detected");
    check_p(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(vtable_one_[WD_SLOT_DRAW_PRIMITIVE])),
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(
            reinterpret_cast<Method>(&foreign_draw_primitive))),
        "and left alone: the daemon never re-chains");
    check(read_stats(api_).stomps_detected >= 1, "the stomp is counted");
    check_u(api_->check_slots(), 1, "checking again reports the same thing and still repairs nothing");

    // A retained vtable whose memory has gone. The liveness re-check must drop
    // it rather than read it.
    Method* const doomed = make_vtable(0);
    check_u(api_->ensure_hooks(as_device(make_device(doomed))), 1, "one more vtable to lose");
    const WdDaemonStats before_drop = read_stats(api_);
    VirtualFree(doomed, 0, MEM_RELEASE);
    check_u(api_->check_slots(), 1, "check_slots survives a vtable that has been freed");
    check_u(read_stats(api_).vtables_dropped - before_drop.vtables_dropped, 1, "and drops it");

    vtable_one_[WD_SLOT_DRAW_PRIMITIVE] = previous;
}

// ---- a live vtable the liveness probe used to condemn --------------------
// The two failures this phase exists for are the same one twice. A vtable that
// is MAPPED and still holds our thunks can fail a liveness probe that judges it
// by the CONTENT of its slots: a foreign hooker that took one slot and then
// unmapped leaves an address that is readable to point at but is not executable
// code. Condemning the record for that:
//
//   (a) stops original_for finding the originals for a vtable our thunks are
//       still installed in, so every one of them returns D3DERR_INVALIDCALL,
//       the client renders nothing for the rest of the session and a lost
//       device can never be recovered because Reset never reaches d3d8;
//   (b) frees the record, after which an install on the same vtable finds our
//       own thunks in the slots, saves THEM as the originals -- they are
//       executable code in a pinned module, so every test passes -- and the
//       next draw call is a thunk forwarding to itself.
//
// So the phase keeps a vtable in exactly that state and asks for frames.

void phase_live_vtable_kept() {
    Method* const patched = make_vtable(1);
    FakeDevice* const owner = patched ? make_device(patched) : 0;
    check(owner != 0, "a vtable that will keep our thunks while a slot goes bad");
    if (!owner) {
        return;
    }

    check_u(api_->ensure_hooks(as_device(owner)), 1, "it installs");
    check_u(slots_from(patched, daemon_a_), WD_SLOT_COUNT, "all six slots hold daemon code");
    const Method our_thunk = patched[WD_SLOT_DRAW_PRIMITIVE_UP];

    // Readable and NOT executable: what a foreign hook's slot points at once
    // its image has gone. DrawPrimitiveUP is the slot it takes here so that
    // DrawPrimitive, SetRenderTarget and Reset -- the three that stop the client
    // dead -- can each be asked for a frame afterwards.
    void* const data_page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(data_page != 0, "a page of data to point a departed hook's slot at");
    if (!data_page) {
        return;
    }
    MEMORY_BASIC_INFORMATION page;
    std::memset(&page, 0, sizeof(page));
    const bool queried = VirtualQuery(data_page, &page, sizeof(page)) != 0;
    check(queried && page.State == MEM_COMMIT && (page.Protect & 0xFF) == PAGE_READWRITE,
        "committed and readable, but not executable: the state that used to condemn the record");
    patched[WD_SLOT_DRAW_PRIMITIVE_UP] = reinterpret_cast<Method>(data_page);

    const WdDaemonStats before = read_stats(api_);
    check_u(api_->check_slots(), 1, "the slot that is no longer ours is reported as a stomp");
    const WdDaemonStats after = read_stats(api_);
    check_u(after.vtables_dropped - before.vtables_dropped, 0,
        "and the record is KEPT: the vtable is readable, so our thunks are still reachable through it");

    // The frames. Each of these would be D3DERR_INVALIDCALL with the record
    // dropped, and the client would never draw again.
    const LONG reset_before = family_calls_[1][WD_INDEX_RESET];
    check_u(static_cast<unsigned long>(call_reset(owner)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_RESET),
        "Reset still reaches d3d8, so a lost device can still be recovered");
    check_u(family_calls_[1][WD_INDEX_RESET] - reset_before, 1, "exactly once");

    const LONG target_before = family_calls_[1][WD_INDEX_SET_RENDER_TARGET];
    check_u(static_cast<unsigned long>(call_set_render_target(owner)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_SET_RENDER_TARGET),
        "SetRenderTarget still reaches d3d8");
    check_u(family_calls_[1][WD_INDEX_SET_RENDER_TARGET] - target_before, 1, "exactly once");

    const LONG draw_before = family_calls_[1][WD_INDEX_DRAW_PRIMITIVE];
    check_u(static_cast<unsigned long>(call_draw_primitive(owner)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_DRAW_PRIMITIVE),
        "DrawPrimitive still reaches d3d8: the game still renders");
    check_u(family_calls_[1][WD_INDEX_DRAW_PRIMITIVE] - draw_before, 1, "exactly once");

    // (b) on the same vtable: the record is still there, so ensure_hooks
    // re-adopts it and reads no slot as an original.
    VtableSnapshot before_ensure;
    snapshot_vtable(patched, &before_ensure);
    check_u(api_->ensure_hooks(as_device(owner)), 1,
        "ensure_hooks re-adopts the record that holds the real originals");
    check_u(changed_slots(patched, before_ensure), 0, "having re-patched nothing");
    check_u(owner->refcount, 0, "and called no method of the device to do it");
    const LONG after_ensure = family_calls_[1][WD_INDEX_DRAW_PRIMITIVE];
    check_u(static_cast<unsigned long>(call_draw_primitive(owner)),
        static_cast<unsigned long>(original_result_ + WD_INDEX_DRAW_PRIMITIVE),
        "and a draw after it still reaches the TRUE original");
    check_u(family_calls_[1][WD_INDEX_DRAW_PRIMITIVE] - after_ensure, 1,
        "exactly once: no thunk was saved as an original, so nothing recursed");

    // Zero is the value a dropped record carries, and by now the table holds
    // several. A device whose vtable pointer is zero must therefore match none
    // of them: matching would hand a caller the originals of a vtable that no
    // longer exists. The thunk is called directly, because no vtable can
    // dispatch a call for a device that has none.
    const Method draw_thunk = patched[WD_SLOT_DRAW_PRIMITIVE];
    check_p(module_of_method(draw_thunk), daemon_a_, "the thunk to call is the daemon's");
    FakeDevice vtable_less;
    std::memset(&vtable_less, 0, sizeof(vtable_less));
    const LONG zero_a = family_calls_[0][WD_INDEX_DRAW_PRIMITIVE];
    const LONG zero_b = family_calls_[1][WD_INDEX_DRAW_PRIMITIVE];
    check_u(static_cast<unsigned long>(reinterpret_cast<DrawPrimitiveMethod>(draw_thunk)(
            as_device(&vtable_less), D3DPT_TRIANGLELIST, 0, 1)),
        static_cast<unsigned long>(D3DERR_INVALIDCALL),
        "a device with no vtable at all matches no record and is refused");
    check_u(family_calls_[0][WD_INDEX_DRAW_PRIMITIVE] - zero_a, 0,
        "reaching none of the originals a dropped record still carries");
    check_u(family_calls_[1][WD_INDEX_DRAW_PRIMITIVE] - zero_b, 0, "in either family");

    // ---- install on a vtable that already holds our thunks ----------------
    // A vtable no record claims, with our six thunks in it: what a wrapper that
    // COPIES a device's vtable hands out, and what a record dropped out from
    // under a patched vtable used to leave behind. Nothing in it may ever be
    // saved as an original.
    Method* const copy = static_cast<Method*>(VirtualAlloc(NULL,
        sizeof(Method) * WD_DEVICE_VTABLE_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FakeDevice* const copy_owner = copy ? make_device(copy) : 0;
    check(copy_owner != 0, "a copy of a patched vtable, at an address no record knows");
    if (!copy_owner) {
        return;
    }

    std::memcpy(copy, patched, sizeof(Method) * WD_DEVICE_VTABLE_SLOTS);
    copy[WD_SLOT_DRAW_PRIMITIVE_UP] = our_thunk;
    check_u(slots_from(copy, daemon_a_), WD_SLOT_COUNT, "it holds our thunks in all six slots");

    VtableSnapshot before_copy;
    snapshot_vtable(copy, &before_copy);
    const WdDaemonStats before_refusal = read_stats(api_);
    check_u(api_->ensure_hooks(as_device(copy_owner)), 0,
        "install refuses a vtable whose slots already hold our own thunks");
    check_u(changed_slots(copy, before_copy), 0, "having written nothing into it");
    check_u(copy_owner->refcount, 0, "and taken no reference");
    check_u(read_stats(api_).install_refused_trampoline
        - before_refusal.install_refused_trampoline, 0, "which is not a trampoline refusal");

    // Nothing claims that copy, so a call through it is an honest refusal. The
    // proof that no thunk was saved as an original is that this RETURNS: with
    // one saved, original_for hands the thunk back to itself and the call never
    // reaches an original, on this frame or any other.
    const LONG copy_before = family_calls_[1][WD_INDEX_DRAW_INDEXED_PRIMITIVE];
    check_u(static_cast<unsigned long>(call_draw_indexed_primitive(copy_owner)),
        static_cast<unsigned long>(D3DERR_INVALIDCALL),
        "a call through an unclaimed thunk refuses rather than forwarding to itself");
    check_u(family_calls_[1][WD_INDEX_DRAW_INDEXED_PRIMITIVE] - copy_before, 0,
        "and reached no original at all");

    const LONG real_before = family_calls_[1][WD_INDEX_DRAW_INDEXED_PRIMITIVE];
    call_draw_indexed_primitive(owner);
    check_u(family_calls_[1][WD_INDEX_DRAW_INDEXED_PRIMITIVE] - real_before, 1,
        "and the vtable that IS claimed still forwards exactly once to the true original");

    // Give the slot back, so the phases after this one start from no stomps.
    patched[WD_SLOT_DRAW_PRIMITIVE_UP] = our_thunk;
    check_u(api_->check_slots(), 0,
        "with the stomped slot returned, the record that was never dropped reports clean");
}

// ---- growth: past the old caps, and under a reader -----------------------
// The daemon pins itself for the life of the client, so a limit it ships with
// can never be raised for a player who already has it. These are the checks
// that there is no limit left to hit: sets and vtables grow on demand, a slot
// that was given up is reused rather than grown past, a growth that cannot
// allocate is a refusal, and -- the one that matters -- a render thread already
// walking the old table when it grows finishes safely and still gates the drain.

const int growth_pool_ = 128;
FeControl growth_controls_[growth_pool_];
WdHandlerSet* growth_sets_[growth_pool_];
unsigned long growth_index_[growth_pool_];
FeControl growth_parked_;

volatile LONG growth_release_tick_ = 0;
FeControl* growth_parked_control_ = 0;

DWORD WINAPI growth_draw_thread(LPVOID) {
    call_draw_primitive(device_one_);
    return 0;
}

DWORD WINAPI growth_release_thread(LPVOID) {
    Sleep(250);
    InterlockedExchange(&growth_release_tick_, static_cast<LONG>(GetTickCount()));
    InterlockedExchange(&growth_parked_control_->park, 0);
    return 0;
}

bool make_growth_set(int which) {
    if (which < 0 || which >= growth_pool_) {
        return false;
    }
    if (!growth_sets_[which]) {
        growth_sets_[which] = engine_a_.create_set(&growth_controls_[which], WD_DAEMON_ABI,
            sizeof(WdHandlerSet));
    }
    return growth_sets_[which] != 0;
}

// Registers pool sets one at a time until one is refused, and answers with its
// pool index. The boundary is FOUND rather than assumed: with every allocation
// refused, a registration succeeds while a free slot remains, and the first one
// that needs the table to grow is the one that fails.
int fill_to_boundary(int from, int limit) {
    InterlockedExchange(&alloc_fail_all_, 1);
    int at = from;
    int refused = -1;
    while (at < limit && refused < 0) {
        if (!make_growth_set(at)) {
            break;
        }
        if (api_->register_set(growth_sets_[at]) == 0) {
            refused = at;
        }
        ++at;
    }
    InterlockedExchange(&alloc_fail_all_, 0);
    return refused;
}

void phase_growth() {
    for (int i = 0; i < growth_pool_; ++i) {
        std::memset(&growth_controls_[i], 0, sizeof(growth_controls_[i]));
        growth_controls_[i].id = static_cast<uint32_t>(1000 + i);
        growth_controls_[i].log = &order_log_;
        growth_sets_[i] = 0;
        growth_index_[i] = 0;
    }

    check(install_alloc_watcher(daemon_a_), "the daemon's HeapAlloc import can be watched");
    if (!heap_alloc_import_) {
        return;
    }

    // Two sets are live from phase_sets, at slots 0 and 1, so pool set k lands
    // in slot k + 2 for as long as nothing is unregistered.
    const int live_before = 2;

    // ---- an allocation that fails is a refusal, never a crash -------------
    const int first_refused = fill_to_boundary(0, 64);
    check(first_refused >= 0, "with every allocation refused, the first registration that needs a "
        "bigger table is refused");
    if (first_refused < 0) {
        remove_alloc_watcher();
        return;
    }

    check_u(read_stats(api_).sets_live, static_cast<unsigned long>(live_before + first_refused),
        "the refusal published nothing: sets_live counts only the ones that registered");
    order_log_.count = 0;
    call_draw_primitive(device_one_);
    check_u(order_log_.count, static_cast<unsigned long>(live_before + first_refused),
        "and a draw visits exactly those, walking no half-built table");

    // Every growth in this phase is counted the same way: a registration that
    // allocated is one that grew the table, and one that did not reused a slot.
    int growths = 0;
    take_allocations();
    const uint32_t after_first = api_->register_set(growth_sets_[first_refused]);
    if (take_allocations() > 0) {
        ++growths;
    }
    check_u(after_first, static_cast<unsigned long>(live_before + first_refused + 1),
        "the same set registers as soon as the allocation succeeds");

    // ---- and a growth that fails PART WAY is thrown away whole ------------
    const int second_refused = fill_to_boundary(first_refused + 1, 96);
    check(second_refused >= 0, "the next boundary refuses the same way");
    if (second_refused < 0) {
        remove_alloc_watcher();
        return;
    }

    InterlockedExchange(&alloc_fail_at_, 3);
    take_allocations();
    const uint32_t partial = api_->register_set(growth_sets_[second_refused]);
    const LONG partial_allocations = take_allocations();
    InterlockedExchange(&alloc_fail_at_, 0);
    check_u(partial, 0, "a growth that fails after its array is allocated is refused too");
    check(partial_allocations >= 3, "and it really did get past the array before it failed");
    check_u(read_stats(api_).sets_live, static_cast<unsigned long>(live_before + second_refused),
        "the half-built table was never published");

    take_allocations();
    const uint32_t after_second = api_->register_set(growth_sets_[second_refused]);
    if (take_allocations() > 0) {
        ++growths;
    }
    check_u(after_second, static_cast<unsigned long>(live_before + second_refused + 1),
        "the set registers on the next try");

    // ---- 48 more, far past the eight the first draft allowed --------------
    const int base = second_refused + 1;
    const int many = 48;
    int ordered_indices = 0;
    for (int i = base; i < base + many; ++i) {
        if (!make_growth_set(i)) {
            break;
        }
        take_allocations();
        growth_index_[i] = api_->register_set(growth_sets_[i]);
        if (take_allocations() > 0) {
            ++growths;
        }
        if (growth_index_[i] == static_cast<unsigned long>(live_before + i + 1)) {
            ++ordered_indices;
        }
    }
    check_u(static_cast<unsigned long>(ordered_indices), static_cast<unsigned long>(many),
        "48 registrations past the old cap each get the next 1-based index");
    check(growths >= 2, "and the slot table grew more than once getting there");
    check_u(read_stats(api_).sets_live, static_cast<unsigned long>(live_before + base + many),
        "every one of them is live");

    order_log_.count = 0;
    call_draw_primitive(device_one_);
    check_u(order_log_.count, static_cast<unsigned long>(live_before + base + many),
        "one draw reaches every registered set");
    int in_order = 0;
    for (int i = 0; i < base + many; ++i) {
        if (order_log_.ids[live_before + i] == growth_controls_[i].id) {
            ++in_order;
        }
    }
    check_u(static_cast<unsigned long>(in_order), static_cast<unsigned long>(base + many),
        "in registration order, across every table the growth left behind");

    // ---- churn reuses slots rather than growing past them -----------------
    for (int i = base; i < base + many; ++i) {
        api_->unregister_set(growth_sets_[i]);
    }
    check_u(read_stats(api_).sets_live, static_cast<unsigned long>(live_before + base),
        "unregistering 48 leaves the rest alone");

    take_allocations();
    int reused = 0;
    for (int i = base; i < base + many; ++i) {
        if (api_->register_set(growth_sets_[i]) == growth_index_[i]) {
            ++reused;
        }
    }
    const LONG churn_allocations = take_allocations();
    check_u(static_cast<unsigned long>(reused), static_cast<unsigned long>(many),
        "re-registering the same 48 sets hands back the same 48 slots");
    check_u(static_cast<unsigned long>(churn_allocations), 0,
        "and allocates nothing: an unregistered slot is reused in place");

    for (int i = 0; i < base + many; ++i) {
        api_->unregister_set(growth_sets_[i]);
    }
    check_u(read_stats(api_).sets_live, static_cast<unsigned long>(live_before),
        "and all of them can be given back");

    // ---- a reader parked on the OLD table while it grows under it ---------
    // Its own control, not one of controls_[]: phase_drain reads those later and
    // a counter left behind here would make its checks pass on old news.
    FeControl* const parked_control = &growth_parked_;
    std::memset(parked_control, 0, sizeof(*parked_control));
    parked_control->id = 777;
    parked_control->log = &order_log_;
    WdHandlerSet* const parked = engine_b_.create_set(parked_control, WD_DAEMON_ABI, sizeof(WdHandlerSet));
    check(parked != 0, "a set whose handler parks a render thread mid-dispatch");
    if (!parked) {
        remove_alloc_watcher();
        return;
    }

    growth_parked_control_ = parked_control;
    check_u(api_->register_set(parked), static_cast<unsigned long>(live_before + 1),
        "it takes the lowest free slot");
    InterlockedExchange(&parked_control->park, 1);

    order_log_.count = 0;
    HANDLE drawer = CreateThread(NULL, 0, &growth_draw_thread, 0, 0, NULL);
    check(drawer != NULL, "a second thread can drive the vtable");
    if (!drawer) {
        remove_alloc_watcher();
        return;
    }

    DWORD waited = 0;
    while (!parked_control->parked && waited < 5000) {
        Sleep(5);
        waited += 5;
    }
    check(parked_control->parked != 0, "that thread is inside the handler, holding the table it took");

    int grew_under = 0;
    int registered_under = 0;
    for (int i = 0; i < growth_pool_; ++i) {
        if (!make_growth_set(i)) {
            break;
        }
        take_allocations();
        if (api_->register_set(growth_sets_[i]) != 0) {
            ++registered_under;
        }
        if (take_allocations() > 0) {
            ++grew_under;
        }
    }
    check_u(static_cast<unsigned long>(registered_under), static_cast<unsigned long>(growth_pool_),
        "the whole pool registers while that thread is parked");
    check(grew_under >= 1, "which grew the table out from under it at least once");
    check(parked_control->parked != 0 && parked_control->park_left == 0,
        "and it is still sitting in the handler, on the table it took");

    // The drain has to wait for it. Its slot is the same OBJECT in the new
    // table as in the old one, which is the whole reason growth is allowed to
    // happen with no lock on the reading side.
    HANDLE releaser = CreateThread(NULL, 0, &growth_release_thread, 0, 0, NULL);
    const DWORD started = GetTickCount();
    api_->unregister_set(parked);
    const DWORD returned = GetTickCount();

    check(parked_control->park_left != 0, "the parked handler had left before unregister_set returned");
    check(returned - started >= 150, "unregister_set waited on the inflight count it raised on the old table");
    check(static_cast<LONG>(returned) >= growth_release_tick_, "it returned after the handler was released");

    WaitForSingleObject(drawer, 5000);
    CloseHandle(drawer);
    if (releaser) {
        WaitForSingleObject(releaser, 5000);
        CloseHandle(releaser);
    }

    check_u(order_log_.count, static_cast<unsigned long>(live_before + 1),
        "the parked call walked exactly the snapshot it took and saw none of the sets added under it");
    check_u(parked_control->pre_draw, 1, "it ran its handler once and completed");

    order_log_.count = 0;
    call_draw_primitive(device_one_);
    check_u(order_log_.count, static_cast<unsigned long>(live_before + growth_pool_),
        "and the next draw reaches every set on the grown table");

    for (int i = 0; i < growth_pool_; ++i) {
        api_->unregister_set(growth_sets_[i]);
        engine_a_.destroy_set(growth_sets_[i]);
        growth_sets_[i] = 0;
    }
    engine_b_.destroy_set(parked);
    growth_parked_control_ = 0;
    check_u(read_stats(api_).sets_live, static_cast<unsigned long>(live_before),
        "and the pool goes back, leaving the two sets phase_sets left");

    std::printf("growth : %d slot-table growths, %d sets at the peak, %d more under a parked reader\n",
        growths, live_before + growth_pool_, grew_under);
    remove_alloc_watcher();
}

// ---- many vtables --------------------------------------------------------

void phase_many_vtables() {
    check(install_alloc_watcher(daemon_a_), "the HeapAlloc import can be watched again");
    if (!heap_alloc_import_) {
        return;
    }

    const int many = 16;
    Method* vtables[16];
    FakeDevice* owners[16];
    int installed = 0;
    int grew = 0;
    for (int i = 0; i < many; ++i) {
        vtables[i] = make_vtable(i % 2);
        owners[i] = vtables[i] ? make_device(vtables[i]) : 0;
        if (!owners[i]) {
            break;
        }
        take_allocations();
        if (api_->ensure_hooks(as_device(owners[i])) == 1) {
            ++installed;
        }
        if (take_allocations() > 0) {
            ++grew;
        }
    }
    check_u(static_cast<unsigned long>(installed), static_cast<unsigned long>(many),
        "sixteen vtables are patched, four times what the first draft retained");
    check(grew >= 2, "and the vtable table grew more than once to hold them");

    int patched = 0;
    int forwarded = 0;
    for (int i = 0; i < many; ++i) {
        if (slots_from(vtables[i], daemon_a_) == WD_SLOT_COUNT) {
            ++patched;
        }

        const int family = i % 2;
        const LONG mine = family_calls_[family][WD_INDEX_DRAW_PRIMITIVE];
        const LONG theirs = family_calls_[1 - family][WD_INDEX_DRAW_PRIMITIVE];
        call_draw_primitive(owners[i]);
        if (family_calls_[family][WD_INDEX_DRAW_PRIMITIVE] - mine == 1
            && family_calls_[1 - family][WD_INDEX_DRAW_PRIMITIVE] - theirs == 0
            && owners[i]->calls[WD_INDEX_DRAW_PRIMITIVE] == 1) {
            ++forwarded;
        }
    }
    check_u(static_cast<unsigned long>(patched), static_cast<unsigned long>(many),
        "each of them holds daemon code in all six slots");
    check_u(static_cast<unsigned long>(forwarded), static_cast<unsigned long>(many),
        "and each forwards to ITS OWN originals, not to another record's");

    // A liveness drop among many: five of the sixteen lose their memory. The
    // records are cleared in place, so the eleven that are left keep working
    // and keep the addresses they had.
    const WdDaemonStats before_drop = read_stats(api_);
    for (int i = 0; i < many; i += 3) {
        VirtualFree(vtables[i], 0, MEM_RELEASE);
        vtables[i] = 0;
    }
    check_u(api_->check_slots(), 0, "check_slots survives six dead vtables among sixteen");
    check_u(read_stats(api_).vtables_dropped - before_drop.vtables_dropped, 6,
        "and drops exactly the six that went away");

    int survivors = 0;
    for (int i = 0; i < many; ++i) {
        if (!vtables[i]) {
            continue;
        }

        const int family = i % 2;
        const LONG mine = family_calls_[family][WD_INDEX_DRAW_PRIMITIVE];
        call_draw_primitive(owners[i]);
        if (family_calls_[family][WD_INDEX_DRAW_PRIMITIVE] - mine == 1) {
            ++survivors;
        }
    }
    check_u(static_cast<unsigned long>(survivors), 10, "the ten survivors still forward correctly");

    Method* const reuse = make_vtable(0);
    FakeDevice* const reuse_device = reuse ? make_device(reuse) : 0;
    take_allocations();
    check_u(reuse_device ? api_->ensure_hooks(as_device(reuse_device)) : 0, 1,
        "a new vtable installs into a dropped record");
    check_u(static_cast<unsigned long>(take_allocations()), 0, "reusing one allocates nothing");

    // ensure_hooks refuses the same way register_set does, and patches nothing.
    InterlockedExchange(&alloc_fail_all_, 1);
    int refused = -1;
    int patched_on_refusal = -1;
    for (int i = 0; i < 64 && refused < 0; ++i) {
        Method* const candidate = make_vtable(0);
        FakeDevice* const device = candidate ? make_device(candidate) : 0;
        if (!device) {
            break;
        }

        VtableSnapshot before;
        snapshot_vtable(candidate, &before);
        if (api_->ensure_hooks(as_device(device)) == 0) {
            refused = i;
            patched_on_refusal = changed_slots(candidate, before);
        }
    }
    InterlockedExchange(&alloc_fail_all_, 0);
    check(refused >= 0, "ensure_hooks refuses when the vtable table cannot grow");
    check_u(static_cast<unsigned long>(patched_on_refusal), 0,
        "and that refusal patched nothing at all");

    Method* const after = make_vtable(0);
    FakeDevice* const after_device = after ? make_device(after) : 0;
    check_u(after_device ? api_->ensure_hooks(as_device(after_device)) : 0, 1,
        "the next install succeeds once the allocation does");
    check_u(after ? slots_from(after, daemon_a_) : 0, WD_SLOT_COUNT,
        "with all six of its slots patched");

    std::printf("vtables: %d patched, %d table growths, %d dropped live\n", installed, grew, 6);
    remove_alloc_watcher();
}

// ---- the thread gate -----------------------------------------------------
// The daemon's entry points are called from the ENGINE's thread, and the engine
// attaches at plugin load and retries in PostRender -- the game's main thread,
// which is not the thread the hooks run on. The device may not be touched from
// there. Every other phase in this file drives the vtable from the main thread
// because a single-threaded driver is the simplest way to test forwarding; this
// one is the opposite, and it is the only one whose thread layout is the point:
//
//   main thread    ensure_hooks x3, register_set, unregister_set, check_slots,
//                  stats -- everything an engine ever calls
//   render thread  every call THROUGH the device, on a thread of its own that
//                  is joined before the main thread asserts anything
//
// Then every device call recorded in the window must have come from the render
// thread. The last thing the phase does is make the violation itself and
// require the gate to catch it, so a pass here is never a pass by vacuum.

typedef void (*OffMainJob)(void);

OffMainJob off_main_job_ = 0;
volatile LONG off_main_thread_id_ = 0;

DWORD WINAPI off_main_thread(LPVOID) {
    InterlockedExchange(&off_main_thread_id_, static_cast<LONG>(GetCurrentThreadId()));
    if (off_main_job_) {
        off_main_job_();
    }
    return 0;
}

// Runs `job` on a thread of its own and waits for it to finish, so the main
// thread is provably outside every device method the job made. Answers with
// that thread's id, or 0 if it could not be run.
DWORD run_off_main(OffMainJob job) {
    off_main_job_ = job;
    InterlockedExchange(&off_main_thread_id_, 0);

    HANDLE thread = CreateThread(NULL, 0, &off_main_thread, 0, 0, NULL);
    if (!thread) {
        off_main_job_ = 0;
        return 0;
    }

    const bool finished = WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0;
    CloseHandle(thread);
    off_main_job_ = 0;
    return finished ? static_cast<DWORD>(off_main_thread_id_) : 0;
}

Method* gate_vtable_ = 0;
FakeDevice* gate_device_ = 0;
FeControl gate_control_;

// One frame's worth of every hooked method.
void gate_frame_job() {
    call_draw_primitive(gate_device_);
    call_draw_indexed_primitive(gate_device_);
    call_draw_primitive_up(gate_device_);
    call_draw_indexed_primitive_up(gate_device_);
    call_set_render_target(gate_device_);
    call_reset(gate_device_);
}

const int gate_extra_draws_ = 8;

void gate_more_frames_job() {
    for (int i = 0; i < gate_extra_draws_; ++i) {
        call_draw_primitive(gate_device_);
    }
}

void phase_thread_gate() {
    gate_vtable_ = make_vtable(0);
    gate_device_ = gate_vtable_ ? make_device(gate_vtable_) : 0;
    check(gate_device_ != 0, "a device whose every method records the thread that called it");
    if (!gate_device_) {
        return;
    }

    std::memset(&gate_control_, 0, sizeof(gate_control_));
    gate_control_.id = 4242;
    gate_control_.log = &order_log_;
    WdHandlerSet* const set = engine_a_.create_set(&gate_control_, WD_DAEMON_ABI, sizeof(WdHandlerSet));
    check(set != 0, "and a handler set to run a full cycle with");
    if (!set) {
        return;
    }

    const DWORD main_thread = GetCurrentThreadId();
    const LONG add_refs_before = add_ref_calls_;
    order_log_.count = 0;

    arm_device_watch();

    // ---- the cycle, exactly as an engine performs it ----------------------
    check_u(api_->ensure_hooks(as_device(gate_device_)), 1,
        "ensure_hooks installs from the main thread, which is where an engine attaches");
    check_u(api_->ensure_hooks(as_device(gate_device_)), 1, "a second one re-adopts the record");

    const DWORD render_thread = run_off_main(&gate_frame_job);
    check(render_thread != 0, "a render thread of its own runs a frame");
    check(render_thread != main_thread, "and it is not the main thread");

    // Between frames is where PostRender's retry lands.
    check_u(api_->ensure_hooks(as_device(gate_device_)), 1, "a third ensure_hooks, between frames");
    check(api_->register_set(set) != 0, "a set registers from the main thread");

    const DWORD render_thread_two = run_off_main(&gate_more_frames_job);
    check(render_thread_two != 0 && render_thread_two != main_thread,
        "more frames arrive, on a render thread again");
    check_u(gate_control_.pre_draw, static_cast<unsigned long>(gate_extra_draws_),
        "the registered handler saw every one of them");

    api_->unregister_set(set);
    check_u(api_->check_slots(), 0, "check_slots runs from the main thread and finds the vtable clean");
    const WdDaemonStats after_cycle = read_stats(api_);
    check_u(after_cycle.size, sizeof(WdDaemonStats), "stats runs from the main thread too");

    const LONG recorded = disarm_device_watch();

    // ---- what the window saw ----------------------------------------------
    check_u(static_cast<unsigned long>(device_log_lost_), 0,
        "every device call made in the window was recorded");
    check_u(static_cast<unsigned long>(recorded),
        static_cast<unsigned long>(WD_SLOT_COUNT + gate_extra_draws_),
        "the cycle really did drive the device: fourteen calls, and not one more than the frames made");

    int kinds = 0;
    for (int index = 0; index < WD_SLOT_COUNT; ++index) {
        if (device_method_seen(index, recorded)) {
            ++kinds;
        }
    }
    check_u(static_cast<unsigned long>(kinds), WD_SLOT_COUNT,
        "all six hooked methods were forwarded through in it");

    check_u(static_cast<unsigned long>(device_calls_from(main_thread, recorded)), 0,
        "NO METHOD OF THE DEVICE WAS INVOKED FROM THE MAIN THREAD, in a full "
        "attach / ensure_hooks / register / draw / unregister cycle");
    check_u(static_cast<unsigned long>(device_calls_from(render_thread, recorded)
            + device_calls_from(render_thread_two, recorded)),
        static_cast<unsigned long>(recorded),
        "every call came from a render thread, and from nowhere else");
    check(!device_method_seen(DEVICE_METHOD_ADD_REF, recorded)
        && !device_method_seen(DEVICE_METHOD_RELEASE, recorded),
        "no reference was taken or given back on any thread");
    check_u(static_cast<unsigned long>(add_ref_calls_ - add_refs_before), 0,
        "three ensure_hooks and fourteen frames add up to zero AddRefs");
    check_u(gate_device_->refcount, 0, "and the device's own refcount never moved");

    // MEASURED, never a literal: under a daemon that calls a device method from
    // the main thread this line says so out loud, beside the failing check.
    std::printf("thread : %ld device calls in the cycle, %d of them from the main thread\n",
        static_cast<long>(recorded), device_calls_from(main_thread, recorded));

    // ---- the gate on itself ------------------------------------------------
    // A check that cannot fail is not a check. This makes exactly the call the
    // gate exists to forbid -- one device method, on the main thread -- and
    // requires it to be seen. Putting device->AddRef() back in install() is the
    // same event from the daemon's side, and fails the check above the same way.
    arm_device_watch();
    call_draw_primitive(gate_device_);
    const LONG probe = disarm_device_watch();
    check_u(static_cast<unsigned long>(probe), 1, "a deliberate main-thread device call is one call");
    check_u(static_cast<unsigned long>(device_calls_from(main_thread, probe)), 1,
        "and the gate SEES it: the check above fails when a device method is called from the main thread");

    engine_a_.destroy_set(set);
}

// ---- a record installed but never drawn through ---------------------------
// The reference the daemon used to take at install was there to keep the
// retained record valid, and the obvious way to move it off the main thread was
// to take it in the first thunk instead. This is the case that answers that
// design: a vtable that is patched and then never dispatched through has no
// first thunk, so a deferred reference would never be taken at all -- weakest
// exactly where an idle device is likeliest to be released. With no reference
// anywhere there is nothing to defer and nothing to miss, and "correct" for
// such a record is that it is indistinguishable from any other:
//
//   - it installs, holds our six thunks, and takes nothing from the device;
//   - check_slots retains it and reports it clean while frames run elsewhere;
//   - a later ensure_hooks re-adopts it and re-patches nothing;
//   - and its FIRST frame, whenever it comes, forwards to ITS originals exactly
//     once -- the record was as valid at that moment as it was at install.

Method* quiet_vtable_ = 0;
FakeDevice* quiet_device_ = 0;
volatile LONG quiet_first_result_ = 0;

void quiet_first_frame_job() {
    InterlockedExchange(&quiet_first_result_,
        static_cast<LONG>(call_draw_primitive(quiet_device_)));
}

void phase_never_drawn() {
    quiet_vtable_ = make_vtable(1);
    quiet_device_ = quiet_vtable_ ? make_device(quiet_vtable_) : 0;
    check(quiet_device_ != 0, "a device that will be hooked and then left entirely alone");
    if (!quiet_device_) {
        return;
    }

    const WdDaemonStats before = read_stats(api_);
    check_u(api_->ensure_hooks(as_device(quiet_device_)), 1, "it installs");
    check_u(slots_from(quiet_vtable_, daemon_a_), WD_SLOT_COUNT, "with all six slots patched");
    check_u(quiet_device_->refcount, 0, "and nothing taken from the device");

    // Frames, on somebody else's vtable. This one is never dispatched through.
    Method* const busy = make_vtable(0);
    FakeDevice* const busy_device = busy ? make_device(busy) : 0;
    check_u(busy_device ? api_->ensure_hooks(as_device(busy_device)) : 0, 1,
        "another vtable installs beside it and takes the frames");
    for (int i = 0; i < 32; ++i) {
        call_draw_primitive(busy_device);
        call_reset(busy_device);
    }

    check_u(api_->check_slots(), 0, "check_slots reports nothing stomped");
    const WdDaemonStats after = read_stats(api_);
    check_u(after.vtables_dropped - before.vtables_dropped, 0,
        "and drops nothing: a record never drawn through is retained like any other");
    check_u(quiet_device_->calls[WD_INDEX_DRAW_PRIMITIVE], 0,
        "the quiet vtable really has never been drawn through");
    check_u(quiet_device_->refcount, 0, "and its device is still untouched");

    VtableSnapshot before_ensure;
    snapshot_vtable(quiet_vtable_, &before_ensure);
    check_u(api_->ensure_hooks(as_device(quiet_device_)), 1, "a later ensure_hooks re-adopts it");
    check_u(changed_slots(quiet_vtable_, before_ensure), 0, "having re-patched nothing");
    check_u(quiet_device_->refcount, 0, "and still holding no reference");

    // The first frame this vtable has ever seen, arbitrarily late.
    const LONG mine = family_calls_[1][WD_INDEX_DRAW_PRIMITIVE];
    const LONG theirs = family_calls_[0][WD_INDEX_DRAW_PRIMITIVE];
    const DWORD thread = run_off_main(&quiet_first_frame_job);
    check(thread != 0 && thread != GetCurrentThreadId(),
        "its first frame ever arrives, on a render thread");
    check_u(static_cast<unsigned long>(quiet_first_result_),
        static_cast<unsigned long>(original_result_ + WD_INDEX_DRAW_PRIMITIVE),
        "with the original's HRESULT");
    check_u(family_calls_[1][WD_INDEX_DRAW_PRIMITIVE] - mine, 1,
        "forwarding to ITS originals exactly once");
    check_u(family_calls_[0][WD_INDEX_DRAW_PRIMITIVE] - theirs, 0, "and to no other record's");
    check_u(quiet_device_->refcount, 0, "with nothing taken from the device on that frame either");
}

// ---- the whole run, in one number ----------------------------------------

void phase_no_reference_taken() {
    check_u(static_cast<unsigned long>(add_ref_calls_), 0,
        "across every install, every ensure_hooks and every frame this run made, "
        "the daemon called AddRef exactly zero times");
    check_u(static_cast<unsigned long>(release_calls_), 0, "and Release zero times");
    std::printf("thread : %ld AddRef and %ld Release on any device, whole run\n",
        static_cast<long>(add_ref_calls_), static_cast<long>(release_calls_));
}

// ---- children ------------------------------------------------------------

bool spawn(const char* arguments, PROCESS_INFORMATION* process) {
    char command[MAX_PATH * 2];
    std::snprintf(command, sizeof(command), "\"%s\" %s", exe_path_, arguments);

    STARTUPINFOA startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    std::memset(process, 0, sizeof(*process));
    return CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, process) != FALSE;
}

void collect(const char* label, const char* result_file, PROCESS_INFORMATION* process) {
    DWORD code = 0xFFFFFFFF;
    if (WaitForSingleObject(process->hProcess, 60000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process->hProcess, &code);
    }
    CloseHandle(process->hThread);
    CloseHandle(process->hProcess);

    int run = 0;
    int failed = -1;
    FILE* const file = std::fopen(result_file, "rb");
    if (file) {
        if (std::fscanf(file, "%d %d", &run, &failed) != 2) {
            failed = -1;
        }
        std::fclose(file);
    }

    std::printf("child  : %-10s exit %lu, %d checks, %d failed\n", label,
        static_cast<unsigned long>(code), run, failed < 0 ? 0 : failed);
    checks_run += run;
    if (failed > 0) {
        checks_failed += failed;
    }
    check(code == 0 && failed == 0, label);
}

void phase_children() {
    char result[MAX_PATH];
    char arguments[MAX_PATH * 2];
    PROCESS_INFORMATION takeover;
    PROCESS_INFORMATION garbage;
    PROCESS_INFORMATION abi;

    path_in(result, sizeof(result), "result_takeover.txt");
    DeleteFileA(result);
    std::snprintf(arguments, sizeof(arguments), "child-takeover 0");
    if (spawn(arguments, &takeover)) {
        collect("takeover", result, &takeover);
    } else {
        check(false, "the takeover child starts");
    }

    path_in(result, sizeof(result), "result_garbage.txt");
    DeleteFileA(result);
    std::snprintf(arguments, sizeof(arguments), "child-garbage 41414141");
    if (spawn(arguments, &garbage)) {
        collect("garbage", result, &garbage);
    } else {
        check(false, "the half-written-record child starts");
    }

    path_in(result, sizeof(result), "result_abi.txt");
    DeleteFileA(result);
    std::snprintf(arguments, sizeof(arguments), "child-abi");
    if (spawn(arguments, &abi)) {
        collect("abi", result, &abi);
    } else {
        check(false, "the ABI-range child starts");
    }

    PROCESS_INFORMATION election;
    path_in(result, sizeof(result), "result_election.txt");
    DeleteFileA(result);
    std::snprintf(arguments, sizeof(arguments), "child-election");
    if (spawn(arguments, &election)) {
        collect("election", result, &election);
    } else {
        check(false, "the election-retry child starts");
    }

    // Two processes at once. The gate makes their elections overlap, and each
    // waits for the other's record before it exits, so neither can finish early
    // and make the isolation trivial.
    char gate_name[WD_NAME_MAX];
    std::snprintf(gate_name, sizeof(gate_name), "Local\\wd_harness_gate_%08X",
        static_cast<unsigned>(GetCurrentProcessId()));
    HANDLE gate = CreateEventA(NULL, TRUE, FALSE, gate_name);
    check(gate != NULL, "a gate to start both processes together");

    char peer_zero[MAX_PATH];
    char peer_one[MAX_PATH];
    path_in(peer_zero, sizeof(peer_zero), "peer0.txt");
    path_in(peer_one, sizeof(peer_one), "peer1.txt");
    DeleteFileA(peer_zero);
    DeleteFileA(peer_one);
    char done_zero[MAX_PATH];
    char done_one[MAX_PATH];
    path_in(done_zero, sizeof(done_zero), "peer0.done");
    path_in(done_one, sizeof(done_one), "peer1.done");
    DeleteFileA(done_zero);
    DeleteFileA(done_one);

    PROCESS_INFORMATION first;
    PROCESS_INFORMATION second;
    std::snprintf(arguments, sizeof(arguments), "child-peer 0 %s", gate_name);
    const bool started_first = spawn(arguments, &first);
    std::snprintf(arguments, sizeof(arguments), "child-peer 1 %s", gate_name);
    const bool started_second = spawn(arguments, &second);
    check(started_first && started_second, "both peer processes start");
    if (gate) {
        SetEvent(gate);
    }

    if (started_first) {
        path_in(result, sizeof(result), "result_peer0.txt");
        collect("peer 0", result, &first);
    }
    if (started_second) {
        path_in(result, sizeof(result), "result_peer1.txt");
        collect("peer 1", result, &second);
    }
    if (gate) {
        CloseHandle(gate);
    }

    // And this process's own record is exactly where it was.
    HANDLE mapping = NULL;
    WdDaemonRecord* record = 0;
    if (open_record(GetCurrentProcessId(), &mapping, &record) && record) {
        check_u(record->magic, WD_MAGIC, "this process's record survived two others electing");
        check(ends_with(record->winner_path, "a\\worlddraw_daemon.dll"), "and still names our winner");
    } else {
        check(false, "this process's record is still there");
    }
}

// ---- a failed election, then a successful one ----------------------------
// Every step of the election can fail without anything being wrong with the
// image running it, and this daemon can never be unloaded and reloaded to try
// again: a failure that latches is a client that must be restarted. So this
// child makes the election fail TWICE, in two different places, and then lets
// it succeed -- in one process, on one loaded image, through the one export.
//
// It needs its own process because the parent's election has already won: the
// only way to watch a first election fail is to be the process it happens in.

typedef HANDLE (WINAPI* CreateFileMappingMethod)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);

CreateFileMappingMethod real_create_file_mapping_ = 0;
uintptr_t* create_mapping_import_ = 0;
uintptr_t create_mapping_original_ = 0;
volatile LONG mapping_calls_ = 0;
volatile LONG mapping_fail_ = 0;

// Counting is as important as failing: it is what proves an election really ran
// again rather than a cached answer being handed back, and what proves a later
// call did NOT run one.
HANDLE WINAPI watching_create_file_mapping(HANDLE file, LPSECURITY_ATTRIBUTES attributes,
    DWORD protect, DWORD high, DWORD low, LPCSTR name) {
    InterlockedIncrement(&mapping_calls_);
    if (mapping_fail_) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return real_create_file_mapping_(file, attributes, protect, high, low, name);
}

LONG take_mapping_calls() {
    return InterlockedExchange(&mapping_calls_, 0);
}

// The election mutex, held from a thread that is not the one that elects. A
// mutex belongs to the thread that took it, so the release has to happen here
// too.
struct MutexHold {
    HANDLE mutex;
    HANDLE release;
    volatile LONG state;    /* 1 held, -1 could not be held */
};

DWORD WINAPI hold_election_mutex(LPVOID parameter) {
    MutexHold* const hold = static_cast<MutexHold*>(parameter);

    char name[WD_NAME_MAX];
    std::snprintf(name, sizeof(name), WD_MUTEX_NAME_FORMAT,
        static_cast<unsigned>(GetCurrentProcessId()));
    hold->mutex = CreateMutexA(NULL, TRUE, name);
    InterlockedExchange(&hold->state, hold->mutex ? 1 : -1);

    WaitForSingleObject(hold->release, 60000);
    if (hold->mutex) {
        ReleaseMutex(hold->mutex);
        CloseHandle(hold->mutex);
    }
    return 0;
}

int child_election(const char* result_file) {
    char name[WD_NAME_MAX];
    std::snprintf(name, sizeof(name), WD_MAPPING_NAME_FORMAT,
        static_cast<unsigned>(GetCurrentProcessId()));

    MutexHold hold;
    std::memset(&hold, 0, sizeof(hold));
    hold.release = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(hold.release != NULL, "an event to release the mutex holder with");
    if (!hold.release) {
        write_result(result_file);
        return 1;
    }

    HANDLE holder = CreateThread(NULL, 0, &hold_election_mutex, &hold, 0, NULL);
    check(holder != NULL, "a thread to hold this process's election mutex");
    if (!holder) {
        write_result(result_file);
        return 1;
    }
    for (int waited = 0; !hold.state && waited < 5000; waited += 5) {
        Sleep(5);
    }
    check(hold.state == 1, "it holds the mutex the election has to take");

    char path[MAX_PATH];
    path_in(path, sizeof(path), "a\\worlddraw_daemon.dll");
    HMODULE daemon = LoadLibraryA(path);
    WdDaemonAcquire acquire = daemon ? reinterpret_cast<WdDaemonAcquire>(
        proc_address(daemon, WD_DAEMON_ACQUIRE_NAME)) : 0;
    check(acquire != 0, "the daemon loads and exports its entry point");
    if (!acquire) {
        SetEvent(hold.release);
        write_result(result_file);
        return 1;
    }

    create_mapping_import_ = find_import(daemon, "KERNEL32.dll", "CreateFileMappingA");
    check(create_mapping_import_ != 0, "the daemon's CreateFileMappingA import can be watched");
    if (create_mapping_import_) {
        check(write_import(create_mapping_import_,
            reinterpret_cast<uintptr_t>(&watching_create_file_mapping), &create_mapping_original_),
            "the watcher is installed");
        real_create_file_mapping_ = reinterpret_cast<CreateFileMappingMethod>(create_mapping_original_);
    }
    take_mapping_calls();

    // ---- failure one: the wait for the election mutex times out -----------
    const DWORD started = GetTickCount();
    check(acquire(WD_DAEMON_ABI) == 0, "an election that cannot take the mutex hands out nothing");
    const DWORD elapsed = GetTickCount() - started;
    check(elapsed >= 4000, "having waited for it rather than failing on something else");
    check_u(static_cast<unsigned long>(take_mapping_calls()), 0,
        "and never reached the mapping: the mutex is where it stopped");
    HANDLE probe = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    check(probe == NULL, "nothing was published");
    if (probe) {
        CloseHandle(probe);
    }

    SetEvent(hold.release);
    WaitForSingleObject(holder, 60000);
    CloseHandle(holder);
    CloseHandle(hold.release);

    // ---- failure two: the mapping cannot be created ----------------------
    // A DIFFERENT step, on a later call, in the same process: the first failure
    // did not latch, and neither does this one.
    InterlockedExchange(&mapping_fail_, 1);
    check(acquire(WD_DAEMON_ABI) == 0, "an election whose mapping cannot be created hands out nothing");
    check_u(static_cast<unsigned long>(take_mapping_calls()), 1,
        "and it really ran again: the first failure did not latch");
    InterlockedExchange(&mapping_fail_, 0);
    probe = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    check(probe == NULL, "still nothing published");
    if (probe) {
        CloseHandle(probe);
    }

    // ---- and then it succeeds --------------------------------------------
    const WdDaemonApi* const api = acquire(WD_DAEMON_ABI);
    check(api != 0, "a later acquire elects successfully after two failed elections");
    check_u(static_cast<unsigned long>(take_mapping_calls()), 1, "having run the election once more");
    if (!api) {
        write_result(result_file);
        return 1;
    }
    check_u(api->abi_version, WD_DAEMON_ABI, "the api it hands out is complete");
    check(api->build_id() != 0 && std::strcmp(api->build_id(), WD_DAEMON_BUILD) == 0,
        "and names this build");
    check_u(api->register_set(0), 0, "the api works");

    HANDLE mapping = NULL;
    WdDaemonRecord* record = 0;
    check(open_record(GetCurrentProcessId(), &mapping, &record), "the record is published at last");
    if (record) {
        check_u(record->magic, WD_MAGIC, "with its magic");
        check(ends_with(record->winner_path, "a\\worlddraw_daemon.dll"), "naming the image that won");
    }

    // ---- success is idempotent -------------------------------------------
    check_p(acquire(WD_DAEMON_ABI), api, "a later call hands out the same api");
    check_u(static_cast<unsigned long>(take_mapping_calls()), 0, "and runs no second election");
    check(acquire(WD_DAEMON_ABI + 1) == 0, "a caller needing a newer ABI is refused");
    check_u(static_cast<unsigned long>(take_mapping_calls()), 0,
        "and an ABI refusal is not an election failure: it re-elects nothing");
    check_p(acquire(WD_DAEMON_ABI), api, "the api is still there afterwards");

    if (create_mapping_import_) {
        uintptr_t ignored = 0;
        write_import(create_mapping_import_, create_mapping_original_, &ignored);
        create_mapping_import_ = 0;
    }

    write_result(result_file);
    return checks_failed == 0 ? 0 : 1;
}

int child_takeover(unsigned long fill, const char* result_file) {
    char name[WD_NAME_MAX];
    std::snprintf(name, sizeof(name), WD_MAPPING_NAME_FORMAT,
        static_cast<unsigned>(GetCurrentProcessId()));

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
        WD_MAPPING_BYTES, name);
    const DWORD created = GetLastError();
    check(mapping != NULL, "the record mapping can be made before any daemon runs");
    check(created != ERROR_ALREADY_EXISTS, "and nothing had made it first");
    if (!mapping) {
        write_result(result_file);
        return 1;
    }

    WdDaemonRecord* const record = static_cast<WdDaemonRecord*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, WD_MAPPING_BYTES));
    check(record != 0, "and mapped");
    if (!record) {
        write_result(result_file);
        return 1;
    }

    // What a daemon that died mid-election leaves behind: a mapping that exists
    // with no valid record in it.
    std::memset(record, 0, WD_MAPPING_BYTES);
    record->magic = static_cast<uint32_t>(fill);
    record->abi_version = 0xDEADBEEF;
    record->record_size = 0xDEADBEEF;
    record->winner_path[0] = 'X';

    char path[MAX_PATH];
    path_in(path, sizeof(path), "a\\worlddraw_daemon.dll");
    HMODULE daemon = LoadLibraryA(path);
    check(daemon != NULL, "a daemon arrives afterwards");
    if (!daemon) {
        write_result(result_file);
        return 1;
    }

    WdDaemonAcquire acquire = reinterpret_cast<WdDaemonAcquire>(
        proc_address(daemon, WD_DAEMON_ACQUIRE_NAME));
    check(acquire != 0, "and exports its entry point");
    if (!acquire) {
        write_result(result_file);
        return 1;
    }

    const WdDaemonApi* const api = acquire(WD_DAEMON_ABI);
    check(api != 0, "it takes over the abandoned mapping rather than refusing");
    check_u(record->magic, WD_MAGIC, "magic is now ours");
    check_u(record->record_size, sizeof(WdDaemonRecord), "the stale record_size was overwritten");
    check_u(record->abi_version, WD_DAEMON_ABI, "and the stale abi_version");
    check(ends_with(record->winner_path, "a\\worlddraw_daemon.dll"), "winner_path names the new winner");
    if (api) {
        check_p(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(record->api.ensure_hooks)),
            reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(api->ensure_hooks)),
            "the record carries the api that was handed out");
        check_u(api->register_set(0), 0, "the api works");
    }

    write_result(result_file);
    return checks_failed == 0 ? 0 : 1;
}

int child_abi(const char* result_file) {
    char path[MAX_PATH];
    path_in(path, sizeof(path), "a\\worlddraw_daemon.dll");
    HMODULE daemon = LoadLibraryA(path);
    check(daemon != NULL, "the daemon loads");
    if (!daemon) {
        write_result(result_file);
        return 1;
    }

    WdDaemonAcquire acquire = reinterpret_cast<WdDaemonAcquire>(
        proc_address(daemon, WD_DAEMON_ACQUIRE_NAME));
    check(acquire != 0, "and exports its entry point");
    if (!acquire) {
        write_result(result_file);
        return 1;
    }

    check(acquire(WD_DAEMON_ABI + 1) == 0, "the first caller is refused if it needs a newer daemon");

    // Refused, but resident: the engine needs the record to be able to say
    // which addon's copy won and at what ABI.
    HANDLE mapping = NULL;
    WdDaemonRecord* record = 0;
    check(open_record(GetCurrentProcessId(), &mapping, &record), "the record was published anyway");
    if (record) {
        check_u(record->magic, WD_MAGIC, "with its magic");
        check_u(record->abi_version, WD_DAEMON_ABI, "and the ABI it really has");
        check(record->winner_path[0] != '\0', "and a path to name in the error");
    }

    const WdDaemonApi* const api = acquire(WD_DAEMON_ABI);
    check(api != 0, "a caller it can serve is served after the refusal");
    check(acquire(0) == api, "and so is one that asks for less");

    write_result(result_file);
    return checks_failed == 0 ? 0 : 1;
}

int child_peer(int index, const char* gate_name, const char* result_file) {
    const DWORD pid = GetCurrentProcessId();
    char name[WD_NAME_MAX];
    std::snprintf(name, sizeof(name), WD_MAPPING_NAME_FORMAT, static_cast<unsigned>(pid));

    HANDLE probe = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    check(probe == NULL, "this process has no record before it elects one");
    if (probe) {
        CloseHandle(probe);
    }

    HANDLE gate = OpenEventA(SYNCHRONIZE, FALSE, gate_name);
    if (gate) {
        WaitForSingleObject(gate, 30000);
        CloseHandle(gate);
    }

    char path[MAX_PATH];
    path_in(path, sizeof(path), index == 0 ? "a\\worlddraw_daemon.dll" : "b\\worlddraw_daemon.dll");
    HMODULE daemon = LoadLibraryA(path);
    WdDaemonAcquire acquire = daemon ? reinterpret_cast<WdDaemonAcquire>(
        proc_address(daemon, WD_DAEMON_ACQUIRE_NAME)) : 0;
    check(acquire != 0, "the daemon loads in this process");
    if (!acquire) {
        write_result(result_file);
        return 1;
    }

    check(acquire(WD_DAEMON_ABI) != 0, "and elects itself here, whatever the other process is doing");

    const char* const own_suffix = index == 0 ? "a\\worlddraw_daemon.dll" : "b\\worlddraw_daemon.dll";
    const char* const peer_suffix = index == 0 ? "b\\worlddraw_daemon.dll" : "a\\worlddraw_daemon.dll";

    HANDLE mapping = NULL;
    WdDaemonRecord* record = 0;
    check(open_record(pid, &mapping, &record), "this process has a record of its own");
    if (record) {
        check_u(record->magic, WD_MAGIC, "published");
        check(ends_with(record->winner_path, own_suffix), "naming the image this process loaded");
    }

    char own_file[MAX_PATH];
    char own_temp[MAX_PATH];
    path_in(own_file, sizeof(own_file), index == 0 ? "peer0.txt" : "peer1.txt");
    path_in(own_temp, sizeof(own_temp), index == 0 ? "peer0.new" : "peer1.new");
    check(announce(own_file, own_temp, static_cast<unsigned long>(pid)),
        "it can announce its pid to the other process");

    char peer_file[MAX_PATH];
    path_in(peer_file, sizeof(peer_file), index == 0 ? "peer1.txt" : "peer0.txt");
    const unsigned long peer_pid = await_announcement(peer_file, 30000);
    check(peer_pid != 0, "the other process is running at the same time");
    if (peer_pid == 0) {
        write_result(result_file);
        return 1;
    }

    HANDLE peer_mapping = NULL;
    WdDaemonRecord* peer_record = 0;
    const bool opened = open_record(static_cast<DWORD>(peer_pid), &peer_mapping, &peer_record);
    if (!opened) {
        std::printf("         peer %d could not open the record of pid %lu (error %lu)\n",
            index, peer_pid, static_cast<unsigned long>(GetLastError()));
    }
    check(opened, "the other process's record lives under its own pid-scoped name");
    if (peer_record) {
        check_u(peer_record->magic, WD_MAGIC, "and is published");
        check(ends_with(peer_record->winner_path, peer_suffix),
            "it elected its own daemon image, not ours");
        check(peer_record != record, "the two records are different objects");
    }
    if (record) {
        check_u(record->magic, WD_MAGIC, "our own record is untouched by the other election");
        check(ends_with(record->winner_path, own_suffix), "and still names our winner");
    }

    char own_done[MAX_PATH];
    char own_done_temp[MAX_PATH];
    char peer_done[MAX_PATH];
    path_in(own_done, sizeof(own_done), index == 0 ? "peer0.done" : "peer1.done");
    path_in(own_done_temp, sizeof(own_done_temp), index == 0 ? "peer0.donenew" : "peer1.donenew");
    path_in(peer_done, sizeof(peer_done), index == 0 ? "peer1.done" : "peer0.done");
    announce(own_done, own_done_temp, static_cast<unsigned long>(pid));
    check(await_announcement(peer_done, 30000) != 0,
        "both processes stayed up until both had read: a record dies with its process");

    write_result(result_file);
    return checks_failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    GetModuleFileNameA(NULL, exe_path_, MAX_PATH);
    std::strncpy(base_dir_, exe_path_, MAX_PATH - 1);
    base_dir_[MAX_PATH - 1] = '\0';
    char* const separator = std::strrchr(base_dir_, '\\');
    if (separator) {
        *separator = '\0';
    }

    const char* const mode = argc > 1 ? argv[1] : "parent";
    char result[MAX_PATH];

    if (std::strcmp(mode, "child-takeover") == 0 || std::strcmp(mode, "child-garbage") == 0) {
        const bool garbage = std::strcmp(mode, "child-garbage") == 0;
        path_in(result, sizeof(result), garbage ? "result_garbage.txt" : "result_takeover.txt");
        const unsigned long fill = argc > 2 ? std::strtoul(argv[2], 0, 16) : 0;
        return child_takeover(fill, result);
    }
    if (std::strcmp(mode, "child-abi") == 0) {
        path_in(result, sizeof(result), "result_abi.txt");
        return child_abi(result);
    }
    if (std::strcmp(mode, "child-election") == 0) {
        path_in(result, sizeof(result), "result_election.txt");
        return child_election(result);
    }
    if (std::strcmp(mode, "child-peer") == 0) {
        const int index = argc > 2 ? std::atoi(argv[2]) : 0;
        const char* const gate = argc > 3 ? argv[3] : "";
        path_in(result, sizeof(result), index == 0 ? "result_peer0.txt" : "result_peer1.txt");
        return child_peer(index, gate, result);
    }

    phase_election();
    if (api_) {
        phase_install();
        phase_sets();
        phase_growth();
        phase_reset_loops();
        phase_drain();
        phase_unregister_then_free();
        phase_refusals();
        phase_stomps();
        phase_live_vtable_kept();
        phase_many_vtables();
        phase_thread_gate();
        phase_never_drawn();
        phase_no_reference_taken();
    }
    phase_children();

    std::printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed == 0) {
        std::printf("PASS   : the daemon behaves across every boundary this can build\n");
        return 0;
    }
    std::printf("FAIL\n");
    return 1;
}
