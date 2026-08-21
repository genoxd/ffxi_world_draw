// The shared surface between the harness driver and the fake engine images it
// loads. The driver owns every FeControl; the engine owns the WdHandlerSet it
// builds, and puts it in a page of its own so the driver can take that page
// away and prove the daemon never touches it again.

#ifndef FAKE_ENGINE_H_
#define FAKE_ENGINE_H_

#include "../../daemon/worlddraw_abi.h"

// Harness bookkeeping: how many handler calls one dispatch can be asked to
// record. Nothing in the daemon is bounded by it -- it only has to be larger
// than the biggest set count the harness itself registers.
#define FE_ORDER_MAX 256

// Which set's handler ran, in the order it ran. Shared by every engine image.
typedef struct FeOrderLog {
    volatile LONG count;
    uint32_t ids[FE_ORDER_MAX];
} FeOrderLog;

typedef struct FeControl {
    uint32_t id;
    FeOrderLog* log;
    volatile LONG pre_draw;
    volatile LONG pre_set_render_target;
    volatile LONG pre_reset;
    volatile LONG post_reset;
    volatile LONG park;         /* pre_draw spins here while this is 1 */
    volatile LONG parked;       /* it has entered the spin */
    volatile LONG park_left;    /* it has come out of the spin */
    volatile LONG last_reset_result;
    void* last_device;
    void* last_render_target;
} FeControl;

typedef WdHandlerSet* (__stdcall* FeCreateSet)(FeControl*, uint32_t, uint32_t);
typedef void (__stdcall* FeDestroySet)(WdHandlerSet*);
typedef void (__stdcall* FeSealSet)(WdHandlerSet*);
typedef HMODULE (__stdcall* FeModule)(void);

#endif  // FAKE_ENGINE_H_
