#include "window/window_event.h"

#include <stddef.h>

#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: WindowEvent (window/window_event.c)
 * LEVEL: L2 — Behavior (per-window OS lifecycle event registry)
 * ============================================================================
 * The fn-pointer lifecycle contract for ONE window. Embedded in the window's
 * platform state (one instance per Window), filled once by the app, fired by
 * the pump pass on Thread 0. Every slot is nullable and every fire is a
 * no-op on a null slot — a window that ignores an event costs nothing and
 * never crashes (the Cold-Strict, Hot-Minimal Validation Law).
 *
 * Focus is Application-level, pressed is Window-level, quit is vetoable:
 *   - onFocusGained/onFocusLost fire only on key-window flips.
 *   - onPressed fires on mouse-down regardless of prior focus.
 *   - onQuitRequested (close btn / Cmd+Q / Cmd+W / Alt+F4) returns bool:
 *     true = may quit. A null slot allows the quit (default-proceed).
 *
 * The fire dispatchers are the ONLY code that reads the slot pointers — the
 * null-checks live here, nowhere else.
 *
 * STRUCT FIELDS (Mirroring window/window_event.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   void *self;                            // opaque owner of every slot
 *   WindowQuitRequestedFn onQuitRequested;  // vetoable quit request
 *   WindowResizedFn onResized;              // content size changed (pixels)
 *   WindowNoArgFn onFullscreen;             // entered fullscreen
 *   WindowNoArgFn onMinimized;              // minimized
 *   WindowNoArgFn onRestored;               // back to normal
 *   WindowNoArgFn onPressed;                // mouse pressed on the window
 *   WindowNoArgFn onFocusGained;            // key window became THIS window
 *   WindowNoArgFn onFocusLost;              // key window left THIS window
 *   WindowNoArgFn onZoomFilled;             // double-click header zoom-to-fill
 *
 * PRIVATE HELPERS: None.  (WindowQuitRequestedFn / WindowResizedFn /
 * WindowNoArgFn are function-pointer typedefs, not classes.)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - WindowEvent_init(self)                : zeros every slot
 *
 * Core Functions (fire dispatchers — Thread 0 only):
 *   - WindowEvent_fireQuitRequested(self, window) -> bool (may quit)
 *   - WindowEvent_fireResized(self, window, width, height)
 *   - WindowEvent_fireFullscreen(self, window)
 *   - WindowEvent_fireMinimized(self, window)
 *   - WindowEvent_fireRestored(self, window)
 *   - WindowEvent_firePressed(self, window)
 *   - WindowEvent_fireFocusGained(self, window)
 *   - WindowEvent_fireFocusLost(self, window)
 *   - WindowEvent_fireZoomFilled(self, window)
 *
 * Setters:
 *   - WindowEvent_setSelf(self, owner)
 *   - WindowEvent_setOnQuitRequested(self, fn)
 *   - WindowEvent_setOnResized(self, fn)
 *   - WindowEvent_setOnFullscreen(self, fn)
 *   - WindowEvent_setOnMinimized(self, fn)
 *   - WindowEvent_setOnRestored(self, fn)
 *   - WindowEvent_setOnPressed(self, fn)
 *   - WindowEvent_setOnFocusGained(self, fn)
 *   - WindowEvent_setOnFocusLost(self, fn)
 *   - WindowEvent_setOnZoomFilled(self, fn)
 *
 * Getters:
 *   - WindowEvent_getSelf(self)
 *   - WindowEvent_getOnQuitRequested(self)
 *   - WindowEvent_getOnResized(self)
 *   - WindowEvent_getOnFullscreen(self)
 *   - WindowEvent_getOnMinimized(self)
 *   - WindowEvent_getOnRestored(self)
 *   - WindowEvent_getOnPressed(self)
 *   - WindowEvent_getOnFocusGained(self)
 *   - WindowEvent_getOnFocusLost(self)
 *   - WindowEvent_getOnZoomFilled(self)
 * ============================================================================
 */

// CONSTRUCTORS
bool WindowEvent_init(WindowEvent *self) {
    if(self == nullptr)
        return false;
    (*self).self = nullptr;
    (*self).onQuitRequested = nullptr;
    (*self).onResized = nullptr;
    (*self).onFullscreen = nullptr;
    (*self).onMinimized = nullptr;
    (*self).onRestored = nullptr;
    (*self).onPressed = nullptr;
    (*self).onFocusGained = nullptr;
    (*self).onFocusLost = nullptr;
    (*self).onZoomFilled = nullptr;
    return true;
}

// CORE FUNCTIONS
bool WindowEvent_fireQuitRequested(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return true;
    if((*self).onQuitRequested == nullptr)
        return true;
    return (*self).onQuitRequested((*self).self, window);
}

void WindowEvent_fireResized(WindowEvent *self, Window *window, int width, int height) {
    if(self == nullptr)
        return;
    if((*self).onResized == nullptr)
        return;
    (*self).onResized((*self).self, window, width, height);
}

void WindowEvent_fireFullscreen(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onFullscreen == nullptr)
        return;
    (*self).onFullscreen((*self).self, window);
}

void WindowEvent_fireMinimized(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onMinimized == nullptr)
        return;
    (*self).onMinimized((*self).self, window);
}

void WindowEvent_fireRestored(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onRestored == nullptr)
        return;
    (*self).onRestored((*self).self, window);
}

void WindowEvent_firePressed(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onPressed == nullptr)
        return;
    (*self).onPressed((*self).self, window);
}

void WindowEvent_fireFocusGained(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onFocusGained == nullptr)
        return;
    (*self).onFocusGained((*self).self, window);
}

void WindowEvent_fireFocusLost(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onFocusLost == nullptr)
        return;
    (*self).onFocusLost((*self).self, window);
}

void WindowEvent_fireZoomFilled(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onZoomFilled == nullptr)
        return;
    (*self).onZoomFilled((*self).self, window);
}

// SETTERS
void WindowEvent_setSelf(WindowEvent *self, void *owner) {
    if(self == nullptr)
        return;
    (*self).self = owner;
}

void WindowEvent_setOnQuitRequested(WindowEvent *self, WindowQuitRequestedFn fn) {
    if(self == nullptr)
        return;
    (*self).onQuitRequested = fn;
}

void WindowEvent_setOnResized(WindowEvent *self, WindowResizedFn fn) {
    if(self == nullptr)
        return;
    (*self).onResized = fn;
}

void WindowEvent_setOnFullscreen(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFullscreen = fn;
}

void WindowEvent_setOnMinimized(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onMinimized = fn;
}

void WindowEvent_setOnRestored(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onRestored = fn;
}

void WindowEvent_setOnPressed(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onPressed = fn;
}

void WindowEvent_setOnFocusGained(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFocusGained = fn;
}

void WindowEvent_setOnFocusLost(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFocusLost = fn;
}

void WindowEvent_setOnZoomFilled(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onZoomFilled = fn;
}

// GETTERS
void *WindowEvent_getSelf(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).self;
}

WindowQuitRequestedFn WindowEvent_getOnQuitRequested(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onQuitRequested;
}

WindowResizedFn WindowEvent_getOnResized(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onResized;
}

WindowNoArgFn WindowEvent_getOnFullscreen(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFullscreen;
}

WindowNoArgFn WindowEvent_getOnMinimized(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onMinimized;
}

WindowNoArgFn WindowEvent_getOnRestored(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onRestored;
}

WindowNoArgFn WindowEvent_getOnPressed(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onPressed;
}

WindowNoArgFn WindowEvent_getOnFocusGained(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFocusGained;
}

WindowNoArgFn WindowEvent_getOnFocusLost(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFocusLost;
}

WindowNoArgFn WindowEvent_getOnZoomFilled(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onZoomFilled;
}