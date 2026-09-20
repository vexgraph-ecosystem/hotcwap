// window_linux.c — Linux/X11 backend for the Window API.
//
// The annotations (@PlatformExclusive, @Draft, @Intention, @Incomplete) are
// real markers defined in src/annotation/*.h — C has no annotation feature,
// so they are macros that expand to static asserts embedding the annotation
// text in compiler diagnostics / debug info.
//
// Every function below is an @Incomplete stub returning a safe default. The
// real implementations will use Xlib/Wayland: XCreateSimpleWindow /
// XMapWindow / XUnmapWindow / XStoreName / XResizeWindow / XMoveWindow /
// XDestroyWindow, and an XPending/XNextEvent event loop.

#include <stdio.h>

#include "window/window.h"
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "annotation/draft.h"
#include "annotation/incomplete.h"
#include "annotation/intention.h"
#include "annotation/platform_exclusive.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window_linux
 * ============================================================================
 * Linux/X11 platform implementation stub of the high-level Window abstraction.
 * Provides fallback symbols matching the window.h platform-agnostic API
 * seam on Linux and Unix environments, returning safe defaults until complete
 * Xlib/XCB integration is linked.
 *
 * Conforms to the Cold-Strict, Hot-Minimal Validation Law and satisfies ABI
 * linkage requirements across non-Apple POSIX environments.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: Window_linux (window/window_linux.c)
 * LEVEL: L4 — Self-Management (Linux/X11 OS window backend)
 * ============================================================================
 * SUMMARY:
 *   Linux/X11 backend stub for the Window API.
 *   Provides fallback symbol implementations for Linux/Unix builds.
 *
 * STRUCT FIELDS:
 * ----------------------------------------------------------------------------
 *   (none — stub backend; real fields live in window/window_cocoa.m)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Window_create(title, width, height)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)
 *   - Window_shouldClose(window)
 *   - Window_pollEvents(void)
 *   - Window_renderGeneration(window)
 *   - Window_center(window)
 *   - Window_minimize(window)
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)
 *   - Window_setEnabled(window, enabled)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setLocation(window, x, y)
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setVisible(window, visible)
 *   - Window_setResizable(window, resizable)
 *   - Window_setClosable(window, closable)
 *   - Window_setMiniaturizable(window, miniaturizable)
 *   - Window_setFullscreenButton(window, enabled)
 *   - Window_setUndecorated(window, mode)
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)
 *   - Window_setMinSize(window, width, height)
 *   - Window_setMaxSize(window, width, height)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_isEnabled(window)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_getMonitorId(window)
 *   - Window_getLocation(window, outX, outY)
 *   - Window_isResizable(window)
 *   - Window_isClosable(window)
 *   - Window_isMiniaturizable(window)
 *   - Window_macOS_isTrafficLightButtonVisible(window, light)
 *   - Window_macOS_getTrafficLightHeaderPosition(window, outX, outY)
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */


#if defined(__linux__) || defined(__unix__)

;;PLATFORM_EXCLUSIVE("Linux")
;;INTENTION("Fills the Window API seam (window.h) on Linux so the engine can be built there; mirrors the legacy linuxWindow.java.")
;;DRAFT

;;;;INCOMPLETE // XCreateSimpleWindow; returns nullptr until implemented.
Window *Window_create(const char *title, int width, int height) {
    (void) title;
    (void) width;
    (void) height;
    return nullptr;
}

;;INCOMPLETE // XDestroyWindow; no-op until implemented.
void Window_destroy(Window *window) {
    (void) window;
}

;;INCOMPLETE // Check DestroyNotify; false until implemented.
bool Window_shouldClose(Window *window) {
    (void) window;
    return false;
}

void Window_setShouldClose(Window *window, bool shouldClose) {
    (void) window;
    (void) shouldClose;
}

;;INCOMPLETE // XPending/XNextEvent loop; no-op until implemented.
void Window_pollEvents(void) {
}

;;INCOMPLETE // SwapBuffers vsync; no-op until implemented.
void Window_setPresentMode(Window *window, int mode) {
    (void) window;
    (void) mode;
}

int Window_getPresentMode(const Window *window) {
    (void) window;
    return WINDOW_PRESENT_FIFO;
}

;;INCOMPLETE // Composite alpha; no-op until implemented.
void Window_setTransparent(Window *window, bool transparent) {
    (void) window;
    (void) transparent;
}

bool Window_isTransparent(const Window *window) {
    (void) window;
    return false;
}

uint64_t Window_renderGeneration(const Window *window) {
    (void) window;
    return 0;
}

;;INCOMPLETE // Input kill switch; no-op until implemented.
void Window_setEnabled(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

bool Window_isEnabled(const Window *window) {
    (void) window;
    return true;
}

;;INCOMPLETE // XStoreName; no-op until implemented.
void Window_setTitle(Window *window, const char *title) {
    (void) window;
    (void) title;
}

;;INCOMPLETE // XResizeWindow; no-op until implemented.
void Window_setSize(Window *window, int width, int height) {
    (void) window;
    (void) width;
    (void) height;
}

;;INCOMPLETE // XMoveWindow; no-op until implemented.
void Window_setLocation(Window *window, int x, int y) {
    (void) window;
    (void) x;
    (void) y;
}

void Window_getContentOrigin(const Window *window, int *outX, int *outY) {
    if (outX) *outX = 0;
    if (outY) *outY = 0;
}

void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata) {
    (void) window;
    (void) fn;
    (void) userdata;
}

;;INCOMPLETE // Hook slot not wired on X11 yet; nullptr until implemented.
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
    (void) window;
    return nullptr;
}

uint32_t Window_getMonitorId(const Window *window) {
    (void) window;
    return 0;
}

;;INCOMPLETE // GetWindowRect top-left; zeros until implemented.
void Window_getLocation(const Window *window, int *outX, int *outY) {
    if (outX) *outX = 0;
    if (outY) *outY = 0;
}

;;INCOMPLETE // Center on the screen; no-op until implemented.
void Window_center(Window *window) {
    (void) window;
}

;;INCOMPLETE // XMapWindow/XUnmapWindow; no-op until implemented.
void Window_setVisible(Window *window, bool visible) {
    (void) window;
    (void) visible;
}

;;INCOMPLETE // Motif hints; false until implemented.
bool Window_isResizable(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // Motif hints; no-op until implemented.
void Window_setResizable(Window *window, bool resizable) {
    (void) window;
    (void) resizable;
}

;;INCOMPLETE // WM_DELETE_WINDOW; false until implemented.
bool Window_isClosable(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // WM_DELETE_WINDOW; no-op until implemented.
void Window_setClosable(Window *window, bool closable) {
    (void) window;
    (void) closable;
}

;;INCOMPLETE // Motif hints; false until implemented.
bool Window_isMiniaturizable(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // Motif hints; no-op until implemented.
void Window_setMiniaturizable(Window *window, bool miniaturizable) {
    (void) window;
    (void) miniaturizable;
}

;;INCOMPLETE // _NET_WM_STATE_FULLSCREEN; no-op until implemented.
void Window_setFullscreenButton(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

;;INCOMPLETE // Fullscreen/borderless chrome switch; no-op until implemented.
void Window_setUndecorated(Window *window, int mode) {
    (void) window;
    (void) mode;
}

void Window_setDecorated(Window *window, bool decorated) {
    Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

bool Window_isDecorated(const Window *window) {
    (void) window;
    return true;
}

void Window_setNaked(Window *window, bool naked) {
    Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

bool Window_isNaked(const Window *window) {
    (void) window;
    return false;
}

void Window_setBorderless(Window *window, bool borderless) {
    Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

bool Window_isBorderless(const Window *window) {
    (void) window;
    return false;
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on X11: needs a Mac to mean anything.
void Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
    (void) window;
    (void) light;
    (void) visible;
    fprintf(stderr, "window: Window_macOS_setTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on X11.
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
    (void) window;
    (void) light;
    fprintf(stderr, "window: Window_macOS_isTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
    return false;
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on X11.
void Window_macOS_setTrafficLightHeaderPosition(Window *window, float x, float y) {
    (void) window;
    (void) x;
    (void) y;
    fprintf(stderr, "window: Window_macOS_setTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on X11.
void Window_macOS_getTrafficLightHeaderPosition(const Window *window, float *outX, float *outY) {
    (void) window;
    fprintf(stderr, "window: Window_macOS_getTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
    if (outX)
        *outX = 0.0f;
    if (outY)
        *outY = 0.0f;
}

;;INCOMPLETE // XIconifyWindow; no-op until implemented.
void Window_minimize(Window *window) {
    (void) window;
}

;;INCOMPLETE // WM_CHANGE_STATE normal; no-op until implemented.
void Window_restore(Window *window) {
    (void) window;
}

;;INCOMPLETE // Check _NET_WM_STATE_HIDDEN; false until implemented.
bool Window_isMinimized(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // Check _NET_WM_STATE_FULLSCREEN; false until implemented.
bool Window_isFullscreen(Window *window) {
    (void) window;
    return false;
}

;;INCOMPLETE // _NET_WM_STATE_FULLSCREEN toggle; no-op until implemented.
void Window_setFullscreen(Window *window, bool fullscreen) {
    (void) window;
    (void) fullscreen;
}

;;INCOMPLETE // _NET_WM_STATE_FULLSCREEN toggle; no-op until implemented.
void Window_toggleFullscreen(Window *window) {
    (void) window;
}

;;INCOMPLETE // WM_CHANGE_STATE / compositor support; no-op until implemented.
void Window_setDRM(Window *window, bool enabled) {
    (void) window;
    (void) enabled;
}

;;INCOMPLETE // WM_NORMAL_HINTS min size; no-op until implemented.
void Window_setMinSize(Window *window, int width, int height) {
    (void) window;
    (void) width;
    (void) height;
}

;;INCOMPLETE // WM_NORMAL_HINTS max size; no-op until implemented.
void Window_setMaxSize(Window *window, int width, int height) {
    (void) window;
    (void) width;
    (void) height;
}

#endif
