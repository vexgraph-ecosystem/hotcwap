#include "kernel/process.h"

#include <stdlib.h>

#include "annotation/overview.h"

;;OVERVIEW
/**
 * CLASS: Process (kernel/process.c)
 * LEVEL: L2 — Behavior (one replaceable caller-thread entry)
 *
 * STRUCT FIELDS (kernel/process.h):
 *   _Atomic(ProcessEntry) entry;   // current callback
 *   _Atomic(void*) context;       // borrowed callback data
 *   _Atomic(void*) hot;           // borrowed association, NOT a module pin
 *   _Atomic(const char*) name;    // borrowed immutable label
 *   atomic_bool occupied;        // non-waiting admission gate
 *   atomic_bool inFlight;        // callback execution telemetry
 *   atomic_uint invocationCount; // completed callbacks
 *
 * CONSTRUCTORS: Process(entry), Process(entry, context),
 *   Process(name, entry, context) dispatch to Process_1 / Process_2 / Process_3.
 * CORE: Process_run(self, exitStatus), Process_free(self).
 * SETTERS: Process_replace(self, entry, context, hot), Process_setName(self, name).
 * GETTERS: Process_getEntry, Process_getContext, Process_getHot, Process_getName,
 *   Process_isRunning, Process_getInvocationCount.
 * PRIVATE HELPERS: none.
 *
 * Run and replacement claim the same gate once, never spin or wait. Failed
 * admission leaves output/binding unchanged. Entry/context/hot mutate together
 * under the gate; individual getters are atomic observations, not a snapshot.
 * Binding setters are deliberately combined to preserve their relationship.
 * Telemetry is read-only. Null entry is rejected; null context/name/hot allowed.
 * Borrowed storage/code outlives its uses; external exclusion is required at
 * free. Callbacks must return normally (no longjmp/thread exit across this API).
 * Generation pinning and unloading remain a loader integration task.
 */

// CONSTRUCTORS
Process *Process_1(ProcessEntry entry) {
    return Process_3(nullptr, entry, nullptr);
}

Process *Process_2(ProcessEntry entry, void *context) {
    return Process_3(nullptr, entry, context);
}

Process *Process_3(const char *name, ProcessEntry entry, void *context) {
    if (!entry)
        return nullptr;
    Process *self = (Process*) calloc(1, sizeof(Process));
    if (!self)
        return nullptr;
    atomic_init(&(*self).entry, entry);
    atomic_init(&(*self).context, context);
    atomic_init(&(*self).hot, nullptr);
    atomic_init(&(*self).name, name);
    atomic_init(&(*self).occupied, false);
    atomic_init(&(*self).inFlight, false);
    atomic_init(&(*self).invocationCount, 0);
    return self;
}

// CORE FUNCTIONS
ProcessResult Process_run(Process *self, int *exitStatus) {
    if (!self || !exitStatus)
        return PROCESS_INVALID;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&(*self).occupied, &expected, true,
                                                 memory_order_acquire, memory_order_relaxed))
        return PROCESS_BUSY;
    ProcessEntry entry = atomic_load_explicit(&(*self).entry, memory_order_relaxed);
    void *context = atomic_load_explicit(&(*self).context, memory_order_relaxed);
    atomic_store_explicit(&(*self).inFlight, true, memory_order_release);
    *exitStatus = (*entry)(context);
    atomic_fetch_add_explicit(&(*self).invocationCount, 1, memory_order_relaxed);
    atomic_store_explicit(&(*self).inFlight, false, memory_order_release);
    atomic_store_explicit(&(*self).occupied, false, memory_order_release);
    return PROCESS_OK;
}

bool Process_free(Process *self) {
    if (!self)
        return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&(*self).occupied, &expected, true,
                                                 memory_order_acquire, memory_order_relaxed))
        return false;
    free(self);
    return true;
}

// SETTERS
ProcessResult Process_replace(Process *self, ProcessEntry entry, void *context, void *hot) {
    if (!self || !entry)
        return PROCESS_INVALID;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&(*self).occupied, &expected, true,
                                                 memory_order_acquire, memory_order_relaxed))
        return PROCESS_BUSY;
    atomic_store_explicit(&(*self).entry, entry, memory_order_relaxed);
    atomic_store_explicit(&(*self).context, context, memory_order_relaxed);
    atomic_store_explicit(&(*self).hot, hot, memory_order_relaxed);
    atomic_store_explicit(&(*self).occupied, false, memory_order_release);
    return PROCESS_OK;
}

ProcessResult Process_setName(Process *self, const char *name) {
    if (!self)
        return PROCESS_INVALID;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&(*self).occupied, &expected, true,
                                                 memory_order_acquire, memory_order_relaxed))
        return PROCESS_BUSY;
    atomic_store_explicit(&(*self).name, name, memory_order_relaxed);
    atomic_store_explicit(&(*self).occupied, false, memory_order_release);
    return PROCESS_OK;
}

// GETTERS
bool Process_isRunning(const Process *self) {
    return self ? atomic_load_explicit(&(*self).inFlight, memory_order_acquire) : false;
}

uint32_t Process_getInvocationCount(const Process *self) {
    return self ? atomic_load_explicit(&(*self).invocationCount, memory_order_relaxed) : 0;
}

ProcessEntry Process_getEntry(const Process *self) {
    return self ? atomic_load_explicit(&(*self).entry, memory_order_relaxed) : nullptr;
}

void *Process_getContext(const Process *self) {
    return self ? atomic_load_explicit(&(*self).context, memory_order_relaxed) : nullptr;
}

void *Process_getHot(const Process *self) {
    return self ? atomic_load_explicit(&(*self).hot, memory_order_relaxed) : nullptr;
}

const char *Process_getName(const Process *self) {
    return self ? atomic_load_explicit(&(*self).name, memory_order_relaxed) : nullptr;
}
