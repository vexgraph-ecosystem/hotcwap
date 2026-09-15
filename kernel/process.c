#include "process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "annotation/intention.h"
#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Process (kernel/process.c)
 * LEVEL: L2 — Behavior (one-shot hot-loadable function wrapper)
 * ============================================================================
 * A Process wraps a single `main`-shaped entry: invoke, run to completion,
 * exit status, re-runnable. The hot module is retired (not unloaded) while a
 * call is in flight. One Process = one function; never ticked, never owns a
 * window, a thread, or a socket.
 *
 * STRUCT FIELDS (Mirroring kernel/process.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   ProcessEntry entry;             // hot-bound entry fn (null = unbound)
 *   void *hot;                      // opaque retire pin (the Conflict Triage Law seam)
 *   _Atomic bool inFlight;          // re-entrancy guard (one invoke at a time)
 *   uint32_t invocationCount;       // completed invocations
 *
 * PRIVATE HELPERS: None.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Process(entry)                        : Process_1(entry)
 *
 * Core Functions:
 *   - Process_run(self, argc, argv)         : invoke entry; returns exit status
 *   - Process_isRunning(self)
 *   - Process_getInvocationCount(self)
 *   - Process_free(self)                    : refused while inFlight (the Teardown Order Law)
 *
 * Setters:
 *   - Process_setEntry(self, entry)         : no-op while inFlight
 *   - Process_setHot(self, pin)
 *
 * Getters:
 *   - Process_getEntry(self)
 *   - Process_getHot(self)
 * ============================================================================
 */

// ;;INTENTION("Retire-pin lifecycle: when hot/hot.h retire handle is wired,
// Process_run pins the module before calling entry and retires after entry
// returns — the old module rides the retire ring until the call completes,
// and Process_setHot rebinds the entry on the next Process_run. Wire to
// HotModule / HotRetireRing via the existing hot_retire_handle seam.")

// CONSTRUCTORS
Process *Process_1(ProcessEntry entry) {
    Process *self = (Process*) calloc(1, sizeof(Process));
    if (!self)
        return NULL;
    (*self).entry = entry;
    (*self).hot = nullptr;
    atomic_init(&(*self).inFlight, false);
    (*self).invocationCount = 0;
    return self;
}

// CORE FUNCTIONS
int Process_run(Process *self, int argc, const char *const *argv) {
    if (!self)
        return -1;
    if (atomic_load_explicit(&(*self).inFlight, memory_order_acquire))
        return -1;
    ProcessEntry fn = (*self).entry;
    if (!fn)
        return -1;

    atomic_store_explicit(&(*self).inFlight, true, memory_order_release);
    int status = fn(argc, argv);
    atomic_store_explicit(&(*self).inFlight, false, memory_order_release);
    (*self).invocationCount++;
    return status;
}

bool Process_isRunning(const Process *self) {
    if (!self)
        return false;
    return atomic_load_explicit(&(*self).inFlight, memory_order_acquire);
}

uint32_t Process_getInvocationCount(const Process *self) {
    if (!self)
        return 0;
    return (*self).invocationCount;
}

void Process_free(Process *self) {
    if (!self)
        return;
    if (atomic_load_explicit(&(*self).inFlight, memory_order_acquire)) {
        fprintf(stderr, "process: refusing free while call is in flight\n");
        return;
    }
    (*self).entry = nullptr;
    (*self).hot = nullptr;
    free(self);
}

// SETTERS
void Process_setEntry(Process *self, ProcessEntry entry) {
    if (!self)
        return;
    if (atomic_load_explicit(&(*self).inFlight, memory_order_acquire))
        return;
    (*self).entry = entry;
}

void Process_setHot(Process *self, void *pin) {
    if (!self)
        return;
    (*self).hot = pin;
}

// GETTERS
ProcessEntry Process_getEntry(const Process *self) {
    if (!self)
        return nullptr;
    return (*self).entry;
}

void *Process_getHot(const Process *self) {
    if (!self)
        return nullptr;
    return (*self).hot;
}
