#include "kernel/kernel.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "annotation/intention.h"
#include "annotation/overview.h"
#include "vulkan/vk.h"
#include "window/window.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Kernel (kernel/kernel.c)
 * LEVEL: L4 — Self-Management (R1 Host; the Vertical Integration Law vs the Four System Levels Law: L = edit-risk, R = supervision)
 * ============================================================================
 * R1 Host Supervisor: STORAGE + DISPATCH, never an executor. Owns the master
 * session arena, the transient scratch arena, and the three per-kind
 * registries (processes / applications / consoles). Kernel_run is a thin
 * reference forward that hands each registered kind to its own run function;
 * the Kernel owns NO loop, NO tick, and NO worker thread. Frame scheduling,
 * the event pump, and presentation live in graphvex's GfxLoop (the Vertical
 * Integration Law / the Window Decoupling Law).
 *
 * STRUCT FIELDS (Mirroring kernel/kernel.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   MemoryArena *arena;                          // master session arena
 *   MemoryArena *transientArena;                 // per-event scratch arena
 *   Application *applications[KERNEL_MAX_APPS];  // windowed apps (opaque handles)
 *   uint32_t applicationCount;                    // used slots in applications[]
 *   Process     *processes[KERNEL_MAX_PROCS];     // one-shot invokables
 *   uint32_t processCount;                       // used slots in processes[]
 *   Console     *consoles[KERNEL_MAX_CONSOLES];  // session pumps
 *   uint32_t consoleCount;                       // used slots in consoles[]
 *   atomic_bool running;                          // terminal-run armed flag
 *   atomic_uintptr_t runThreadId;                 // arming (Thread 0) id; 0 = never armed
 *   pthread_mutex_t addLock;                      // guards the mailbox fields below
 *   KernelDeferred *deferred;                     // growable pending-add slots
 *   size_t deferredCount;                         // queued entries
 *   size_t deferredCap;                           // allocated slots
 *
 * PRIVATE HELPERS (kept file-local pure-data only, each with full fields):
 * ----------------------------------------------------------------------------
 *   KernelDeferred    // deferred-add mailbox slot (kernel.h): memory married
 *                     // to Kernel, ALL behavior lives in this file
 *     KernelDeferredKind kind;   // which registry the pending add targets
 *     void *ptr;                 // Application* / Process* / Console* (as void*)
 *
 *   Static wiring (no stored state): kernelRunActive, kernelPostDeferred,
 *   kernelApplyDeferred, kernelDrainDeferred, kernelAdd{Application,Process,
 *   Console}Internal, kernelAllDone, kernelStartApplication.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Kernel()                            : Kernel_0()
 *   - Kernel(arenaBytes)                  : Kernel_1(arenaBytes)
 *   - Kernel(arenaBytes, transientBytes)  : Kernel_2(arenaBytes, transientBytes)
 *
 * Core Functions:
 *   - Kernel_destroy(self)   : legacy shim over free
 *   - Kernel_free(self)
 *   - Kernel_stop(self)      : stop apps, cancel consoles, clear running
 *   - Kernel_isRunning(self) : terminal-run armed?
 *   - Kernel_runAll(self)    : completion reactor (arm -> dispatch -> supervise
 *                              -> drain deferred -> disarm)
 *   - Kernel_runProcess(self, p)     : forward to Process_run
 *   - Kernel_runConsole(self, c)     : forward to Console_run
 *   - Kernel_runApplication(self, a) : start + forward to graphvex GfxLoop
 *   - Kernel_run(...)         (arity macro: 1 arg -> runAll, 2 args -> dispatch by type)
 *   - Kernel_addApplication / removeApplication / getters (add* honors the
 *     Terminal-Run Contract: armed-thread refusal + off-thread mailbox post)
 *   - Kernel_addProcess / removeProcess / getters
 *   - Kernel_addConsole / removeConsole / getters
 *
 * Getters:
 *   - Kernel_getArena(self)
 *   - Kernel_getTransientArena(self)
 * ============================================================================
 */

;;INTENTION("Kernel struct is calloc-owned like Application_0; "
            "arenas are MemoryArena-owned. Migrating the struct itself"
            " into arena storage happens once the registries move to doubling arena slabs per the Dynamic Scalability & Anti-Hardcoding Law — keeps teardown order provable today per the Conflict Triage Law.")

;;INTENTION("Kernel_runApplication starts the app, shows its windows, then"
            " blocks in Application_run's keep-alive parked loop until every "
            "window closes. graphvex's GfxLoop frame scheduler lands later and "
            "layers on top — registering windows into the loop, driving the "
            "event pump, present-on-demand, and telemetry. The Kernel never "
            "owns the loop either way; hotcwap's Application owns the window "
            "end.")

;;INTENTION("The Terminal-Run Contract (the Conflict Triage Law): Kernel_runAll"
            " ARMS the kernel — running=true + runThreadId set on the arming"
            " thread. After arming, Thread-0 registration is REFUSED with one"
            " stderr warn: the run loop applies only what it was given before"
            " run, so the arming thread cannot re-enter or mutate the registry"
            " it is currently iterating (the Tier-1 thread-safety half). Other"
            " threads may still register via the deferred-add mailbox (mutex-"
            " guarded, growable realloc — a COLD path, never a steady-state"
            " allocation), drained on Thread 0 by the run loop's next pass, so"
            " a hot-reloaded module / worker can spawn an Application mid-run"
            " on its own schedule. The registries are never copied to the"
            " mailbox — the mailbox FEEDS the same arrays on the running"
            " thread, keeping all registry mutation single-threaded.")


// CONSTRUCTORS
Kernel *Kernel_0(void) {
    return Kernel_2(KERNEL_ARENA_DEFAULT, KERNEL_TRANSIENT_DEFAULT);
}

Kernel *Kernel_1(size_t arenaBytes) {
    return Kernel_2(arenaBytes, KERNEL_TRANSIENT_DEFAULT);
}

Kernel *Kernel_2(size_t arenaBytes, size_t transientBytes) {
    Kernel *self = (Kernel*) calloc(1, sizeof(Kernel));
    if (!self)
        return NULL;
    MemoryArena *arena = MemoryArena_create(arenaBytes);
    if (!arena) {
        free(self);
        return NULL;
    }
    MemoryArena *scratch = MemoryArena_create(transientBytes);
    if (!scratch) {
        MemoryArena_destroy(arena);
        free(self);
        return NULL;
    }
    (*self).arena = arena;
    (*self).transientArena = scratch;
    (*self).applicationCount = 0;
    (*self).processCount = 0;
    (*self).consoleCount = 0;
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    atomic_store_explicit(&(*self).runThreadId, (uintptr_t) 0, memory_order_relaxed);
    pthread_mutex_init(&(*self).addLock, NULL);
    (*self).deferred = NULL;
    (*self).deferredCount = 0;
    (*self).deferredCap = 0;
    return self;
}

// CORE FUNCTIONS
bool Kernel_free(Kernel *self) {
    if (!self)
        return false;
    if ((*self).applicationCount > 0 || (*self).processCount > 0 || (*self).consoleCount > 0) {
        fprintf(stderr,
                "kernel: %u app(s), %u process(es), %u console(s) still registered; remove before free\n",
                (*self).applicationCount, (*self).processCount, (*self).consoleCount);
        return false;
    }
    Kernel_stop(self);

    if (Vk_ready())
        Vk_shutdown();

    pthread_mutex_destroy(&(*self).addLock);
    free((*self).deferred);
    (*self).deferred = NULL;
    (*self).deferredCount = 0;
    (*self).deferredCap = 0;

    for (uint32_t i = 0; i < KERNEL_MAX_APPS; i++)
        (*self).applications[i] = NULL;
    (*self).applicationCount = 0;
    for (uint32_t i = 0; i < KERNEL_MAX_PROCS; i++)
        (*self).processes[i] = NULL;
    (*self).processCount = 0;
    for (uint32_t i = 0; i < KERNEL_MAX_CONSOLES; i++)
        (*self).consoles[i] = NULL;
    (*self).consoleCount = 0;

    MemoryArena *scratch = (*self).transientArena;
    MemoryArena *arena = (*self).arena;
    (*self).transientArena = NULL;
    (*self).arena = NULL;
    if (scratch)
        MemoryArena_destroy(scratch);
    if (arena)
        MemoryArena_destroy(arena);
    free(self);
    return true;
}

void Kernel_destroy(Kernel *self) {
    if (!self)
        return;
    (void) Kernel_free(self);
}

void Kernel_stop(Kernel *self) {
    if (!self)
        return;
    // Ends a live Kernel_runAll immediately: the reactor loop re-checks
    // running each pass and exits. Clearing running FIRST (before the app/
    // console stop passes below) races nothing — the loop is the only reader
    // and it reads running on the arming thread.
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *app = (*self).applications[i];
        if (app)
            Application_stop(app);
    }
    for (uint32_t i = 0; i < (*self).consoleCount; i++) {
        Console *c = (*self).consoles[i];
        if (c)
            Console_cancel(c);
    }
}

bool Kernel_isRunning(const Kernel *self) {
    if (!self)
        return false;
    return atomic_load_explicit(&(*self).running, memory_order_relaxed);
}

// --- REGISTRY INTERNALS + DEFERRED MAILBOX -----------------------------------
// The three Kernel_add* entry points split into (a) the Terminal-Run guard
// and (b) apply-internal peers that touch ONLY the registry arrays. The
// guard lives at the public seam; the apply-internal peers are the sole
// writers of the arrays. During a live run only the arming thread mutates
// the registries (via drain), keeping iteration single-threaded per the
// Tier-1 thread-safety half of the ;;INTENTION above.

static bool kernelRunActive(const Kernel *self) {
    return atomic_load_explicit(&(*self).running, memory_order_relaxed);
}

// Forward: start one app manifest non-blocking (defined below with the run
// helpers; kernelApplyDeferred admits deferred adds and starts them like the
// initial pass would).
static void kernelStartApplication(Kernel *self, Application *a);

static bool kernelAddApplicationInternal(Kernel *self, Application *app) {
    for (uint32_t i = 0; i < (*self).applicationCount; i++)
        if ((*self).applications[i] == app)
            return false;
    if ((*self).applicationCount >= KERNEL_MAX_APPS)
        return false;
    (*self).applications[(*self).applicationCount++] = app;
    return true;
}

static bool kernelAddProcessInternal(Kernel *self, Process *p) {
    for (uint32_t i = 0; i < (*self).processCount; i++)
        if ((*self).processes[i] == p)
            return false;
    if ((*self).processCount >= KERNEL_MAX_PROCS)
        return false;
    (*self).processes[(*self).processCount++] = p;
    return true;
}

static bool kernelAddConsoleInternal(Kernel *self, Console *c) {
    for (uint32_t i = 0; i < (*self).consoleCount; i++)
        if ((*self).consoles[i] == c)
            return false;
    if ((*self).consoleCount >= KERNEL_MAX_CONSOLES)
        return false;
    (*self).consoles[(*self).consoleCount++] = c;
    return true;
}

// Post a pending add from ANY thread (mutex-guarded, growable realloc — cold
// path, never steady-state). The drain on Thread 0 steals the whole batch in
// one lock and re-owns the arrays for the rest of the pass.
static bool kernelPostDeferred(Kernel *self, KernelDeferredKind kind, void *ptr) {
    pthread_mutex_lock(&(*self).addLock);
    if ((*self).deferredCount == (*self).deferredCap) {
        size_t cap = (*self).deferredCap ? (*self).deferredCap * 2 : 4;
        KernelDeferred *nx = (KernelDeferred*) realloc((*self).deferred, cap * sizeof(KernelDeferred));
        if (!nx) {
            pthread_mutex_unlock(&(*self).addLock);
            fprintf(stderr, "kernel: deferred-add mailbox grow failed (Kernel_add* dropped)\n");
            return false;
        }
        (*self).deferred = nx;
        (*self).deferredCap = cap;
    }
    (*self).deferred[(*self).deferredCount].kind = kind;
    (*self).deferred[(*self).deferredCount].ptr = ptr;
    (*self).deferredCount++;
    pthread_mutex_unlock(&(*self).addLock);
    return true;
}

// Admit one pending add into the registry on the arming thread, then START
// it exactly like the initial pass would: processes invoke one-shot, consoles
// spawn their session, applications start + show their windows. A kind that
// fails the dup/full/guards stays unregistered and silent.
static void kernelApplyDeferred(Kernel *self, KernelDeferredKind kind, void *ptr) {
    switch (kind) {
        case KERNEL_DEFERRED_APPLICATION: {
            Application *a = (Application*) ptr;
            if (kernelAddApplicationInternal(self, a))
                kernelStartApplication(self, a);
            break;
        }
        case KERNEL_DEFERRED_PROCESS: {
            Process *p = (Process*) ptr;
            if (kernelAddProcessInternal(self, p))
                (void) Process_run(p, 0, nullptr);
            break;
        }
        case KERNEL_DEFERRED_CONSOLE: {
            Console *c = (Console*) ptr;
            if (kernelAddConsoleInternal(self, c))
                (void) Console_run(c);
            break;
        }
        case KERNEL_DEFERRED_NONE:
        default:
            break;
    }
}

// Steal the whole pending batch under the lock, then apply each entry OUTSIDE
// the lock — the run loop keeps iterating its own registry, never the mailbox.
static void kernelDrainDeferred(Kernel *self) {
    pthread_mutex_lock(&(*self).addLock);
    KernelDeferred *batch = (*self).deferred;
    size_t n = (*self).deferredCount;
    (*self).deferred = NULL;
    (*self).deferredCount = 0;
    (*self).deferredCap = 0;
    pthread_mutex_unlock(&(*self).addLock);

    for (size_t i = 0; i < n; i++)
        kernelApplyDeferred(self, batch[i].kind, batch[i].ptr);

    if (batch)
        free(batch);
}

// Every registered kind reports done? Processes are done by construction
// after the initial invoke pass (one-shot); consoles when their session
// joined (Console_isRunning false); apps per the Application_isFinished
// completion predicate (all windows closed or externally stopped).
static bool kernelAllDone(const Kernel *self) {
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *a = (*self).applications[i];
        if (a && !Application_isFinished(a))
            return false;
    }
    for (uint32_t i = 0; i < (*self).consoleCount; i++) {
        Console *c = (*self).consoles[i];
        if (c && Console_isRunning(c))
            return false;
    }
    return true;
}

// Start one app manifest NON-blocking: flip running, show every window. The
// reactor supervises it to completion; the blocking Kernel_runApplication
// path adds Application_run on top.
static void kernelStartApplication(Kernel *self, Application *a) {
    (void) self;
    if (!a)
        return;
    Application_start(a);
    uint32_t winCount = Application_getWindowCount(a);
    for (uint32_t i = 0; i < winCount; i++) {
        Window *w = Application_getWindow(a, i);
        if (w)
            Window_show(w);
    }
}

// --- WORK DISPATCH ----------------------------------------------------------
// The Kernel forwards work, it never does it: each registered kind is relayed
// to its own run function. The frame loop, event pump, presentation, and
// per-frame handler invocation live in graphvex's GfxLoop (the Vertical
// Integration Law / the Window Decoupling Law). Kernel_runAll is the ONE
// supervising entry: it ARMS (the Terminal-Run Contract), dispatches the
// initial registrations, then runs a completion reactor until every kind is
// done or Kernel_stop() lands.

int Kernel_runAll(Kernel *self) {
    if (!self)
        return KERNEL_EXIT_NO_APPS;
    if ((*self).applicationCount == 0 && (*self).processCount == 0 && (*self).consoleCount == 0)
        return KERNEL_EXIT_NO_APPS;

    // TERMINAL-RUN ARM: from here the arming (Thread 0) thread can no longer
    // register — all Kernel_add* on it are refused; other threads post into
    // the deferred mailbox, drained on the next pass below.
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);
    atomic_store_explicit(&(*self).runThreadId, (uintptr_t) pthread_self(), memory_order_relaxed);

    int rc = KERNEL_EXIT_OK;

    // One-shot invokables: run to completion on the arming thread.
    for (uint32_t i = 0; i < (*self).processCount; i++) {
        Process *p = (*self).processes[i];
        if (p) {
            int r = Process_run(p, 0, nullptr);
            if (r != 0 && rc == KERNEL_EXIT_OK)
                rc = r;
        }
    }

    // Session pumps + windowed apps: start without blocking — the reactor
    // below supervises them to completion.
    for (uint32_t i = 0; i < (*self).consoleCount; i++) {
        Console *c = (*self).consoles[i];
        if (c)
            (void) Console_run(c);
    }
    for (uint32_t i = 0; i < (*self).applicationCount; i++)
        kernelStartApplication(self, (*self).applications[i]);

    // COMPLETION REACTOR: drain deferred adds, pump OS events, supervise
    // bounded console slices, ask app closed-state at a ~250ms cadence. 5ms
    // per pass keeps teardown responsive (the Bounded Wait Law) without a
    // busy spin.
    const struct timespec park = {0, 5000000L};
    uint64_t pass = 0;
    while (atomic_load_explicit(&(*self).running, memory_order_relaxed) && !kernelAllDone(self)) {
        kernelDrainDeferred(self);

        Window_pollEvents();

        char buf[4096];
        for (uint32_t i = 0; i < (*self).consoleCount; i++) {
            Console *c = (*self).consoles[i];
            if (!c || !Console_isRunning(c))
                continue;
            size_t n = 0;
            if (Console_poll(c, buf, sizeof(buf), &n) && n > 0)
                (void) fwrite(buf, 1, n, stdout);
        }

        if ((pass % 50) == 0) {
            for (uint32_t i = 0; i < (*self).applicationCount; i++) {
                Application *a = (*self).applications[i];
                if (a && Application_isFinished(a))
                    Application_stop(a);
            }
        }

        nanosleep(&park, NULL);
        pass++;
    }

    // Reactor exit: stop every kind still live so nothing outlives the run
    // (mirrors Kernel_stop minus the external cancel), then best-effort-drain
    // straggler deferred adds so the mailbox is empty, then disarm.
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *a = (*self).applications[i];
        if (a && Application_isRunning(a))
            Application_stop(a);
    }
    kernelDrainDeferred(self);

    atomic_store_explicit(&(*self).runThreadId, (uintptr_t) 0, memory_order_relaxed);
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);

    return rc;
}

int Kernel_runProcess(Kernel *self, Process *p) {
    (void) self;
    return p ? Process_run(p, 0, nullptr) : KERNEL_EXIT_NO_APPS;
}

bool Kernel_runConsole(Kernel *self, Console *c) {
    (void) self;
    return c ? Console_run(c) : false;
}

int Kernel_runApplication(Kernel *self, Application *a) {
    if (!self || !a)
        return KERNEL_EXIT_NO_APPS;
    kernelStartApplication(self, a);

    // Keep the app alive on its own: the blocking Application_run parked loop
    // (hotcwap's own, graphvex-independent) lives until EVERY window is
    // closed, then returns. graphvex's GfxLoop frame scheduler lands later
    // and layers on top of this keep-alive — the Kernel never owns the loop
    // either way.
    Application_run(a);
    return KERNEL_EXIT_OK;
}

// --- APPLICATION REGISTRY ---
// Registration is the PRE-arm contract: while Kernel_runAll is live, the
// arming thread's adds are refused (one warn) and other threads' adds post
// to the deferred mailbox, admitted on the running thread's next pass.
bool Kernel_addApplication(Kernel *self, Application *app) {
    if (!self || !app)
        return false;
    if (kernelRunActive(self)) {
        if ((uintptr_t) pthread_self() == atomic_load_explicit(&(*self).runThreadId, memory_order_relaxed)) {
            fprintf(stderr, "kernel: Kernel_addApplication refused on the arming thread while Kernel_run is live — register before Kernel_run(), or post from another thread\n");
            return false;
        }
        return kernelPostDeferred(self, KERNEL_DEFERRED_APPLICATION, app);
    }
    return kernelAddApplicationInternal(self, app);
}

bool Kernel_removeApplication(Kernel *self, Application *app) {
    if (!self || !app)
        return false;
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        if ((*self).applications[i] == app) {
            (*self).applications[i] = (*self).applications[--(*self).applicationCount];
            (*self).applications[(*self).applicationCount] = NULL;
            return true;
        }
    }
    return false;
}

// --- PROCESS REGISTRY ---
bool Kernel_addProcess(Kernel *self, Process *p) {
    if (!self || !p)
        return false;
    if (kernelRunActive(self)) {
        if ((uintptr_t) pthread_self() == atomic_load_explicit(&(*self).runThreadId, memory_order_relaxed)) {
            fprintf(stderr, "kernel: Kernel_addProcess refused on the arming thread while Kernel_run is live — register before Kernel_run(), or post from another thread\n");
            return false;
        }
        return kernelPostDeferred(self, KERNEL_DEFERRED_PROCESS, p);
    }
    return kernelAddProcessInternal(self, p);
}

bool Kernel_removeProcess(Kernel *self, Process *p) {
    if (!self || !p)
        return false;
    for (uint32_t i = 0; i < (*self).processCount; i++) {
        if ((*self).processes[i] == p) {
            (*self).processes[i] = (*self).processes[--(*self).processCount];
            (*self).processes[(*self).processCount] = NULL;
            return true;
        }
    }
    return false;
}

// --- CONSOLE REGISTRY ---
bool Kernel_addConsole(Kernel *self, Console *c) {
    if (!self || !c)
        return false;
    if (kernelRunActive(self)) {
        if ((uintptr_t) pthread_self() == atomic_load_explicit(&(*self).runThreadId, memory_order_relaxed)) {
            fprintf(stderr, "kernel: Kernel_addConsole refused on the arming thread while Kernel_run is live — register before Kernel_run(), or post from another thread\n");
            return false;
        }
        return kernelPostDeferred(self, KERNEL_DEFERRED_CONSOLE, c);
    }
    return kernelAddConsoleInternal(self, c);
}

bool Kernel_removeConsole(Kernel *self, Console *c) {
    if (!self || !c)
        return false;
    for (uint32_t i = 0; i < (*self).consoleCount; i++) {
        if ((*self).consoles[i] == c) {
            (*self).consoles[i] = (*self).consoles[--(*self).consoleCount];
            (*self).consoles[(*self).consoleCount] = NULL;
            return true;
        }
    }
    return false;
}

// GETTERS
Application *Kernel_getApplication(const Kernel *self, uint32_t index) {
    if (!self)
        return NULL;
    if (index >= (*self).applicationCount)
        return NULL;
    return (*self).applications[index];
}

uint32_t Kernel_getApplicationCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).applicationCount;
}

uint32_t Kernel_getApplications(const Kernel *self, Application **out, uint32_t cap) {
    if (!self || !out || cap == 0)
        return 0;
    uint32_t n = (*self).applicationCount;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).applications[i];
    return n;
}

Process *Kernel_getProcess(const Kernel *self, uint32_t index) {
    if (!self)
        return NULL;
    if (index >= (*self).processCount)
        return NULL;
    return (*self).processes[index];
}

uint32_t Kernel_getProcessCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).processCount;
}

uint32_t Kernel_getProcesses(const Kernel *self, Process **out, uint32_t cap) {
    if (!self || !out || cap == 0)
        return 0;
    uint32_t n = (*self).processCount;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).processes[i];
    return n;
}

Console *Kernel_getConsole(const Kernel *self, uint32_t index) {
    if (!self)
        return NULL;
    if (index >= (*self).consoleCount)
        return NULL;
    return (*self).consoles[index];
}

uint32_t Kernel_getConsoleCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).consoleCount;
}

uint32_t Kernel_getConsoles(const Kernel *self, Console **out, uint32_t cap) {
    if (!self || !out || cap == 0)
        return 0;
    uint32_t n = (*self).consoleCount;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).consoles[i];
    return n;
}

MemoryArena *Kernel_getArena(const Kernel *self) {
    if (!self)
        return NULL;
    return (*self).arena;
}

MemoryArena *Kernel_getTransientArena(const Kernel *self) {
    if (!self)
        return NULL;
    return (*self).transientArena;
}