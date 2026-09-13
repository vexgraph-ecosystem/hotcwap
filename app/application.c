#include "app/application.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "annotation/overview.h"
#include "font/font_bake.h"
#include "input/key.h"
#include "input/mouse.h"
#include "io/vfs.h"
#include "oop/type.h"
#include "system/system.h"
#include "time/nanotime.h"
#include "vulkan/vk.h"
#include "vulkan/vk_pane.h"
#include "window/window.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Application (app/application.c)
 * LEVEL: L2 — Behavior (executable identity, runtime runner & window registry)
 * ============================================================================
 * The truth about the running executable: name, author, version, icon, and
 * execution mode (CLI, TUI, or GUI). Owns the application lifecycle:
 * bootstrap, presentation worker threading, event pumping, and clean exit.
 *
 * STRUCT FIELDS (Mirroring app/application.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char name[APP_MAX_NAME];               // app name (default "vex")
 *   char author[APP_MAX_NAME];             // author / studio (default "")
 *   char version[APP_MAX_VERSION];         // version string, e.g. "1.2.3"
 *   char iconPath[APP_MAX_ICON_PATH];      // icon path reference (default "")
 *   Window *windows[APP_MAX_WINDOWS];      // registered top-level windows
 *   uint32_t window_count;                 // used slots in windows[]
 *   AppMode mode;                          // execution mode (default APP_MODE_AUTO)
 *   _Atomic bool running;                  // runtime active flag
 *   Thread *presentWorker;                 // background presentation thread (GUI mode)
 *   HotModule *hot;                        // dynamic module watcher
 *   SpvWatch *spvWatch;                    // SPIR-V shader watcher
 *   _Atomic uint32_t fps;                  // live telemetry: FPS
 *   _Atomic uint32_t frametimeUs;          // live telemetry: frametime (microseconds)
 *   AppRunFn runHandler;                   // custom run override (nullable)
 *   void *runUserdata;                     // userdata for runHandler
 *   AppTickFn tickFn;                      // per-frame tick callback (nullable)
 *   void *tickUserdata;                    // userdata for tickFn
 *   AppHotReloadFn hotReloadFn;            // hot-reload callback (nullable)
 *   void *hotReloadUserdata;               // userdata for hotReloadFn
 *
 * PRIVATE HELPERS: None.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Application()                        : Application_0()
 *   - Application(name)                    : Application_1(name)
 *   - Application(name, author, version)   : Application_3(name, author, version)
 *
 * Core Functions:
 *   - Application_init()
 *   - Application_shutdown()
 *   - Application_run(self)
 *   - Application_start(self)
 *   - Application_stop(self)
 *   - Application_isRunning(self)
 *   - Application_tick(self, dt)
 *   - Application_free(self)
 *   - Application_addWindow(self, win)
 *   - Application_removeWindow(self, win)
 *
 * Mode & Callbacks:
 *   - Application_setMode(self, mode)
 *   - Application_getMode(self)
 *   - Application_setRunHandler(self, fn, user)
 *   - Application_onTick(self, fn, user)
 *   - Application_onHotReload(self, fn, user)
 *
 * Telemetry:
 *   - Application_getFps(self)
 *   - Application_getFrametimeUs(self)
 *   - Application_getHot(self)
 *   - Application_getSpvWatch(self)
 *
 * Setters:
 *   - Application_setName(self, name)
 *   - Application_setAuthor(self, author)
 *   - Application_setVersion(self, version)
 *   - Application_setIconPath(self, iconPath)
 *
 * Getters:
 *   - Application_getName(self)
 *   - Application_getAuthor(self)
 *   - Application_getVersion(self)
 *   - Application_getIconPath(self)
 *   - Application_getWindow(self, index)
 *   - Application_getWindowCount(self)
 *   - Application_getWindows(self, out, cap)
 * ============================================================================
 */

// BOOTSTRAP
bool Application_init(void) {
    System_initializeAll();
    Vfs_init();
    if (getenv("ANTI_BAKE_FONTS")) {
        FontBake_refreshAllFonts();
    }
    return true;
}

void Application_shutdown(void) {
    Key_shutdown();
}

// CONSTRUCTORS
Application *Application_0(void) {
    Application *self = (Application*) calloc(1, sizeof(Application));
    if (!self) return NULL;
    strncpy((*self).name, "vex", APP_MAX_NAME - 1);
    (*self).mode = APP_MODE_AUTO;
    return self;
}

Application *Application_1(const char *name) {
    Application *self = Application_0();
    if (!self) return NULL;
    Application_setName(self, name);
    return self;
}

Application *Application_3(const char *name, const char *author, const char *version) {
    Application *self = Application_0();
    if (!self) return NULL;
    Application_setName(self, name);
    Application_setAuthor(self, author);
    Application_setVersion(self, version);
    return self;
}

// CORE FUNCTIONS
void Application_free(Application *self) {
    if (!self) return;
    if (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        Application_stop(self);
    }
    free(self);
}

void Application_stop(Application *self) {
    if (!self) return;
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
}

void Application_start(Application *self) {
    if (!self)
        return;
    if (!(*self).hot)
        (*self).hot = Hot_init("hot");
    if (!(*self).spvWatch)
        (*self).spvWatch = SpvWatch_init();
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);
}

bool Application_tick(Application *self, double dt) {
    if (!self)
        return false;
    if (!atomic_load_explicit(&(*self).running, memory_order_relaxed))
        return false;

    if ((*self).hot) {
        uint32_t loaded = 0;
        Hot_poll((*self).hot, &loaded);
        if (loaded > 0 && (*self).hotReloadFn)
            (*self).hotReloadFn(self, loaded, (*self).hotReloadUserdata);
    }

    bool allClosed = ((*self).window_count > 0);
    for (uint32_t i = 0; i < (*self).window_count; i++) {
        Window *w = (*self).windows[i];
        if (!w)
            continue;
        if (Window_shouldClose(w))
            continue;
        allClosed = false;
        Window_dispatchEvents(w);
    }

    if ((*self).window_count > 0 && allClosed) {
        atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
        return false;
    }

    if ((*self).tickFn)
        (*self).tickFn(self, dt, (*self).tickUserdata);

    return true;
}

bool Application_isRunning(const Application *self) {
    return self ? atomic_load_explicit(&(*self).running, memory_order_relaxed) : false;
}

void Application_setMode(Application *self, AppMode mode) {
    if (!self) return;
    (*self).mode = mode;
}

AppMode Application_getMode(const Application *self) {
    return self ? (*self).mode : APP_MODE_AUTO;
}

void Application_setRunHandler(Application *self, AppRunFn fn, void *userdata) {
    if (!self) return;
    (*self).runHandler = fn;
    (*self).runUserdata = userdata;
}

void Application_onTick(Application *self, AppTickFn fn, void *userdata) {
    if (!self) return;
    (*self).tickFn = fn;
    (*self).tickUserdata = userdata;
}

void Application_onHotReload(Application *self, AppHotReloadFn fn, void *userdata) {
    if (!self) return;
    (*self).hotReloadFn = fn;
    (*self).hotReloadUserdata = userdata;
}

uint32_t Application_getFps(const Application *self) {
    return self ? atomic_load_explicit(&(*self).fps, memory_order_relaxed) : 0;
}

uint32_t Application_getFrametimeUs(const Application *self) {
    return self ? atomic_load_explicit(&(*self).frametimeUs, memory_order_relaxed) : 0;
}

HotModule *Application_getHot(const Application *self) {
    return self ? (*self).hot : nullptr;
}

SpvWatch *Application_getSpvWatch(const Application *self) {
    return self ? (*self).spvWatch : nullptr;
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
            (*self).windows[(*self).window_count] = NULL;
            return true;
        }
    }
    return false;
}

// SETTERS
void Application_setName(Application *self, const char *name) {
    if (!self || !name) return;
    strncpy((*self).name, name, APP_MAX_NAME - 1);
    (*self).name[APP_MAX_NAME - 1] = '\0';
}

void Application_setAuthor(Application *self, const char *author) {
    if (!self || !author) return;
    strncpy((*self).author, author, APP_MAX_NAME - 1);
    (*self).author[APP_MAX_NAME - 1] = '\0';
}

void Application_setVersion(Application *self, const char *version) {
    if (!self || !version) return;
    strncpy((*self).version, version, APP_MAX_VERSION - 1);
    (*self).version[APP_MAX_VERSION - 1] = '\0';
}

void Application_setIconPath(Application *self, const char *iconPath) {
    if (!self || !iconPath) return;
    strncpy((*self).iconPath, iconPath, APP_MAX_ICON_PATH - 1);
    (*self).iconPath[APP_MAX_ICON_PATH - 1] = '\0';
}

// GETTERS
const char *Application_getName(const Application *self) {
    if (!self) return NULL;
    return (*self).name;
}

const char *Application_getAuthor(const Application *self) {
    if (!self) return NULL;
    return (*self).author;
}

const char *Application_getVersion(const Application *self) {
    if (!self) return NULL;
    return (*self).version;
}

const char *Application_getIconPath(const Application *self) {
    if (!self) return NULL;
    return (*self).iconPath;
}

Window *Application_getWindow(const Application *self, uint32_t index) {
    if (!self) return NULL;
    if (index >= (*self).window_count) return NULL;
    return (*self).windows[index];
}

uint32_t Application_getWindowCount(const Application *self) {
    if (!self) return 0;
    return (*self).window_count;
}

uint32_t Application_getWindows(const Application *self, Window **out, uint32_t cap) {
    if (!self || !out) return 0;
    uint32_t n = (*self).window_count;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).windows[i];
    return n;
}

// --- RUNTIME WORKERS & RUN LOOPS ---

static void app_present_job(Thread *selfThread, void *task) {
    (void) selfThread;
    Application *self = (Application*) task;
    if (!self) return;

    uint64_t lastReportNanos = NanoTime_now();
    uint32_t frameCount = 0;

    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        uint64_t frameStart = NanoTime_now();

        Vk_clearPresent();
        Window_presentPanesWithTransaction(VkPane_presentAll);

        frameCount++;
        uint64_t frameEnd = NanoTime_now();
        uint64_t frameUs = (frameEnd - frameStart) / 1000;
        atomic_store_explicit(&(*self).frametimeUs, (uint32_t) frameUs, memory_order_relaxed);

        uint64_t elapsed = frameEnd - lastReportNanos;
        if (elapsed >= 500000000ULL) {
            uint32_t fps = (uint32_t)((frameCount * 1000000000ULL) / elapsed);
            atomic_store_explicit(&(*self).fps, fps, memory_order_relaxed);
            frameCount = 0;
            lastReportNanos = frameEnd;
        }
    }
}

static int run_cli(Application *self) {
    (*self).hot = Hot_init("hot");
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);

    uint64_t lastTick = NanoTime_now();

    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        if ((*self).hot) {
            uint32_t loaded = 0;
            Hot_poll((*self).hot, &loaded);
            if (loaded > 0 && (*self).hotReloadFn)
                (*self).hotReloadFn(self, loaded, (*self).hotReloadUserdata);
        }

        uint64_t now = NanoTime_now();
        double dt = (double)(now - lastTick) / 1e9;
        lastTick = now;

        if ((*self).tickFn)
            (*self).tickFn(self, dt, (*self).tickUserdata);

        struct timespec ts = { 0, 10 * 1000 * 1000 }; // 10ms slice
        nanosleep(&ts, nullptr);
    }

    return 0;
}

static int run_tui(Application *self) {
    (*self).hot = Hot_init("hot");
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);

    uint64_t lastTick = NanoTime_now();

    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        if ((*self).hot) {
            uint32_t loaded = 0;
            Hot_poll((*self).hot, &loaded);
            if (loaded > 0 && (*self).hotReloadFn)
                (*self).hotReloadFn(self, loaded, (*self).hotReloadUserdata);
        }

        uint64_t now = NanoTime_now();
        double dt = (double)(now - lastTick) / 1e9;
        lastTick = now;

        if ((*self).tickFn)
            (*self).tickFn(self, dt, (*self).tickUserdata);

        struct timespec ts = { 0, 5 * 1000 * 1000 }; // 5ms slice
        nanosleep(&ts, nullptr);
    }

    return 0;
}

static int run_gui(Application *self) {
    (*self).hot = Hot_init("hot");
    (*self).spvWatch = SpvWatch_init();

    // Warm up each window while hidden, then show
    for (uint32_t i = 0; i < (*self).window_count; i++) {
        Window *w = (*self).windows[i];
        if (!w) continue;
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
        // Warm up each window while hidden, then show. Both gates must pass:
        // the board presents AND every pane presented at least once.
        bool boardOk = false;
        bool paneOk = false;
        for (int frame = 0; frame < 60; frame++) {
            if (Vk_clearPresent())
                boardOk = true;
            // Pane warm-up: presents every registered pane chain while still
            // hidden, so first pane pixels exist BEFORE Window_show — the
            // window renders the exact moment it appears instead of N blank
            // ticks later. Panes self-register during these warm-up presents
            // (preFrame attach), which also moves registration off the
            // worker-startup path. No panes yet counts as ready.
            if (VkPane_count() == 0 || Window_presentPanesWithTransaction(VkPane_presentAll))
                paneOk = true;
            if (boardOk && paneOk)
                break;
            struct timespec ws = { 0, 8 * 1000 * 1000 };
            nanosleep(&ws, nullptr);
        }
        Window_show(w);
    }

    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);

    // Spawn present worker
    (*self).presentWorker = Thread_new(TYPE_THREAD_UI_SINGLETON, app_present_job, 1024, false, false);
    if ((*self).presentWorker && Thread_run((*self).presentWorker)) {
        Thread_submit((*self).presentWorker, self);
    }

    uint64_t lastTick = NanoTime_now();
    uint64_t lastReport = lastTick;
    char titleBuf[256];

    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        Window_pollEvents();

        bool allClosed = true;
        for (uint32_t i = 0; i < (*self).window_count; i++) {
            Window *w = (*self).windows[i];
            if (!w) continue;
            if (Window_shouldClose(w))
                continue;
            allClosed = false;
            Window_dispatchEvents(w);
        }

        if (allClosed || Key_isDown(KEY_ESCAPE)) {
            atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
            break;
        }

        Mouse_dispatchEvents();
        Key_dispatchEvents();

        if ((*self).hot) {
            uint32_t loaded = 0;
            Hot_poll((*self).hot, &loaded);
            if (loaded > 0 && (*self).hotReloadFn)
                (*self).hotReloadFn(self, loaded, (*self).hotReloadUserdata);
        }

        uint64_t now = NanoTime_now();
        double dt = (double)(now - lastTick) / 1e9;
        lastTick = now;

        if ((*self).tickFn)
            (*self).tickFn(self, dt, (*self).tickUserdata);

        if (now - lastReport >= 500000000ULL) {
            uint32_t fps = atomic_load_explicit(&(*self).fps, memory_order_relaxed);
            uint32_t ft = atomic_load_explicit(&(*self).frametimeUs, memory_order_relaxed);
            for (uint32_t i = 0; i < (*self).window_count; i++) {
                Window *w = (*self).windows[i];
                if (w) {
                    snprintf(titleBuf, sizeof(titleBuf), "%s | %u FPS (%u us)",
                             (*self).name, fps, ft);
                    Window_setTitle(w, titleBuf);
                }
            }
            lastReport = now;
        }

        struct timespec ts = { 0, 1 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }

    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    if ((*self).presentWorker) {
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, nullptr);
    }

    return 0;
}

int Application_run(Application *self) {
    if (!self) return -1;

    if ((*self).runHandler) {
        atomic_store_explicit(&(*self).running, true, memory_order_relaxed);
        int rc = (*self).runHandler(self, (*self).runUserdata);
        atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
        return rc;
    }

    AppMode resolvedMode = (*self).mode;
    if (resolvedMode == APP_MODE_AUTO) {
        resolvedMode = ((*self).window_count > 0) ? APP_MODE_GUI : APP_MODE_CLI;
    }

    switch (resolvedMode) {
        case APP_MODE_CLI:
            return run_cli(self);
        case APP_MODE_TUI:
            return run_tui(self);
        case APP_MODE_GUI:
        default:
            return run_gui(self);
    }
}
