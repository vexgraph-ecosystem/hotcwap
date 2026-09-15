#ifndef HOT_PROCESS_PROCESS_H
#define HOT_PROCESS_PROCESS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// kernel/process.h — One-shot hot-loadable function wrapper.
//
// A Process wraps a single `main`-shaped entry (ProcessEntry): invoke it
// on the caller's thread, it runs to completion, returns an exit status.
// Re-runnable ("run it back"); the hot module is retired (not unloaded) while
// a call is in flight (the Conflict Triage Law).
//
// Lifecycle:
//   Process(entry) -> Process_run(self, argc, argv) -> Process_run(...) -> Process_free
//
// Never ticked.  Never owns a window, a thread, or a socket.

typedef struct Process Process;

typedef int (*ProcessEntry)(int argc, const char *const *argv);

struct Process {
    ProcessEntry entry;             // hot-bound entry fn (null = unbound)
    void *hot;                      // opaque retire pin (the Conflict Triage Law; set after hot-load)
    _Atomic bool inFlight;          // re-entrancy guard (one invoke at a time)
    uint32_t invocationCount;       // completed invocations
};

// --- Overloaded constructors ---
//
//   Process(entry)  -> one-shot fn wrapper
//
Process *Process_1(ProcessEntry entry);

#define PROCESS_CHOOSER(_0, _1, NAME, ...) NAME

#define Process(...) PROCESS_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    Process_1 \
)(__VA_ARGS__)

// Free the Process. Refused (warn + return) while a call is in flight
// (the Teardown Order Law / the Cold-Strict, Hot-Minimal Validation Law). Null-safe.
void Process_free(Process *self);

// --- Core ---
// Invoke the wrapped entry on the caller's thread. Returns the entry's
// exit status, or -1 on null self / null entry / re-entrant call (the Cold-Strict, Hot-Minimal Validation Law).
int  Process_run(Process *self, int argc, const char *const *argv);

bool Process_isRunning(const Process *self);
uint32_t Process_getInvocationCount(const Process *self);

// --- Setters / Getters (the Symmetric Getter/Setter Completeness Law, the Living `;;OVERVIEW` Blueprint Law contract) ---
// setEntry: no-op while inFlight. getEntry: null-safe (returns null).
void        Process_setEntry(Process *self, ProcessEntry entry);
ProcessEntry Process_getEntry(const Process *self);

// Hot module pin (void* — no hot.h include; wiring via the Conflict Triage Law seam).
void  Process_setHot(Process *self, void *pin);
void *Process_getHot(const Process *self);

#endif
