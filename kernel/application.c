#include "application.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "annotation/intention.h"
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "input/key.h"
#include "system/system.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Application
 * ============================================================================
 * RAISON D'ÊTRE:
 *   The executable manifest for a running graphical application: identity
 *   (name/author/version/icon), top-level window registry, and hot-reload slot.
 *   Application classifies GUI executables that present pixels through a Window;
 *   it is a pure data descriptor with zero loops, ticks, or render workers.
 *
 * MEMORY LAYOUT & LIFECYCLE:
 *   Flat fixed-size buffers for strings and an array of up to 16 Window pointers.
 *   Atomic fields ensure thread-safe telemetry (fps, frametimeUs) and running state.
 *   Allocation is cold-path heap via calloc; windows are referenced, never owned.
 *
 * OPERATIONAL INVARIANTS:
 *   - The Kernel drives the event pump and observes the running flag.
 *   - Application_run is an opt-in parked loop blocking on window closure only.
 *   - Frame scheduling and GPU presentation belong strictly to R3 GfxLoop.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Application (kernel/application.c)
 * LEVEL: L2 — Behavior (executable identity, window registry & hot-module slot)
 * ============================================================================
 * SUMMARY:
 *   The manifest for a running executable: name, author, version, icon, and the
 *   window registry. Application is GUI by definition (presents pixels through
 *   a Window); CLI functions are Process, TUI sessions are Console. It is a
 *   pure data object — it NEVER owns a loop, a tick, or a present worker. The
 *   Kernel (R1) drives the event pump and observes the running flag; graphvex
 *   (R3) drives frame scheduling, presentation, and telemetry writes.
 *
 * STRUCT FIELDS (Mirroring kernel/application.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char name[APP_MAX_NAME];               // app name (default "vex")
 *   char author[APP_MAX_NAME];             // author / studio (default "")
 *   char version[APP_MAX_VERSION];         // version string, e.g. "1.2.3"
 *   char iconPath[APP_MAX_ICON_PATH];      // icon path reference (default "")
 *   Window *windows[APP_MAX_WINDOWS];      // registered top-level windows
 *   uint32_t window_count;                 // used slots in windows[]
 *   _Atomic bool running;                  // runtime active flag (Kernel writes, graphvex reads)
 *   HotModule *hot;                        // dynamic module watcher (opt-in via setHot)
 *   _Atomic uint32_t fps;                  // live telemetry: FPS (graphvex writes)
 *   _Atomic uint32_t frametimeUs;          // live telemetry: frametime in microseconds
 *   AppHotReloadFn hotReloadFn;            // hot-reload notification callback (nullable)
 *   void *hotReloadUserdata;               // userdata for hotReloadFn
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   appAllWindowsClosed(const self) : true when window_count>0 and EVERY
 *       registered window reports shouldClose (plain if/read — the Window
 *       owns its own events; this only asks). False for a windowless
 *       manifest (nothing to end).
 *   appParkSlice(const self)       : one 250ms park turn in 25ms slices;
 *       each slice lets the Window pump its own queue (Window_pollEvents)
 *       and re-checks the running flag. This is what keeps an empty window
 *       alive and responsive — never a present, never a tick.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Application()                            : Application_0()
 *   - Application(name)                        : Application_1(name)
 *   - Application(name, author, version)       : Application_3(name, author, version)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Application_init()                       : One-shot bootstrap
 *   - Application_shutdown()                   : Teardown input/key state
 *   - Application_free(self)                   : Release manifest memory
 *   - Application_start(self)                  : Flag only — marks running true
 *   - Application_stop(self)                   : Flag only — marks running false
 *   - Application_isRunning(self)              : Queries atomic running flag
 *   - Application_isFinished(self)             : Completion predicate for Kernel
 *   - Application_run(self)                    : Keep-alive parked loop
 *   - Application_pollHot(self)                : Generation-driven swap
 *   - Application_onHotReload(self, fn, user)  : Assign hot-reload callback
 *   - Application_addWindow(self, win)         : Register top-level window
 *   - Application_removeWindow(self, win)      : Unregister top-level window
 *
 * Private Core Functions: (.c static)
 *   - appAllWindowsClosed(self)                : Test if all registered windows closed
 *   - appParkSlice(self)                       : 250ms park in 25ms slices
 *
 * Public Setters: (.h)
 *   - Application_setHot(self, hot)
 *   - Application_setName(self, name)
 *   - Application_setAuthor(self, author)
 *   - Application_setVersion(self, version)
 *   - Application_setIconPath(self, iconPath)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Application_getFps(self)
 *   - Application_getFrametimeUs(self)
 *   - Application_getHot(self)
 *   - Application_getName(self)
 *   - Application_getAuthor(self)
 *   - Application_getVersion(self)
 *   - Application_getIconPath(self)
 *   - Application_getWindow(self, index)
 *   - Application_getWindowCount(self)
 *   - Application_getWindows(self, out, cap)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// BOOTSTRAP — runs exactly once (first Application() constructor triggers it).
// Callers just construct and free; init is the constructor's job, teardown is
// the caller's (they opened the Application, they close it).
// Plain bool, not _Atomic: construction is single-threaded cold-path (the Cold-Strict, Hot-Minimal Validation Law).
static bool s_bootstrapped = false;

bool Application_init() {
    if (s_bootstrapped)
        return true;
    s_bootstrapped = true;
    System_initializeAll();
    // NOTE: no Vfs_init() here by design — the VFS lives in darling R4
    // (io/vfs.c) since the split and boots itself once UI paths resolve.
    // R1 never reaches up the stack (the Vertical Integration Law).
    return true;
}

void Application_shutdown() {
    Key_shutdown();
}

// KEEP-ALIVE PARKED LOOP — the app lives on its own: Application_run parks
// the main thread here until EVERY registered window is closed, ending the
// application. It does two things only: lets the Window chew its own event
// queue (Window_pollEvents — events are the Window's job, never the app's),
// and asks each window whether it shouldClose (a plain if/read). The park
// cadence is 250ms (4 checks/sec — not hot looping); events are pumped every
// 25ms slice so the empty window stays responsive during the park. Nothing
// here touches graphvex, present, or GfxLoop — hotcwap owns the window end.
#define APP_PARK_CADENCE_NS (250 * 1000 * 1000)
#define APP_PARK_SLICE_NS   (25 * 1000 * 1000)
#define APP_PARK_SLICES     10

static bool appAllWindowsClosed(const Application *self) {
    if ((*self).window_count == 0)
        return false; // windowless manifest: nothing to end
    for (uint32_t i = 0; i < (*self).window_count; i++)
        if (!Window_shouldClose((*self).windows[i]))
            return false;
    return true;
}

static void appParkSlice(Application *self) {
    for (uint32_t i = 0; i < APP_PARK_SLICES; i++) {
        if (!atomic_load_explicit(&(*self).running, memory_order_relaxed))
            return;
        Window_pollEvents(); // the Window pumps its own queue (its job)
        struct timespec slice = { APP_PARK_SLICE_NS / 1000000000ULL,
                                  APP_PARK_SLICE_NS % 1000000000ULL };
        nanosleep(&slice, nullptr);
    }
}

// CONSTRUCTORS (PUBLIC & PRIVATE)
Application *Application_0(void) {
    Application_init();
    Application *self = (Application*) calloc(1, sizeof(Application));
    if (!self) return nullptr;
    strncpy((*self).name, "vex", APP_MAX_NAME - 1);
    return self;
}

Application *Application_1(const char *name) {
    Application *self = Application_0();
    if (!self) return nullptr;
    Application_setName(self, name);
    return self;
}

Application *Application_3(const char *name, const char *author, const char *version) {
    Application *self = Application_0();
    if (!self) return nullptr;
    Application_setName(self, name);
    Application_setAuthor(self, author);
    Application_setVersion(self, version);
    return self;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
void Application_free(Application *self) {
    if (!self) return;
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    free(self);
}

void Application_start(Application *self) {
    if (!self) return;
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);
}

void Application_stop(Application *self) {
    if (!self) return;
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
}

bool Application_isRunning(const Application *self) {
    return self ? atomic_load_explicit(&(*self).running, memory_order_relaxed) : false;
}

bool Application_isFinished(const Application *self) {
    if(self == nullptr)
        return true;
    if(!atomic_load_explicit(&(*self).running, memory_order_relaxed))
        return true;
    if(appAllWindowsClosed(self))
        return true;
    return false;
}

// The parked keep-alive loop. BLOCKS until every registered window is closed
// (or an external stop flips running). The empty window lives on its own here
// — no graphvex, no present, no GfxLoop: hotcwap manages the Windows and ends
// the Application when they are all gone. The Window pumps its own events; the
// app only ASKS "any window still open?" at the 250ms cadence. So
// Kernel_run(kernel, app) blocks: the app stays alive exactly as long as a
// window is open.
void Application_run(Application *self) {
    if (!self) return;
    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        if (appAllWindowsClosed(self)) {
            atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
            return;
        }
        appParkSlice(self);
        Application_pollHot(self); // generation-driven hot swap at ~250ms cadence
    }
}

bool Application_addWindow(Application *self, Window *win) {
    if (!self || !win) return false;
    for (uint32_t i = 0; i < (*self).window_count; i++)
        if ((*self).windows[i] == win) return false;
    if ((*self).window_count >= APP_MAX_WINDOWS) return false;
    (*self).windows[(*self).window_count++] = win;
    return true;
}

bool Application_removeWindow(Application *self, Window *win) {
    if (!self || !win) return false;
    for (uint32_t i = 0; i < (*self).window_count; i++) {
        if ((*self).windows[i] == win) {
            (*self).windows[i] = (*self).windows[--(*self).window_count];
            (*self).windows[(*self).window_count] = nullptr;
            return true;
        }
    }
    return false;
}

void Application_onHotReload(Application *self, AppHotReloadFn fn, void *userdata) {
    if (!self) return;
    (*self).hotReloadFn = fn;
    (*self).hotReloadUserdata = userdata;
}

void Application_pollHot(Application *self) {
    if (!self || !(*self).hot)
        return;
    uint32_t loaded = 0;
    HotResult r = Hot_poll((*self).hot, &loaded);
    if (r == HOT_OK && loaded > 0 && (*self).hotReloadFn)
        (*self).hotReloadFn(self, loaded, (*self).hotReloadUserdata);
}

// SETTERS (PUBLIC & PRIVATE)
;;SETTER
void Application_setHot(Application *self, HotModule *hot) {
    if (!self) return;
    (*self).hot = hot;
}

;;SETTER
void Application_setName(Application *self, const char *name) {
    if (!self || !name) return;
    strncpy((*self).name, name, APP_MAX_NAME - 1);
    (*self).name[APP_MAX_NAME - 1] = '\0';
}

;;SETTER
void Application_setAuthor(Application *self, const char *author) {
    if (!self || !author) return;
    strncpy((*self).author, author, APP_MAX_NAME - 1);
    (*self).author[APP_MAX_NAME - 1] = '\0';
}

;;SETTER
void Application_setVersion(Application *self, const char *version) {
    if (!self || !version) return;
    strncpy((*self).version, version, APP_MAX_VERSION - 1);
    (*self).version[APP_MAX_VERSION - 1] = '\0';
}

;;SETTER
void Application_setIconPath(Application *self, const char *iconPath) {
    if (!self || !iconPath) return;
    strncpy((*self).iconPath, iconPath, APP_MAX_ICON_PATH - 1);
    (*self).iconPath[APP_MAX_ICON_PATH - 1] = '\0';
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
uint32_t Application_getFps(const Application *self) {
    return self ? atomic_load_explicit(&(*self).fps, memory_order_relaxed) : 0;
}

;;GETTER
uint32_t Application_getFrametimeUs(const Application *self) {
    return self ? atomic_load_explicit(&(*self).frametimeUs, memory_order_relaxed) : 0;
}

;;GETTER
HotModule *Application_getHot(const Application *self) {
    return self ? (*self).hot : nullptr;
}

;;GETTER
const char *Application_getName(const Application *self) {
    if (!self) return nullptr;
    return (*self).name;
}

;;GETTER
const char *Application_getAuthor(const Application *self) {
    if (!self) return nullptr;
    return (*self).author;
}

;;GETTER
const char *Application_getVersion(const Application *self) {
    if (!self) return nullptr;
    return (*self).version;
}

;;GETTER
const char *Application_getIconPath(const Application *self) {
    if (!self) return nullptr;
    return (*self).iconPath;
}

;;GETTER
Window *Application_getWindow(const Application *self, uint32_t index) {
    if (!self) return nullptr;
    if (index >= (*self).window_count) return nullptr;
    return (*self).windows[index];
}

;;GETTER
uint32_t Application_getWindowCount(const Application *self) {
    if (!self) return 0;
    return (*self).window_count;
}

;;GETTER
uint32_t Application_getWindows(const Application *self, Window **out, uint32_t cap) {
    if (!self || !out) return 0;
    uint32_t n = (*self).window_count;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).windows[i];
    return n;
}

