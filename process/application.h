#ifndef HOT_PROCESS_APPLICATION_H
#define HOT_PROCESS_APPLICATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "hot/hot.h"
#include "hot/spv_watch.h"
#include "window/window.h"

// process/application.h — Executable-level manifest, window registry & hot-module slot.
//
// Application is a pure manifest: identity (name/author/version/icon), the
// window registry, and a hot-module slot. Application is GUI by definition —
// the process taxonomy (the Vertical Integration Law) classifies anything that
// presents pixels through a Window as an Application; CLI functions become
// Process, TUI sessions become Console. There is no "mode": a manifest with
// zero windows is a degenerate (headless-test) Application, nothing more.
//
// Application NEVER owns a loop, a tick, or a present worker — the event
// pump lives in hotcwap's Window (R1), and frame scheduling / presentation
// lives in graphvex's GfxLoop (R3). Application_start/stop just flip the
// running flag; the Kernel observes it.
//
// Application_run is the KEEP-ALIVE PARKED LOOP (hotcwap's own, no graphvex):
// it BLOCKS until every registered window is closed, then ends the app by
// flipping running=false. It parks at a 250ms cadence, letting the Window
// pump its own events in between — so an empty window lives on its own and
// Kernel_run(kernel, app) returns only once the user closed all windows.
//
// Ownership law: Application REGISTERS windows, never destroys them during
// steady state. Application_free frees the Application manifest struct.

#define APP_MAX_WINDOWS 16
#define APP_MAX_NAME 64
#define APP_MAX_VERSION 16
#define APP_MAX_ICON_PATH 512

typedef struct Application Application;

typedef void (*AppHotReloadFn)(Application *self, uint32_t loaded, void *userdata);

struct Application {
    char name[APP_MAX_NAME];               // app name (default "vex")
    char author[APP_MAX_NAME];             // author / studio (default "")
    char version[APP_MAX_VERSION];         // version string, e.g. "1.2.3"
    char iconPath[APP_MAX_ICON_PATH];      // icon path reference (default "")
    Window *windows[APP_MAX_WINDOWS];      // registered top-level windows
    uint32_t window_count;                 // used slots in windows[]
    _Atomic bool running;                  // runtime active flag (Kernel writes, graphvex reads)
    HotModule *hot;                        // dynamic module watcher (opt-in via Application_setHot)
    SpvWatch *spvWatch;                    // SPIR-V shader watcher (opt-in)
    _Atomic uint32_t fps;                  // live telemetry: FPS (graphvex writes)
    _Atomic uint32_t frametimeUs;          // live telemetry: frametime in microseconds (graphvex writes)
    AppHotReloadFn hotReloadFn;            // hot-reload notification callback (nullable)
    void *hotReloadUserdata;               // userdata for hotReloadFn
};

// --- Subsystem bootstrap & shutdown ---
// One-shot bootstrap (System/VFS/fonts). Auto-invoked by the FIRST
// Application() constructor; idempotent beyond that. Explicit calls are
// optional and redundant. Teardown (Application_shutdown) stays caller-owned.
bool Application_init(void);
void Application_shutdown(void);

// --- Overloaded constructors (the Window chooser idiom) ---
//
//   Application()                          -> defaults ("vex", no windows)
//   Application("name")                    -> named
//   Application("name", "author", "1.0.0") -> full identity
//
// Strings are copied in (fixed storage, zero steady-state malloc).
// Getters return pointers into internal storage, stable until the next set.
Application *Application_0(void);
Application *Application_1(const char *name);
Application *Application_3(const char *name, const char *author, const char *version);

#define APPLICATION_CHOOSER(_0, _1, _2, _3, NAME, ...) NAME

#define Application(...) APPLICATION_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    Application_3, Application_2, Application_1, Application_0 \
)(__VA_ARGS__)

// Free the Application. Registered windows are untouched (OS-owned).
void Application_free(Application *self);

// --- Lifecycle flags (Kernel-owned, no loop involvement) ---
void Application_start(Application *self);
void Application_stop(Application *self);
bool Application_isRunning(const Application *self);

// --- Completion predicate (drives the Kernel completion reactor) ---
// True when the app can never end itself: running flipped false (external
// stop), or window_count > 0 and EVERY registered window reports shouldClose.
// A windowless manifest (zero windows) is NOT finished by this rule — it
// lives until an explicit Application_stop, nothing ends it by itself.
bool Application_isFinished(const Application *self);

// --- Keep-alive parked loop (hotcwap's own) ---
// BLOCKS until every registered window is closed (or stop flips running).
// The Window pumps its own events; this only ASKS closed-state at a 250ms
// cadence. Empty windows live on their own — no graphvex dependency.
void Application_run(Application *self);

// --- Lifecycle callbacks ---
void Application_onHotReload(Application *self, AppHotReloadFn fn, void *userdata);

// --- Telemetry ---
uint32_t   Application_getFps(const Application *self);
uint32_t   Application_getFrametimeUs(const Application *self);
HotModule *Application_getHot(const Application *self);
SpvWatch  *Application_getSpvWatch(const Application *self);
// Opt-in: assign a pre-initialized HotModule to enable hot-reload for this
// application. NULL disables.
void        Application_setHot(Application *self, HotModule *hot);

// --- Identity: symmetric setters / getters (the Symmetric Getter/Setter Completeness Law) ---
void        Application_setName(Application *self, const char *name);
const char *Application_getName(const Application *self);
void        Application_setAuthor(Application *self, const char *author);
const char *Application_getAuthor(const Application *self);
void        Application_setVersion(Application *self, const char *version);
const char *Application_getVersion(const Application *self);
void        Application_setIconPath(Application *self, const char *iconPath);
const char *Application_getIconPath(const Application *self);

// --- Window registry (multiwindow) ---
// Register a live window. False on NULL, duplicate, or full registry.
bool      Application_addWindow(Application *self, Window *win);
// Unregister a window (swap-remove, order not preserved). False if absent.
bool      Application_removeWindow(Application *self, Window *win);
// Window at index, or NULL when out of range.
Window   *Application_getWindow(const Application *self, uint32_t index);
// Number of registered windows.
uint32_t  Application_getWindowCount(const Application *self);
// Copy registry into out[] (up to cap), returns entries written.
uint32_t  Application_getWindows(const Application *self, Window **out, uint32_t cap);

#endif
