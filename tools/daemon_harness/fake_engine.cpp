// A stand-in for the engine image: it builds handler sets whose functions live
// in this image and whose storage is a page of its own. Built three times into
// three directories under the same basename, which the loader treats as three
// separate images -- that is the topology the daemon has to survive.
//
// It deliberately does NOT take the self-reference the real engine takes at
// register_set: the harness needs to be able to unmap this image while the
// daemon is still running, to prove the daemon kept no pointer into it.

#include "fake_engine.h"

namespace {

const char module_anchor_ = 'e';

void note(FeControl* control) {
    if (!control->log) {
        return;
    }

    const LONG at = InterlockedIncrement(&control->log->count) - 1;
    if (at >= 0 && at < FE_ORDER_MAX) {
        control->log->ids[at] = control->id;
    }
}

void __stdcall handler_pre_reset(void* user, IDirect3DDevice8* device, D3DPRESENT_PARAMETERS*) {
    FeControl* const control = static_cast<FeControl*>(user);
    control->last_device = device;
    InterlockedIncrement(&control->pre_reset);
    note(control);
}

void __stdcall handler_post_reset(void* user, IDirect3DDevice8* device, HRESULT result) {
    FeControl* const control = static_cast<FeControl*>(user);
    control->last_device = device;
    control->last_reset_result = static_cast<LONG>(result);
    InterlockedIncrement(&control->post_reset);
    note(control);
}

void __stdcall handler_pre_set_render_target(void* user, IDirect3DDevice8* device,
    IDirect3DSurface8* render_target, IDirect3DSurface8*) {
    FeControl* const control = static_cast<FeControl*>(user);
    control->last_device = device;
    control->last_render_target = render_target;
    InterlockedIncrement(&control->pre_set_render_target);
    note(control);
}

void __stdcall handler_pre_draw(void* user, IDirect3DDevice8* device) {
    FeControl* const control = static_cast<FeControl*>(user);
    control->last_device = device;
    InterlockedIncrement(&control->pre_draw);
    note(control);

    // The park is how the harness holds a thread inside a handler while it asks
    // the daemon to unregister the set.
    if (control->park) {
        InterlockedExchange(&control->parked, 1);
        while (control->park) {
            SwitchToThread();
        }
        InterlockedExchange(&control->park_left, 1);
    }
}

}  // namespace

// The set gets a page to itself so the driver can seal it or free it.
extern "C" WdHandlerSet* __stdcall fe_create_set(FeControl* control, uint32_t abi_version, uint32_t size) {
    void* page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) {
        return NULL;
    }

    WdHandlerSet* const set = static_cast<WdHandlerSet*>(page);
    set->abi_version = abi_version;
    set->size = size;
    set->user = control;
    set->pre_reset = &handler_pre_reset;
    set->post_reset = &handler_post_reset;
    set->pre_set_render_target = &handler_pre_set_render_target;
    set->pre_draw = &handler_pre_draw;
    return set;
}

extern "C" void __stdcall fe_destroy_set(WdHandlerSet* set) {
    if (set) {
        VirtualFree(set, 0, MEM_RELEASE);
    }
}

// Anything reading this set afterwards faults, which is the point.
extern "C" void __stdcall fe_seal_set(WdHandlerSet* set) {
    DWORD old_protect = 0;
    if (set) {
        VirtualProtect(set, 4096, PAGE_NOACCESS, &old_protect);
    }
}

extern "C" HMODULE __stdcall fe_module(void) {
    HMODULE module = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        &module_anchor_, &module);
    return module;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
