#include "spoke/vexspoke.h"

#include "nio/mem.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "input/focus.h"
#include "event/keyhandler.h"
#include "event/mousehandler.h"
#include "event/touchhandler.h"
#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: VexspokeApi (spoke/vexspoke.c — table defined in spoke/vexspoke.h)
 * LEVEL: L2 — Behavior (R1→R2 bridge: contract table + static binder)
 * ============================================================================
 * The single seam through which hotcwap touches vexspoke. The header owns the
 * dependency-free contract (opaque void* throughout, so kernel/ never names
 * a vexspoke type); this file owns the exactly-one place that includes
 * vexspoke headers, adapting static symbols into the table. Strangler
 * phase 1: every slot fills from the static link (wrappers where the opaque
 * shape differs, direct assignment where signatures already match), so the
 * call-site migration proves the table with zero behavior change. The
 * refresh phase replaces the filler with dlopen — call sites never move
 * again.
 *
 * STRUCT FIELDS (Mirroring spoke/vexspoke.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   void *(*createArena)(size_t, uint64_t*);  // arena slab set + attested id
 *   void (*destroyArena)(void*);              // tear down an arena handle
 *   void (*keyAttachWindow)(uint32_t, const void*);   // key ring attach
 *   bool (*keyDetachWindow)(uint32_t, const void*);   // key ring detach
 *   void (*keyDetachWindowAll)(uint32_t);             // key ring detach-all
 *   void (*keyPushEvent)(uint32_t, int, int, uint64_t); // key event push
 *   void (*keyPushCharEvent)(uint32_t, uint32_t);      // text char push
 *   void (*keyDispatchEvents)(void);                   // key ring drain
 *   bool (*keyIsDown)(int);                           // key held probe
 *   void (*keyShutdown)(void);                         // key ring teardown
 *   void (*mouseAttachWindow)(uint32_t, const void*); // mouse ring attach
 *   bool (*mouseDetachWindow)(uint32_t, const void*); // mouse ring detach
 *   void (*mouseDetachWindowAll)(uint32_t);           // mouse detach-all
 *   void (*mousePushButtonEvent)(uint32_t, int, int, uint64_t); // button push
 *   void (*mousePushMoveEvent)(uint32_t, double, double);       // move push
 *   void (*mousePushMoveDeltaEvent)(uint32_t, double, double);  // delta push
 *   void (*mousePushDragEvent)(uint32_t, int, double, double);  // drag push
 *   void (*mousePushScrollEvent)(uint32_t, double, double);     // scroll push
 *   void (*mousePushZoomEvent)(uint32_t, double);               // zoom push
 *   void (*mouseDispatchEvents)(void);                 // mouse ring drain
 *   double (*mouseX)(void);                            // cursor x probe
 *   void (*touchAttachWindow)(uint32_t, const void*); // touch ring attach
 *   bool (*touchDetachWindow)(uint32_t, const void*); // touch ring detach
 *   void (*touchDetachWindowAll)(uint32_t);           // touch detach-all
 *   void (*touchPushTouchEvent)(uint32_t, int, int, double, double, double, uint64_t); // touch push
 *   void (*touchDispatchEvents)(void);                // touch ring drain
 *   void (*focusSet)(uint32_t);                       // spotlight mirror
 *   bool (*focusIsFocused)(uint32_t);                 // spotlight probe
 *   bool bound;                            // true once any binder filled the table
 *   uint64_t generation;                   // binder generation (static phase: 1)
 *   uint64_t abi;                          // verified digest (static phase: 0, unverified)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - VexspokeApi_fillStatic(api)  : fill every slot from static symbols
 *
 * Setters:
 *   - (none — the table is filled whole by binders, never piecemeal)
 *
 * Getters:
 *   - (none — call sites read table members directly)
 * ============================================================================
 */

// Opaque-shape adapters: the table speaks void* so hotcwap never names a
// vexspoke type; these eight wrappers cast back at the single seam.
static void *wrapCreateArena(size_t totalBytes, uint64_t *outTypeId) {
    MemoryArena *a = MemoryArena_create(totalBytes);
    if (outTypeId)
        *outTypeId = (a != nullptr) ? VEXSPOKE_TYPE_ARENA : 0;
    return (void*) a;
}

static void wrapDestroyArena(void *arena) {
    MemoryArena_destroy((MemoryArena*) arena);
}

static void wrapKeyAttach(uint32_t windowId, const void *listener) {
    Key_attachWindow(windowId, (const KeyHandler*) listener);
}

static bool wrapKeyDetach(uint32_t windowId, const void *listener) {
    return Key_detachWindow(windowId, (const KeyHandler*) listener);
}

static void wrapMouseAttach(uint32_t windowId, const void *listener) {
    Mouse_attachWindow(windowId, (const MouseHandler*) listener);
}

static bool wrapMouseDetach(uint32_t windowId, const void *listener) {
    return Mouse_detachWindow(windowId, (const MouseHandler*) listener);
}

static void wrapTouchAttach(uint32_t windowId, const void *listener) {
    Touch_attachWindow(windowId, (const TouchHandler*) listener);
}

static bool wrapTouchDetach(uint32_t windowId, const void *listener) {
    return Touch_detachWindow(windowId, (const TouchHandler*) listener);
}

// CORE FUNCTIONS
bool VexspokeApi_fillStatic(VexspokeApi *api) {
    if (api == nullptr)
        return false;
    (*api).createArena = wrapCreateArena;
    (*api).destroyArena = wrapDestroyArena;
    (*api).keyAttachWindow = wrapKeyAttach;
    (*api).keyDetachWindow = wrapKeyDetach;
    (*api).keyDetachWindowAll = Key_detachWindowAll;
    (*api).keyPushEvent = Key_pushEvent;
    (*api).keyPushCharEvent = Key_pushCharEvent;
    (*api).keyDispatchEvents = Key_dispatchEvents;
    (*api).keyIsDown = Key_isDown;
    (*api).keyShutdown = Key_shutdown;
    (*api).mouseAttachWindow = wrapMouseAttach;
    (*api).mouseDetachWindow = wrapMouseDetach;
    (*api).mouseDetachWindowAll = Mouse_detachWindowAll;
    (*api).mousePushButtonEvent = Mouse_pushButtonEvent;
    (*api).mousePushMoveEvent = Mouse_pushMoveEvent;
    (*api).mousePushMoveDeltaEvent = Mouse_pushMoveDeltaEvent;
    (*api).mousePushDragEvent = Mouse_pushDragEvent;
    (*api).mousePushScrollEvent = Mouse_pushScrollEvent;
    (*api).mousePushZoomEvent = Mouse_pushZoomEvent;
    (*api).mouseDispatchEvents = Mouse_dispatchEvents;
    (*api).mouseX = Mouse_x;
    (*api).touchAttachWindow = wrapTouchAttach;
    (*api).touchDetachWindow = wrapTouchDetach;
    (*api).touchDetachWindowAll = Touch_detachWindowAll;
    (*api).touchPushTouchEvent = Touch_pushTouchEvent;
    (*api).touchDispatchEvents = Touch_dispatchEvents;
    (*api).focusSet = Focus_set;
    (*api).focusIsFocused = Focus_isFocused;
    (*api).bound = true;
    (*api).generation = 1;
    (*api).abi = 0;
    return true;
}
