#include "kernel/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "annotation/overview.h"
#include "hot/spv_watch.h"
#include "input/key.h"
#include "input/mouse.h"
#include "oop/type.h"
#include "time/nanotime.h"
#include "vulkan/vk.h"
#include "vulkan/vk_layer.h"
#include "vulkan/vk_pane.h"
#include "window/window.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Kernel (kernel/kernel.c)
 * LEVEL: L4 — Self-Management (R0 Supervisor; Rule 17 vs Rule 28: L = edit-risk, R = supervision)
 * ============================================================================
 * R0 Host Supervisor: the thin nano-VM. Owns the master session arena, the
 * transient per-tick scratch arena, and the Application registry (N apps x M
 * windows per process). Boots first, tears down last. Holds windows stable
 * across HotModule swaps; knows nothing about darling widgets, api-haven
 * schemas, database drivers, language grammars, or engines.
 *
 * STRUCT FIELDS (Mirroring kernel/kernel.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   MemoryArena *arena;                          // master session arena
 *   MemoryArena *transientArena;                  // per-tick scratch arena
 *   Application *applications[KERNEL_MAX_APPS];   // registered apps (opaque handles)
 *   uint32_t applicationCount;                    // used slots in applications[]
 *   _Atomic bool running;                         // supervisor active flag
 *   Thread *presentWorker;                        // present thread (board + panes)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   kernel_present_job(thread, task)   // worker loop: Vk_clearPresent then
 *                                      // VkPane_presentAll while running (the
 *                                      // two-thread live-resize contract: thread
 *                                      // 0 pumps events, this thread keeps
 *                                      // presenting/animating during drags).
 *                                      // Pacing: fence-paced healthy path,
 *                                      // budget-paced every path, never bare spin.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Kernel()                            : Kernel_0()
 *   - Kernel(arenaBytes)                  : Kernel_1(arenaBytes)
 *   - Kernel(arenaBytes, transientBytes)  : Kernel_2(arenaBytes, transientBytes)
 *
 * Core Functions:
 *   - Kernel_destroy(self)
 *   - Kernel_free(self)
 *   - Kernel_stop(self)
 *   - Kernel_isRunning(self)
 *   - Kernel_runAll(self)
 *   - Kernel_runOne(self, app)
 *   - Kernel_run(...)                 (arity macro: 1 arg -> runAll, 2 args -> runOne)
 *   - Kernel_tick(self, dt)
 *   - Kernel_addApplication(self, app)
 *   - Kernel_removeApplication(self, app)
 *
 * Getters:
 *   - Kernel_getApplication(self, index)
 *   - Kernel_getApplicationCount(self)
 *   - Kernel_getApplications(self, out, cap)
 *   - Kernel_getArena(self)
 *   - Kernel_getTransientArena(self)
 * ============================================================================
 */

// ;;INTENTION("Phase-1 Kernel struct is calloc-owned like Application_0; arenas are MemoryArena-owned. Migrating the struct itself into arena storage happens once multi-app Kernel_run multiplexing lands — keeps teardown order provable today per Rule 33.")


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
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);
    return self;
}

// CORE FUNCTIONS
bool Kernel_free(Kernel *self) {
    if (!self)
        return false;
    if ((*self).applicationCount > 0) {
        fprintf(stderr, "kernel: %u applications still registered; remove before free\n",
                (*self).applicationCount);
        return false;
    }
    Kernel_stop(self);

    // Bounded wait for worker threads to observe running = false (Rule 27)
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, nullptr);

    if (Vk_ready())
        Vk_shutdown();

    for (uint32_t i = 0; i < (*self).applicationCount; i++)
        (*self).applications[i] = NULL;
    (*self).applicationCount = 0;

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
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *app = (*self).applications[i];
        if (app)
            Application_stop(app);
    }
}

bool Kernel_isRunning(const Kernel *self) {
    if (!self)
        return false;
    return atomic_load_explicit(&(*self).running, memory_order_relaxed);
}

// --- PRESENT WORKER (thread-1 GUI mode) -----------------------------------
// The two-thread live-resize contract: thread 0 owns the OS event pump and
// the AppKit live-resize tracking loop, and this worker owns ALL
// presentation — demand propagation, retained layers, board swapchain first,
// then every pane chain — so scenes KEEP ANIMATING while the user drags the
// window (thread 0 is inside the modal tracking loop; its tick cannot
// present). Demand is re-armed here every poll (bool stores only, Rule 35
// hot-minimal), so immediate-on-demand survives a stalled tick.
// Pacing: fence-paced healthy path, budget-paced every path, never bare spin.
// Clean chains skip inside VkPane_presentAll, so idle rests at 0 presents
// while the poll itself stays cheap. The sleep executes OUTSIDE any
// Vk_ready() guard so the worker yields CPU to thread 0 even when Vulkan is
// not ready.
// Thread 1: Retained scene manager worker.
// Runs offscreen scene rendering (VkLayer_visit) for retained scenes,
// allowing scenes to update continuously in the background.
static void kernel_present_job(Thread *selfThread, void *task) {
    (void) selfThread;
    Kernel *self = (Kernel*) task;
    if (!self)
        return;

    const uint64_t frameBudgetNs = 16666667ULL; // ~60fps
    const uint64_t minSleepNs = 1000000ULL;     // 1ms floor

    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (Vk_ready()) {
            Window *w = nullptr;
            if ((*self).applicationCount > 0 && (*self).applications[0] != nullptr)
                w = Application_getWindow((*self).applications[0], 0);
            bool sceneAdvanced = false;
            if (w != nullptr) {
                Panel *content = Window_getContentPanel(w);
                if (content != nullptr) {
                    extern void Darling_propagatePaneDirty(Window *window, Panel *contentPanel);
                    Darling_propagatePaneDirty(w, content);
                }
                // Render dirty retained scene targets offscreen
                sceneAdvanced = VkLayer_visit();
            }
#ifdef __APPLE__
            // Explicit per-walk transaction: the worker owns no runloop, so
            // YES-presents release here instead of stalling for thread 0.
            Window_workerPresentBegin();
#endif
            bool walkPresented = false;
            if (VkPane_count() == 0) {
                walkPresented = Vk_clearPresent();
            } else {
                walkPresented = VkPane_presentAll();
            }
#ifdef __APPLE__
            Window_workerPresentEnd();
#endif
#ifndef NDEBUG
            // Throttled walk census (1Hz, debug only — release stays silent
            // per Rule 35): which stage of the demand chain is stuck is
            // answered by one line. panes=chains present, layers=retained
            // scene targets registered, layerRendered/planePresented=did
            // work this walk, live=stuck-resize flag.
            static uint64_t s_walkLogLast = 0;
            uint64_t walkNow = NanoTime_now();
            if (walkNow - s_walkLogLast >= 1000000000ULL) {
                s_walkLogLast = walkNow;
                int live = (w != nullptr && Window_isLiveResizing(w)) ? 1 : 0;
                fprintf(stderr, "vk: walk panes=%d layers=%d layerRendered=%d panePresented=%d live=%d\n",
                        VkPane_count(), VkLayer_count(),
                        sceneAdvanced ? 1 : 0, walkPresented ? 1 : 0, live);
            }
#endif
        }

        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        uint64_t elapsedNs = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL
                           + (uint64_t)(end.tv_nsec - start.tv_nsec);

        uint64_t sleepNs = (elapsedNs < frameBudgetNs) ? (frameBudgetNs - elapsedNs) : minSleepNs;
        if (sleepNs < minSleepNs)
            sleepNs = minSleepNs;

        struct timespec ts = { 0, (long)sleepNs };
        nanosleep(&ts, nullptr);
    }
}

bool Kernel_tick(Kernel *self, double dt) {
    if (!self)
        return false;
    if (!atomic_load_explicit(&(*self).running, memory_order_relaxed))
        return false;

    // 1. Reset per-cycle scratch arena FIRST before any event polling or allocations
    MemoryArena *scratch = (*self).transientArena;
    if (scratch)
        MemoryArena_freeAll(scratch);

    // 2. Single Thread-0 OS event pump
    Window_pollEvents();

    Mouse_dispatchEvents();
    Key_dispatchEvents();

    if (Key_isDown(KEY_ESCAPE)) {
        Kernel_stop(self);
        return false;
    }

    // 3. Poll SPV shader watchers on registered applications
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *app = (*self).applications[i];
        if (!app)
            continue;
        SpvWatch *spv = Application_getSpvWatch(app);
        if (spv && SpvWatch_changed(spv))
            SpvWatch_snap(spv);
    }

    // 4. Tick each active application
    bool anyRunning = false;
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *app = (*self).applications[i];
        if (!app)
            continue;
        if (Application_isRunning(app)) {
            if (Application_tick(app, dt))
                anyRunning = true;
        }
    }

    if (!anyRunning) {
        atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
        return false;
    }

    // 5. Layout/attach pass — owned by Thread 0 (Main Thread).
    // preFrame mutates layer ownership and layout, which is thread-0-only
    // (Rule 11.6). Presentation runs on the present worker
    // (kernel_present_job), so immediate-on-demand demand survives the modal
    // live-resize loop that parks this tick. Direct Kernel_tick callers with
    // no worker spawned (tests) present inline as the legacy single-thread
    // path. Presents synchronously with WindowServer via CATransaction for
    // presentsWithTransaction=YES.
    if (Vk_ready()) {
        if ((*self).applicationCount > 0 && (*self).applications[0]) {
            Window *w = Application_getWindow((*self).applications[0], 0);
            if (w) {
                extern void Darling_preFrame(Window *window, int drawW, int drawH, void *userdata);
                int winW = Window_width(w);
                int winH = Window_height(w);
                Darling_preFrame(w, winW, winH, nullptr);
            }
        }
        if (!(*self).presentWorker) {
#ifdef __APPLE__
            Window_workerPresentBegin();
#endif
            if (VkPane_count() == 0) {
                Vk_clearPresent();
            } else {
                VkPane_presentAll();
            }
#ifdef __APPLE__
            Window_workerPresentEnd();
#endif
        }
    }

    return true;
}

int Kernel_runAll(Kernel *self) {
    if (!self)
        return KERNEL_EXIT_NO_APPS;
    if ((*self).applicationCount == 0)
        return KERNEL_EXIT_OK;
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);

    // Warm up / start registered applications
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *app = (*self).applications[i];
        if (!app)
            continue;
        Application_start(app);

        uint32_t winCount = Application_getWindowCount(app);
        for (uint32_t wIdx = 0; wIdx < winCount; wIdx++) {
            Window *w = Application_getWindow(app, wIdx);
            if (w) {
                if (!Vk_ready()) {
                    Vk_setWindowSeam(w,
                                     (void *(*)(void *))Window_metalLayer,
                                     (bool (*)(void *))Window_isTransparent,
                                     (VkWindowPresentMode (*)(void *))Window_getPresentMode,
                                     (uint64_t (*)(void *))Window_renderGeneration,
                                     (bool (*)(void *))Window_isLiveResizing,
                                     (void (*)(void *, void *, void *))Window_setResizeRenderHook,
                                     (void (*)(void *))Window_setGravityTopLeft);
                    Vk_init();
                }
                // Warm up while hidden: both gates must pass — the board
                // presents AND every pane presented at least once.
                bool boardOk = false;
                bool paneOk = false;
                for (int frame = 0; frame < 60; frame++) {
                    if (Vk_clearPresent())
                        boardOk = true;
                    // Pane warm-up: first pane pixels must exist BEFORE
                    // Window_show — otherwise the window appears blank and
                    // only fills in ticks later (panes self-register during
                    // these warm-up presents via preFrame attach).
                    if (VkPane_count() == 0 || VkPane_presentAll())
                        paneOk = true;
                    if (boardOk && paneOk)
                        break;
                    struct timespec ws = { 0, 8 * 1000 * 1000 };
                    nanosleep(&ws, nullptr);
                }
                Window_show(w);
            }
        }
    }

    uint64_t lastTick = NanoTime_now();

    // Two-thread mode: spawn the present worker so the board + panes keep
    // rendering/animating while thread 0 pumps the OS event loop (live
    // resize tracking runs INSIDE Window_pollEvents on thread 0 — a
    // single-threaded present loop stops dead during a drag).
    (*self).presentWorker = Thread_new(TYPE_THREAD_UI_SINGLETON, kernel_present_job,
                                       1024, false, false);
    if ((*self).presentWorker && Thread_run((*self).presentWorker))
        Thread_submit((*self).presentWorker, self);

    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        uint64_t now = NanoTime_now();
        double dt = (double)(now - lastTick) / 1e9;
        lastTick = now;

        if (!Kernel_tick(self, dt))
            break;

        struct timespec ts = { 0, 1 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }

    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);

    // Bounded join (Rule 26/27): the worker's loop checks running each pass
    // and Vk_clearPresent's waits are all timeout-bounded, so Thread_stop's
    // join completes in bounded time BEFORE the caller tears down Vulkan.
    if ((*self).presentWorker) {
        Thread_stop((*self).presentWorker);
        (*self).presentWorker = NULL;
    }

    return KERNEL_EXIT_OK;
}

int Kernel_runOne(Kernel *self, Application *app) {
    if (!self || !app)
        return KERNEL_EXIT_NO_APPS;
    (void) Kernel_addApplication(self, app);
    return Kernel_runAll(self);
}

bool Kernel_addApplication(Kernel *self, Application *app) {
    if (!self || !app)
        return false;
    for (uint32_t i = 0; i < (*self).applicationCount; i++)
        if ((*self).applications[i] == app)
            return false;
    if ((*self).applicationCount >= KERNEL_MAX_APPS)
        return false;
    (*self).applications[(*self).applicationCount++] = app;
    return true;
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
    if (!self)
        return 0;
    if (!out || cap == 0)
        return 0;
    uint32_t count = (*self).applicationCount;
    uint32_t n = count < cap ? count : cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).applications[i];
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
