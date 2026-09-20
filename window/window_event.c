#include "window/window_event.h"

#include <stddef.h>

#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: WindowEvent
 * ============================================================================
 * Lifecycle event callback registry bound to an individual window instance.
 * Embedded directly within the window's platform state rather than heap allocated,
 * providing zero-allocation event dispatch. Every callback slot is nullable and
 * dispatched strictly on Thread 0 during event pump loops; unhandled events
 * incur zero overhead and safely no-op under the Cold-Strict, Hot-Minimal
 * validation rules.
 *
 * Supports vetoable termination requests via onQuitRequested, geometry changes,
 * state flips (fullscreen, minimize, restore), focus transitions, and occlusion
 * visibility updates.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: WindowEvent (window/window_event.c)
 * LEVEL: L2 — Behavior (per-window OS lifecycle event registry)
 * ============================================================================
 * SUMMARY:
 *   The fn-pointer lifecycle contract for ONE window. Embedded in the window's
 *   platform state (one instance per Window), filled once by the app, fired by
 *   the pump pass on Thread 0. Every slot is nullable and every fire is a
 *   no-op on a null slot — a window that ignores an event costs nothing and
 *   never crashes (the Cold-Strict, Hot-Minimal Validation Law).
 *
 *   Focus is Application-level, pressed is Window-level, quit is vetoable:
 *     - onFocusGained/onFocusLost fire only on key-window flips.
 *     - onPressed fires on mouse-down regardless of prior focus.
 *     - onQuitRequested (close btn / Cmd+Q / Cmd+W / Alt+F4) returns bool:
 *       true = may quit. A null slot allows the quit (default-proceed).
 *
 *   The fire dispatchers are the ONLY code that reads the slot pointers — the
 *   null-checks live here, nowhere else.
 *
 * STRUCT FIELDS:
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
 *   WindowOcclusionFn onOcclusionChanged;   // occlusion flipped (visible flag)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - WindowEvent_init(self)                : zeros every slot
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - WindowEvent_fireQuitRequested(self, window) : Dispatches quit query (returns true if may quit)
 *   - WindowEvent_fireResized(self, window, width, height) : Dispatches resize notification
 *   - WindowEvent_fireFullscreen(self, window)    : Dispatches fullscreen transition
 *   - WindowEvent_fireMinimized(self, window)     : Dispatches minimize transition
 *   - WindowEvent_fireRestored(self, window)      : Dispatches restore transition
 *   - WindowEvent_firePressed(self, window)       : Dispatches mouse-down notification
 *   - WindowEvent_fireFocusGained(self, window)   : Dispatches focus gained notification
 *   - WindowEvent_fireFocusLost(self, window)     : Dispatches focus lost notification
 *   - WindowEvent_fireZoomFilled(self, window)    : Dispatches header zoom notification
 *   - WindowEvent_fireOcclusionChanged(self, window, visible) : Dispatches occlusion update
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
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
 *   - WindowEvent_setOnOcclusionChanged(self, fn)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
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
 *   - WindowEvent_getOnOcclusionChanged(self)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// CONSTRUCTORS (PUBLIC & PRIVATE)
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
    (*self).onOcclusionChanged = nullptr;
    return true;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
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

void WindowEvent_fireOcclusionChanged(WindowEvent *self, Window *window, bool visible) {
    if(self == nullptr)
        return;
    if((*self).onOcclusionChanged == nullptr)
        return;
    (*self).onOcclusionChanged((*self).self, window, visible);
}

// SETTERS (PUBLIC & PRIVATE)

;;SETTER
void WindowEvent_setSelf(WindowEvent *self, void *owner) {
    if(self == nullptr)
        return;
    (*self).self = owner;
}

;;SETTER
void WindowEvent_setOnQuitRequested(WindowEvent *self, WindowQuitRequestedFn fn) {
    if(self == nullptr)
        return;
    (*self).onQuitRequested = fn;
}

;;SETTER
void WindowEvent_setOnResized(WindowEvent *self, WindowResizedFn fn) {
    if(self == nullptr)
        return;
    (*self).onResized = fn;
}

;;SETTER
void WindowEvent_setOnFullscreen(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFullscreen = fn;
}

;;SETTER
void WindowEvent_setOnMinimized(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onMinimized = fn;
}

;;SETTER
void WindowEvent_setOnRestored(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onRestored = fn;
}

;;SETTER
void WindowEvent_setOnPressed(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onPressed = fn;
}

;;SETTER
void WindowEvent_setOnFocusGained(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFocusGained = fn;
}

;;SETTER
void WindowEvent_setOnFocusLost(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFocusLost = fn;
}

;;SETTER
void WindowEvent_setOnZoomFilled(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onZoomFilled = fn;
}

;;SETTER
void WindowEvent_setOnOcclusionChanged(WindowEvent *self, WindowOcclusionFn fn) {
    if(self == nullptr)
        return;
    (*self).onOcclusionChanged = fn;
}

// GETTERS (PUBLIC & PRIVATE)

;;GETTER
void *WindowEvent_getSelf(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).self;
}

;;GETTER
WindowQuitRequestedFn WindowEvent_getOnQuitRequested(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onQuitRequested;
}

;;GETTER
WindowResizedFn WindowEvent_getOnResized(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onResized;
}

;;GETTER
WindowNoArgFn WindowEvent_getOnFullscreen(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFullscreen;
}

;;GETTER
WindowNoArgFn WindowEvent_getOnMinimized(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onMinimized;
}

;;GETTER
WindowNoArgFn WindowEvent_getOnRestored(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onRestored;
}

;;GETTER
WindowNoArgFn WindowEvent_getOnPressed(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onPressed;
}

;;GETTER
WindowNoArgFn WindowEvent_getOnFocusGained(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFocusGained;
}

;;GETTER
WindowNoArgFn WindowEvent_getOnFocusLost(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFocusLost;
}

;;GETTER
WindowNoArgFn WindowEvent_getOnZoomFilled(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onZoomFilled;
}

;;GETTER
WindowOcclusionFn WindowEvent_getOnOcclusionChanged(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onOcclusionChanged;
}