#include "kernel/process.h"

#include <stdlib.h>

#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Process
 * ============================================================================
 * Represents a single, replaceable caller-thread execution entry. Serves as
 * the minimal invocation abstraction for one-shot CLI tasks or hot-swappable
 * discrete procedures, decoupling function execution from thread management.
 *
 * Process maintains atomic references to its entry function, borrowed context,
 * optional hot-reload module association, and execution telemetry. An atomic
 * occupancy gate ensures that executions and runtime replacements never race
 * or wait, failing fast with PROCESS_BUSY when contended.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Process (kernel/process.c)
 * LEVEL: L2 — Behavior (one replaceable caller-thread entry)
 * ============================================================================
 * SUMMARY:
 *   One caller-thread invocation at a time. Name, context, and hot association
 *   are borrowed; code and data must remain live while referenced. No module
 *   pin, thread, or cancellation mechanism is created here.
 *
 * STRUCT FIELDS (Mirroring kernel/process.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   _Atomic(ProcessEntry) entry;   // current callback
 *   _Atomic(void*) context;        // borrowed callback data
 *   _Atomic(void*) hot;            // borrowed association, NOT a module pin
 *   _Atomic(const char*) name;     // borrowed immutable label
 *   atomic_bool occupied;          // non-waiting admission gate
 *   atomic_bool inFlight;          // callback execution telemetry
 *   atomic_uint invocationCount;   // completed callbacks
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Process(entry)                          : Process_1(entry)
 *   - Process(entry, context)                 : Process_2(entry, context)
 *   - Process(name, entry, context)           : Process_3(name, entry, context)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Process_run(self, exitStatus)           : Claim gate and run entry callback
 *   - Process_free(self)                      : Claim gate and free memory
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - Process_replace(self, entry, ctx, hot)  : Atomic multi-field binding replace
 *   - Process_setName(self, name)             : Atomic label assignment
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Process_isRunning(self)                 : Telemetry in-flight observation
 *   - Process_getInvocationCount(self)        : Telemetry completed callback count
 *   - Process_getEntry(self)                  : Current entry function pointer
 *   - Process_getContext(self)                : Current context pointer
 *   - Process_getHot(self)                    : Current hot module association
 *   - Process_getName(self)                   : Current process name
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// CONSTRUCTORS (PUBLIC & PRIVATE)
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

// CORE FUNCTIONS (PUBLIC & PRIVATE)
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

// SETTERS (PUBLIC & PRIVATE)
;;SETTER
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

;;SETTER
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

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
bool Process_isRunning(const Process *self) {
    return self ? atomic_load_explicit(&(*self).inFlight, memory_order_acquire) : false;
}

;;GETTER
uint32_t Process_getInvocationCount(const Process *self) {
    return self ? atomic_load_explicit(&(*self).invocationCount, memory_order_relaxed) : 0;
}

;;GETTER
ProcessEntry Process_getEntry(const Process *self) {
    return self ? atomic_load_explicit(&(*self).entry, memory_order_relaxed) : nullptr;
}

;;GETTER
void *Process_getContext(const Process *self) {
    return self ? atomic_load_explicit(&(*self).context, memory_order_relaxed) : nullptr;
}

;;GETTER
void *Process_getHot(const Process *self) {
    return self ? atomic_load_explicit(&(*self).hot, memory_order_relaxed) : nullptr;
}

;;GETTER
const char *Process_getName(const Process *self) {
    return self ? atomic_load_explicit(&(*self).name, memory_order_relaxed) : nullptr;
}

