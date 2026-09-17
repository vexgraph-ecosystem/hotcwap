// window/window_cocoa.m — the AppKit window backend (the one ObjC file).
//
// vex's core is pure C23; this is the ONE file that talks to AppKit, because
// NSWindow/NSApplication are ObjC objects and there is no pure-C way to
// create them. Everything above this boundary stays C; everything here is
// "dip into the OS, hand back a handle, pump the OS event queue".
//
// This is the FRESH window backend, rebuilt from scratch after the trash-era
// shim retired. It is deliberately LEAN: a window with bridges, and nothing
// else. It creates and owns an NSWindow, mirrors the OS's size/move/focus/
// monitor state into C-visible words, fires the per-window WindowEvent
// lifecycle (quit veto, resized, fullscreen, minimized, restored, pressed,
// focus, zoom), and routes OS input into the vexspoke Key/Mouse/Touch rings.
// There is NO Vulkan, NO Metal, NO CAMetalLayer, NO board/pane compositing,
// and NO present worker in this file — rendering is the render repos' job
// (graphvex/darling) and reaches the screen through the content view and the
// event bridges, exactly per the Window Decoupling Law (a Window is a dumb
// surface + callback bridge, never a renderer).
//
// The GPU-era composite surface (boards, panes, worker present, software
// frame present) is retained as thin INERT stubs at the bottom of this file
// so the still-unmigrated darling compositor links; each carries an
// ;;INTENTION marker. When darling migrates to the WindowEvent bridge, the
// stubs and their window.h declarations retire together.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <stdatomic.h>
#include <math.h>

#include "window/window.h"
#include "input/focus.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "annotation/overview.h"
#include "annotation/intention.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window_cocoa.m)
 * LEVEL: L4 — Self-Management (AppKit OS window shim owned by the OS)
 * ============================================================================
 * The fresh, lean AppKit window backend. One opaque C handle per NSWindow;
 * the engine loop constructs it, configures the chrome, shows it, then pumps
 * Window_pollEvents once per frame while a render path draws through the
 * content view / event bridges. OS input is routed into the vexspoke device
 * rings (tagged with this window's id); OS lifecycle (quit, resize,
 * fullscreen, minimize, restore, press, focus, zoom) fires the embedded
 * WindowEvent. Zero Vulkan, zero Metal, zero compositing — a Window is a
 * dumb surface + callback bridge per the Window Decoupling Law.
 *
 * STRUCT FIELDS (Mirroring window/window.h incomplete tag — completed here):
 * ----------------------------------------------------------------------------
 *   NSWindow *nsWindow;           // AppKit window (we own it; releasedWhenClosed NO)
 *   WindowDelegate *delegate;     // per-window close/resize/focus delegate
 *   WindowEvent lifecycle;        // OS lifecycle registry (window/window_event.h)
 *   uint32_t id;                  // engine window id (1..N, 0 = FOCUS_BROADCAST)
 *   _Atomic bool shouldClose;     // true once close requested (Thread 0 writes, loop reads)
 *   _Atomic uint64_t sizeGeneration; // resize-reflection counter (thread 0 bumps)
 *   _Atomic int cachedWidth;      // content width at last thread-0 event (any thread reads)
 *   _Atomic int cachedHeight;     // content height at last thread-0 event
 *   double cachedX;               // top-left screen X at last thread-0 event
 *   double cachedY;               // top-left screen Y at last thread-0 event
 *   double cachedContentX;        // CONTENT top-left X (below title bar)
 *   double cachedContentY;        // CONTENT top-left Y (below title bar)
 *   _Atomic bool liveResizing;    // thread 0 during NSViewLiveResize; renderer consumes
 *   _Atomic bool miniaturizing;   // thread 0 during genie minimize; suppress merge
 *   _Atomic int presentMode;      // present pacing (FIFO/IMMEDIATE), pure state
 *   _Atomic bool transparent;     // composite transparency request, pure state
 *   _Atomic uint64_t renderGeneration; // policy-reflection counter (rebuild ticket)
 *   _Atomic(void*) topLayer;      // content board handle (owned by the render repo)
 *   _Atomic(void*) bottomLayer;   // scene board handle (owned by the render repo)
 *   _Atomic bool enabled;         // false mutes ALL OS input for this window
 *   bool lastFocused;             // focus-flip detection during the pump
 *   _Atomic uint32_t monitorId;   // CGDirectDisplayID mirror (0 = unmapped)
 *   WindowCursorType cursorType;  // active OS cursor style
 *   bool lightVisible[3];       // macOS traffic-light visibility (close/mini/zoom)
 *   float lightOX;              // traffic-light cluster offset X from native layout
 *   float lightOY;              // traffic-light cluster offset Y from native layout
 *   bool lightBaseSet;          // lightBase[] snapshot taken yet
 *   NSRect lightBase[3];        // native close/mini/zoom frames at snapshot time
 *   WindowResizeRenderFn resizeRenderFn;  // resize-cadence render hook
 *   void *resizeRenderUserdata;   // hook userdata
 *
 * PRIVATE HELPERS (kept file-local, no external API):
 * ----------------------------------------------------------------------------
 *   WindowContentView : NSView      — flipped content view (top-left origin)
 *     BOOL active;                  // unused; class exists for flipped geometry
 *   WindowAppDelegate : NSObject
 *     (implements applicationShouldTerminateAfterLastWindowClosed)
 *   WindowDelegate : NSObject <NSWindowDelegate>
 *     atomic_bool *shouldClosePtr;  // weak assignment into the owning handle
 *     Window *handlePtr;            // weak assignment back to the owning handle
 *   WindowSlot                      — id-registry slot record
 *     NSWindow *window;             // OS window owning this id
 *     Window *handle;               // C handle owning that window
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Window_0(void)
 *   - Window_1(title)
 *   - Window_3(title, width, height)
 *   - Window_new(desc)
 *   - Window_create(title, width, height)
 *
 * Core Functions:
 *   - Window_destroy(window)          : detach delegate, close, free handle
 *   - Window_shouldClose(window)
 *   - Window_pollEvents(void)         : drain OS queue once per frame
 *   - routeEvent(event)               : OS event -> device rings + WindowEvent
 *   - windowAlloc(desc)               : shared constructor core
 *   - Window_width(window) / Window_height(window)
 *   - Window_dispatchEvents(window)
 *   - Window_compositePanes/window)   : inert (;;INTENTION)
 *   - Window_compositeBoards(window)  : inert (;;INTENTION)
 *   - Window_orderLayers(window)      : inert (;;INTENTION)
 *   - Window_attachPanes/resizePanes  : inert (;;INTENTION)
 *   - Window_present(window, frame)   : inert (;;INTENTION)
 *   - Window_workerPresentBegin/End   : inert (;;INTENTION)
 *   - Window_contentView(window)
 *   - Window_metalLayer(window)       : nullptr — no Metal here (;;INTENTION)
 *
 * Setters:
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setLocation(window, x, y)
 *   - Window_center(window)
 *   - Window_show(window) / Window_hide(window) / Window_setVisible(window, v)
 *   - Window_setTopLayer/BottomLayer(window, layer)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)
 *   - Window_setEnabled(window, enabled)
 *   - Window_setResizable/Closable/Miniaturizable(window, flag)
 *   - Window_setFullscreenButton(window, enabled)
 *   - Window_setUndecorated(window, mode)
 *   - Window_setFloatingTrafficLights(window, floating)
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)
 *   - Window_setOpacity(window, opacity)
 *   - Window_setTransparentBackground(window, transparent)
 *   - Window_setBlur(window, blur)
 *   - Window_setAlwaysOnTop(window, onTop)
 *   - Window_setClickThrough(window, clickThrough)
 *   - Window_setShadow(window, shadow)
 *   - Window_setMovableByBackground(window, movable)
 *   - Window_setFullscreen(window, fullscreen) / Window_toggleFullscreen(window)
 *   - Window_setDRM(window, enabled)
 *   - Window_setMinSize/MaxSize(window, width, height)
 *   - Window_setCursorType(window, type)
 *   - Window_setCursorLocked(window, locked)
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_addKeyAdapter/MouseAdapter/TouchAdapter(window, adapter)
 *   - Window_focus(window)
 *   - Window_setGravityTopLeft(window)  : no-op (;;INTENTION)
 *
 * Getters:
 *   - Window_getLocation(window, outX, outY)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_getTopLayer/BottomLayer(window)
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_renderGeneration(window)
 *   - Window_isEnabled(window)
 *   - Window_isLiveResizing(window)
 *   - Window_isResizable/Closable/Miniaturizable(window)
 *   - Window_macOS_isTrafficLightButtonVisible(window, light)
 *   - Window_macOS_getTrafficLightHeaderPosition(window, outX, outY)
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *   - Window_getCursorType(window)
 *   - Window_removeKeyAdapter/MouseAdapter/TouchAdapter(window, adapter)
  *   - Window_id(window)
  *   - Window_isFocused(window)
  *   - Window_getLifecycle(window)
  *   - Window_getMonitorId(window)
  *   - Window_sizeGeneration(window)
 * ============================================================================
 */
;;INTENTION("GPU-era composite surface (attachPanes/resizePanes/compositePanes/compositeBoards/orderLayers/metalLayer/setGravityTopLeft/workerPresentBegin/workerPresentEnd/present) is retained as inert stubs so the still-unmigrated darling compositor keeps linking; zero Vulkan/Metal code lives in this file. They retire together with their window.h declarations once darling migrates onto the WindowEvent bridge (the Window Decoupling Law).")
;;INTENTION("presentMode/transparent/renderGeneration are pure atomic policy state (swapchain-rebuild tickets for a future render path), not GPU calls; no render logic exists here.")

// Multi-tap window for double-click style counting (legacy parity: 250ms).
static const uint64_t kTapThresholdNanos = 250000000ULL;

// Carbon virtual keycode -> KEY_* code. -1 = unmapped. Same table the legacy
// macOSWindow built (physical F1-F12, not Fn-doubled media keys).
static int macKeyMap[128] = {
    [0] = KEY_A,                   [1] = KEY_S,
    [2] = KEY_D,                   [3] = KEY_F,
    [4] = KEY_H,                   [5] = KEY_G,
    [6] = KEY_Z,                   [7] = KEY_X,
    [8] = KEY_C,                   [9] = KEY_V,
    [11] = KEY_B,                  [12] = KEY_Q,
    [13] = KEY_W,                  [14] = KEY_E,
    [15] = KEY_R,                  [16] = KEY_Y,
    [17] = KEY_T,                  [18] = KEY_NUM_1,
    [19] = KEY_NUM_2,              [20] = KEY_NUM_3,
    [21] = KEY_NUM_4,              [22] = KEY_NUM_6,
    [23] = KEY_NUM_5,              [24] = KEY_EQUAL,
    [25] = KEY_NUM_9,              [26] = KEY_NUM_7,
    [27] = KEY_MINUS,              [28] = KEY_NUM_8,
    [29] = KEY_NUM_0,              [30] = KEY_RIGHT_BRACKET,
    [31] = KEY_O,                  [32] = KEY_U,
    [33] = KEY_LEFT_BRACKET,       [34] = KEY_I,
    [35] = KEY_P,                  [36] = KEY_ENTER,
    [37] = KEY_L,                  [38] = KEY_J,
    [39] = KEY_APOSTROPHE,         [40] = KEY_K,
    [41] = KEY_SEMICOLON,          [42] = KEY_BACKSLASH,
    [43] = KEY_COMMA,              [44] = KEY_SLASH,
    [45] = KEY_N,                  [46] = KEY_M,
    [47] = KEY_PERIOD,             [48] = KEY_TAB,
    [49] = KEY_SPACE,              [50] = KEY_GRAVE_ACCENT,
    [51] = KEY_BACKSPACE,          [53] = KEY_ESCAPE,
    [54] = KEY_RIGHT_SUPER,        [55] = KEY_LEFT_SUPER,
    [56] = KEY_LEFT_SHIFT,         [57] = KEY_CAPS_LOCK,
    [58] = KEY_LEFT_ALT,           [59] = KEY_LEFT_CONTROL,
    [60] = KEY_RIGHT_SHIFT,        [61] = KEY_RIGHT_ALT,
    [62] = KEY_RIGHT_CONTROL,      [63] = KEY_FN,
    [96] = KEY_F5,                 [97] = KEY_F6,
    [98] = KEY_F7,                 [99] = KEY_F3,
    [100] = KEY_F8,                [101] = KEY_F9,
    [103] = KEY_F11,               [109] = KEY_F10,
    [111] = KEY_F12,               [118] = KEY_F4,
    [120] = KEY_F2,                [122] = KEY_F1,
    [123] = KEY_LEFT,              [124] = KEY_RIGHT,
    [125] = KEY_DOWN,              [126] = KEY_UP,
};

// Cursor-lock state. While locked the pointer is decoupled from motion and
// re-warped to the anchor centre every pump pass; NSEvent deltas feed mouse.
static bool s_cursorLocked = false;
static CGPoint s_lockCenter = {0, 0};

@class WindowDelegate;

// The opaque handle handed back to C. Holds the NS objects we must keep alive
// (window + delegate) plus every C-visible reflection word. `id` is the
// engine's small window number (1..N; 0 is the broadcast reserved id) used to
// tag input events and route them to per-window listeners. `lifecycle` is the
// embedded WindowEvent — the app's OS-lifecycle bridge (the Window Decoupling
// Law: the window publishes live state and callbacks; it never renders).
struct Window {
    NSWindow *nsWindow;
    WindowDelegate *delegate;
    WindowEvent lifecycle;
    uint32_t id;
    _Atomic bool shouldClose;
    _Atomic uint64_t sizeGeneration;
    _Atomic int cachedWidth;
    _Atomic int cachedHeight;
    double cachedX;
    double cachedY;
    double cachedContentX;
    double cachedContentY;
    _Atomic bool liveResizing;
    _Atomic bool miniaturizing;

    // Pure-state reflection words (no GPU calls in this file).
    _Atomic int presentMode;
    _Atomic bool transparent;
    _Atomic uint64_t renderGeneration;

    // Board slots — opaque layer handles consumed by graphvex/darling; this
    // window stores them, never dereferences them. Panels live on the Frame.
    _Atomic(void*) topLayer;
    _Atomic(void*) bottomLayer;

    _Atomic bool enabled;
    _Atomic bool keyEnabled; // false = canBecomeKeyWindow refuses (held by a modal dialog)
    bool lastFocused;
    _Atomic uint32_t monitorId;
    WindowCursorType cursorType;

    // Traffic-light chrome (NAKED full-size content floats them over content):
    // per-button visibility + a cluster offset relative to the native layout,
    // snapshotted whenever the style mask changes so the offset always reads
    // "shifted from the current native layout".
    bool lightVisible[3];  // close / mini / zoom
    float lightOX;         // cluster offset in points from the native layout
    float lightOY;
    bool lightBaseSet;     // lightBase[] snapshotted yet
    NSRect lightBase[3];   // native close/mini/zoom frames at snapshot time

    WindowResizeRenderFn resizeRenderFn;
    void *resizeRenderUserdata;
};

// Window id registry: slot i holds the entries for engine id i (index = id,
// slot 0 reserved for FOCUS_BROADCAST). Grows by doubling (the Dynamic
// Scalability & Anti-Hardcoding Law — no fixed window ceiling; the pump and
// the id scan are the only walkers, and live windows are few).
typedef struct WindowSlot {
    NSWindow *window;
    Window *handle;
} WindowSlot;

static WindowSlot *s_refs = nullptr;
static uint32_t s_refCap = 0;

static bool windowRefsGrow(void) {
    uint32_t nextCap = (s_refCap == 0) ? 8u : s_refCap * 2u;
    WindowSlot *grown = (WindowSlot*) realloc(s_refs, (size_t) nextCap * sizeof(WindowSlot));
    if (grown == nullptr)
        return false;
    for (uint32_t i = s_refCap; i < nextCap; i++) {
        grown[i].window = nil;
        grown[i].handle = nullptr;
    }
    s_refs = grown;
    s_refCap = nextCap;
    return true;
}

// Resolve an id for a newly created window, or 0 when the table cannot grow.
static uint32_t windowIdAcquire(NSWindow *window, Window *handle) {
    if (s_refCap == 0 && !windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++) {
        if (s_refs[i].window == nil) {
            s_refs[i].window = window;
            s_refs[i].handle = handle;
            return i;
        }
    }
    if (!windowRefsGrow())
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++) {
        if (s_refs[i].window == nil) {
            s_refs[i].window = window;
            s_refs[i].handle = handle;
            return i;
        }
    }
    return FOCUS_BROADCAST;
}

static void windowIdRelease(uint32_t id) {
    if (id != FOCUS_BROADCAST && id < s_refCap) {
        s_refs[id].window = nil;
        s_refs[id].handle = nullptr;
    }
}

// Reverse lookup: which engine id does this OS window carry? 0 if unknown.
static uint32_t windowIdOf(NSWindow *window) {
    if (window == nil)
        return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < s_refCap; i++)
        if (s_refs[i].window == window)
            return i;
    return FOCUS_BROADCAST;
}

static Window *windowHandleOf(NSWindow *window) {
    if (window == nil)
        return nullptr;
    for (uint32_t i = 1; i < s_refCap; i++)
        if (s_refs[i].window == window)
            return s_refs[i].handle;
    return nullptr;
}

// Flipped content view so sublayers and Cocoa geometry natively speak
// TOP-LEFT coordinates (matching darling Container_resolve layout 1:1). Also
// tracks live-resize start/end onto the handle so Window_isLiveResizing works
// without any GPU state.
@interface WindowContentView : NSView
@end

@implementation WindowContentView
- (BOOL)isFlipped {
    return YES;
}

- (void)viewWillStartLiveResize {
    Window *w = windowHandleOf([self window]);
    if (w)
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
    [super viewWillStartLiveResize];
}

- (void)viewDidEndLiveResize {
    Window *w = windowHandleOf([self window]);
    if (w)
        atomic_store_explicit(&(*w).liveResizing, false, memory_order_relaxed);
    [super viewDidEndLiveResize];
}
@end

// Resize reflection helper (defined after the delegate). Thread 0 only.
static void windowRefreshSize(Window *window);

// App-level delegate: lets the process end when the last window closes.
@interface WindowAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation WindowAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*) sender {
    (void) sender;
    return YES;
}
@end

// Window-level delegate: this is how we learn the user clicked the close
// button, resized, zoomed, or used the green orb. The pointers are (assign)
// because the delegate must not own our C struct.
@interface WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) atomic_bool *shouldClosePtr;
@property(nonatomic, assign) Window *handlePtr;
@end

@implementation WindowDelegate
- (BOOL)windowShouldClose:(NSWindow*) sender {
    (void) sender;
    Window *w = self.handlePtr;
    if (w)
        return WindowEvent_fireQuitRequested(&(*w).lifecycle, w);
    return YES;
}

- (void)windowWillClose:(NSNotification*) notification {
    (void) notification;
    if (self.shouldClosePtr)
        atomic_store_explicit(self.shouldClosePtr, true, memory_order_relaxed);
}

- (void)windowDidResize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        windowRefreshSize(w);
}

- (void)windowWillEnterFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
}

- (void)windowDidEnterFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).liveResizing, false, memory_order_relaxed);
        WindowEvent_fireFullscreen(&(*w).lifecycle, w);
    }
}

- (void)windowWillExitFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
}

- (void)windowDidExitFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).liveResizing, false, memory_order_relaxed);
        WindowEvent_fireRestored(&(*w).lifecycle, w);
    }
}

- (void)windowWillMiniaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).miniaturizing, true, memory_order_relaxed);
}

- (void)windowDidMiniaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).miniaturizing, false, memory_order_relaxed);
        WindowEvent_fireMinimized(&(*w).lifecycle, w);
    }
}

- (void)windowWillDeminiaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(&(*w).miniaturizing, true, memory_order_relaxed);
}

- (void)windowDidDeminiaturize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).miniaturizing, false, memory_order_relaxed);
        WindowEvent_fireRestored(&(*w).lifecycle, w);
    }
}

// Zoom-to-fill (green button / double-click titlebar): publish the lifecycle
// event before AppKit performs the standard-frame change.
- (BOOL)windowShouldZoom:(NSWindow*) sender toFrame:(NSRect)newFrame {
    (void) sender;
    (void) newFrame;
    Window *w = self.handlePtr;
    if (w)
        WindowEvent_fireZoomFilled(&(*w).lifecycle, w);
    return YES;
}

// Occlusion flip (buried under windows / minimized / hidden Space / ordered
// out): publish the lifecycle event with current visibility. An ordered-out
// window reads zero state bits, so it reports not-visible and the renderer
// rests. The notification itself is the flip signal — no extra cache.
- (void)windowDidChangeOcclusionState:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        bool visible = false;
        if ((*w).nsWindow != nil)
            visible = ([(*w).nsWindow occlusionState] & NSWindowOcclusionStateVisible) != 0;
        WindowEvent_fireOcclusionChanged(&(*w).lifecycle, w, visible);
    }
}
@end

// Resize reflection: compare the live content size against the cache and bump
// sizegen + fire onResized + run the resize hook only on an actual change, so
// a renderer polling once per frame pays one int compare. Thread 0 only.
static void windowRefreshSize(Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return;
    @autoreleasepool {
        NSRect content = [(*window).nsWindow contentRectForFrameRect:[(*window).nsWindow frame]];
        int cw = (int) content.size.width;
        int ch = (int) content.size.height;
        if (cw != atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed)
            || ch != atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed)) {
            atomic_store_explicit(&(*window).cachedWidth, cw, memory_order_relaxed);
            atomic_store_explicit(&(*window).cachedHeight, ch, memory_order_relaxed);
            atomic_fetch_add_explicit(&(*window).sizeGeneration, 1, memory_order_release);
            WindowEvent_fireResized(&(*window).lifecycle, window, cw, ch);
            if ((*window).resizeRenderFn)
                (*window).resizeRenderFn((*window).resizeRenderUserdata);
        }
    }
}

// Mirror the OS key-window into the focus word AND the WindowEvent focus
// slots (flips only). Thread 0 only, called at the end of each pump pass.
static void windowRefreshFocus(Window *window, NSWindow *keyWindow) {
    if (window == nullptr || (*window).nsWindow == nil)
        return;
    bool focused = (windowIdOf(keyWindow) == (*window).id);
    if (focused != (*window).lastFocused) {
        (*window).lastFocused = focused;
        if (focused)
            WindowEvent_fireFocusGained(&(*window).lifecycle, window);
        else
            WindowEvent_fireFocusLost(&(*window).lifecycle, window);
    }
}

// Mirror the OS display assignment into the atomic word; 0 means unmapped.
static uint32_t resolveMonitorId(Window *window) {
    if (window == nullptr)
        return 0;
    @autoreleasepool {
        NSScreen *screen = [(*window).nsWindow screen];
        if (screen == nil)
            return 0;
        NSNumber *displayId = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        return displayId ? (uint32_t) [displayId unsignedIntValue] : 0;
    }
}

static void windowRefreshMonitor(Window *window) {
    if (window == nullptr)
        return;
    uint32_t next = resolveMonitorId(window);
    atomic_store_explicit(&(*window).monitorId, next, memory_order_relaxed);
}

// Re-centre the cursor during the pump while locked, so the warp registers
// before the next event loop exit.
static void recenterIfLocked(void) {
    if (s_cursorLocked)
        CGWarpMouseCursorPosition(s_lockCenter);
}

// Content-area coordinates (top-left origin) for a mouse event. Events that
// miss every window fall back to raw screen-space values.
static void mouseLocation(NSEvent *event, double *outX, double *outY) {
    NSPoint p = [event locationInWindow];
    double x = p.x;
    double y = p.y;
    NSWindow *eventWindow = [event window];
    if (eventWindow) {
        NSRect content = [[eventWindow contentView] frame];
        y = content.size.height - y;
    }
    *outX = x;
    *outY = y;
}

// Map an NSTouch phase onto the Touch_* action codes.
static int touchAction(NSTouchPhase phase) {
    if (phase & NSTouchPhaseBegan)
        return TOUCH_DOWN;
    if (phase & (NSTouchPhaseMoved | NSTouchPhaseStationary))
        return TOUCH_MOVE;
    if (phase & NSTouchPhaseEnded)
        return TOUCH_UP;
    return TOUCH_CANCEL;
}

// Feed one gesture/touch carrier event into input.Touch: resolve each active
// touch into its slot (identity hash % TOUCH_MAX), normalize position into
// content coords, and estimate pressure from the resting flag.
static void dispatchTouches(NSEvent *event, uint32_t wid) {
    NSWindow *eventWindow = [event window];
    if (eventWindow == nil)
        return;
    NSView *contentView = [eventWindow contentView];
    if (contentView == nil)
        return;

    NSSet *touches = [event touchesMatchingPhase:NSTouchPhaseAny inView:contentView];
    if (touches == nil || touches.count == 0)
        return;

    NSRect frame = [contentView frame];
    double winW = frame.size.width;
    double winH = frame.size.height;

    for (NSTouch *touch in touches) {
        NSUInteger slot = [[touch identity] hash] % TOUCH_MAX;
        NSPoint norm = [touch normalizedPosition];
        double posX = norm.x * winW;
        double posY = (1.0 - norm.y) * winH;
        double pressure = [touch isResting] ? 0.2 : 0.8;
        Touch_pushTouchEvent(wid, (int) slot, touchAction([touch phase]),
                             posX, posY, pressure, kTapThresholdNanos);
    }
}

// Intercept-and-forward: read everything the engine cares about off each OS
// event, push it into the input modules, then hand the event back to AppKit
// so the responder chain keeps working. This is the Thread-0 producer side of
// the input pipeline.
static void routeEvent(NSEvent *event) {
    NSEventType type = [event type];
    uint32_t wid = windowIdOf([event window]);

    // Disabled windows receive nothing: events never enter the device rings.
    Window *target = (wid != FOCUS_BROADCAST) ? windowHandleOf([event window]) : nullptr;
    if (target && !atomic_load_explicit(&(*target).enabled, memory_order_relaxed))
        return;

    switch (type) {
        case NSEventTypeFlagsChanged: {
            short macCode = [event keyCode];
            NSEventModifierFlags flags = [event modifierFlags];
            int stdKey = (macCode >= 0 && macCode < 128) ? macKeyMap[macCode] : -1;
            if (stdKey != -1) {
                bool isDown = false;
                if (macCode == 54 || macCode == 55) {
                    isDown = (flags & NSEventModifierFlagCommand) != 0;
                } else if (macCode == 56 || macCode == 60) {
                    isDown = (flags & NSEventModifierFlagShift) != 0;
                } else if (macCode == 58 || macCode == 61) {
                    isDown = (flags & NSEventModifierFlagOption) != 0;
                } else if (macCode == 59 || macCode == 62) {
                    isDown = (flags & NSEventModifierFlagControl) != 0;
                } else if (macCode == 57) {
                    isDown = (flags & NSEventModifierFlagCapsLock) != 0;
                }
                Key_pushEvent(wid, stdKey,
                              isDown ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                              kTapThresholdNanos);
            }
            break;
        }

        case NSEventTypeKeyDown:
        case NSEventTypeKeyUp: {
            NSEventModifierFlags flags = [event modifierFlags];
            bool cmd = (flags & NSEventModifierFlagCommand) != 0;
            if (cmd != (Key_isDown(KEY_LEFT_SUPER) || Key_isDown(KEY_RIGHT_SUPER))) {
                Key_pushEvent(wid, KEY_LEFT_SUPER, cmd ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            bool ctrl = (flags & NSEventModifierFlagControl) != 0;
            if (ctrl != (Key_isDown(KEY_LEFT_CONTROL) || Key_isDown(KEY_RIGHT_CONTROL))) {
                Key_pushEvent(wid, KEY_LEFT_CONTROL, ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            bool alt = (flags & NSEventModifierFlagOption) != 0;
            if (alt != (Key_isDown(KEY_LEFT_ALT) || Key_isDown(KEY_RIGHT_ALT))) {
                Key_pushEvent(wid, KEY_LEFT_ALT, alt ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            bool shift = (flags & NSEventModifierFlagShift) != 0;
            if (shift != (Key_isDown(KEY_LEFT_SHIFT) || Key_isDown(KEY_RIGHT_SHIFT))) {
                Key_pushEvent(wid, KEY_LEFT_SHIFT, shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            }
            short macCode = [event keyCode];
            if (macCode >= 0 && macCode < 128) {
                int stdKey = macKeyMap[macCode];
                if (stdKey != -1)
                    Key_pushEvent(wid, stdKey,
                                  type == NSEventTypeKeyDown ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                  kTapThresholdNanos);
                if (type == NSEventTypeKeyDown && stdKey != -1) {
                    NSString *chars = [event characters];
                    if (chars.length > 0) {
                        unsigned char c0 = (unsigned char) [chars characterAtIndex:0];
                        if (c0 > 0)
                            Key_pushCharEvent(wid, c0);
                    }
                }
            }
            break;
        }

        case NSEventTypeScrollWheel:
            Mouse_pushScrollEvent(wid, [event scrollingDeltaX], [event scrollingDeltaY]);
            break;

        case NSEventTypeMagnify:
            Mouse_pushZoomEvent(wid, [event magnification]);
            break;

        case NSEventTypeLeftMouseDown:
        case NSEventTypeRightMouseDown:
        case NSEventTypeOtherMouseDown: {
            int button = (type == NSEventTypeLeftMouseDown) ? MOUSE_LEFT
                       : ((type == NSEventTypeRightMouseDown) ? MOUSE_RIGHT
                                                              : (int) [event buttonNumber]);
            double x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushButtonEvent(wid, button, KEY_ACTION_DOWN, kTapThresholdNanos);
            if (target)
                WindowEvent_firePressed(&(*target).lifecycle, target);
            break;
        }

        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseUp: {
            int button = (type == NSEventTypeLeftMouseUp) ? MOUSE_LEFT
                       : ((type == NSEventTypeRightMouseUp) ? MOUSE_RIGHT
                                                             : (int) [event buttonNumber]);
            double x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushButtonEvent(wid, button, KEY_ACTION_UP, kTapThresholdNanos);
            break;
        }

        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged: {
            if (s_cursorLocked) {
                Mouse_pushMoveDeltaEvent(wid, [event deltaX], [event deltaY]);
                break;
            }
            double x, y;
            mouseLocation(event, &x, &y);
            if (type == NSEventTypeMouseMoved) {
                Mouse_pushMoveEvent(wid, x, y);
            } else {
                int button = (type == NSEventTypeLeftMouseDragged) ? MOUSE_LEFT
                           : ((type == NSEventTypeRightMouseDragged) ? MOUSE_RIGHT
                                                                      : (int) [event buttonNumber]);
                Mouse_pushDragEvent(wid, button, x, y);
            }
            break;
        }

        case NSEventTypeGesture:
        case NSEventTypeBeginGesture:
        case NSEventTypeEndGesture:
            dispatchTouches(event, wid);
            break;

        default:
            break;
    }
}

// Drain the OS event queue. Called every frame from the engine loop (the
// "poll" half of poll-then-tick). Returns immediately; never blocks. After
// the pump, mirror the OS's key window into the focus word so the rest of
// the engine can ask "who is focused?" without touching AppKit.
static NSWindow *sLastWindow = nil;
// Key-window claim pending: set by Window_show/Window_focus, drained by the
// pump once the app is active and the claim sticks.
static NSWindow *sPendingKeyWindow = nil;
static WindowAppDelegate *sAppDelegate = nil; // one app delegate for the whole process

void Window_pollEvents(void) {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES])) {
            routeEvent(event);
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
        recenterIfLocked();
        Focus_set(windowIdOf([NSApp keyWindow]));

        if (sPendingKeyWindow) {
            if ([NSApp keyWindow] == sPendingKeyWindow) {
                sPendingKeyWindow = nil;
            } else if ([NSApp isActive] && [sPendingKeyWindow canBecomeKeyWindow]) {
                [sPendingKeyWindow makeKeyWindow];
                if ([NSApp keyWindow] == sPendingKeyWindow)
                    sPendingKeyWindow = nil;
            }
        }

        // Reflection pass: size/move/focus/monitor mirroring per live window.
        NSWindow *keyWindow = [NSApp keyWindow];
        for (uint32_t i = 1; i < s_refCap; i++) {
            NSWindow *w = s_refs[i].window;
            if (w == nil)
                continue;
            Window *handle = s_refs[i].handle;
            if (handle == nullptr)
                continue;
            windowRefreshSize(handle);

            // Move reflection: top-left screen coords, same space
            // setLocation speaks. No WindowEvent slot (the registry has no
            // onMoved) — the cache feeds Window_getLocation only.
            NSRect frame = [w frame];
            NSRect content = [w contentRectForFrameRect:frame];
            CGFloat screenHeight = [[NSScreen mainScreen] frame].size.height;
            double tx = (double) frame.origin.x;
            double ty = (double) (screenHeight - frame.origin.y - frame.size.height);
            (*handle).cachedX = tx;
            (*handle).cachedY = ty;
            double cx = (double) frame.origin.x;
            double cy = (double) (screenHeight - content.origin.y - content.size.height);
            (*handle).cachedContentX = cx;
            (*handle).cachedContentY = cy;

            // Focus flip: mirror the OS spotlight into the WindowEvent slots.
            windowRefreshFocus(handle, keyWindow);
            // Monitor mirror: resolve the carrying display, flip the atomic.
            windowRefreshMonitor(handle);
        }
    }
}

// Traffic-light indices into the lightVisible[]/lightBase[] arrays (kept
// beside the constructors: windowAlloc seeds visibility, refreshChrome and
// the Window_macOS_* setters consume it further below).
enum { LIGHT_CLOSE = 0, LIGHT_MINI = 1, LIGHT_ZOOM = 2 };

// Key-gate window subclass: while a modal dialog holds its parent, the
// parent's C flag flips and AppKit itself refuses it key — no focus flash,
// no input gap, no yank-back. Render + ordering are untouched (only key).
@interface VexWindow : NSWindow
@property (assign, nonatomic) Window *vexHandle;
@end

@implementation VexWindow
- (BOOL)canBecomeKeyWindow {
    Window *h = self.vexHandle;
    if (h != NULL && !atomic_load_explicit(&(*h).keyEnabled, memory_order_relaxed))
        return NO;
    return [super canBecomeKeyWindow];
}
@end

// Build the NSWindow + C handle. Shared by every constructor. The window is
// created HIDDEN — visibility is an explicit Window_show() decision, so
// construct -> mutate -> show never flashes a half-configured window.
// Initial placement also happens here (not via a post-creation move):
// creating already-placed keeps AppKit's move tracker quiet — a
// setFrameOrigin: on a never-shown window logs "move completed
// without beginning".
static Window *windowAlloc(const WindowDesc *desc) {
    @autoreleasepool {
        if (NSApp == nil)
            [NSApplication sharedApplication];
        if (sAppDelegate == nil) {
            sAppDelegate = [[WindowAppDelegate alloc] init];
            [NSApp setDelegate:sAppDelegate];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }

        NSWindowStyleMask style = NSWindowStyleMaskTitled
                                | NSWindowStyleMaskClosable
                                | NSWindowStyleMaskMiniaturizable
                                | NSWindowStyleMaskResizable;

        // Resolve the content rect so the resulting FRAME lands placed:
        // centered in the main screen's visible frame by default, or at the
        // caller's top-left x/y. frameRectForContentRect gives the chrome
        // size, so centering is frame-exact (not off by half a titlebar).
        CGFloat cw = (CGFloat)(*desc).width;
        CGFloat ch = (CGFloat)(*desc).height;
        NSRect wantFrame = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, cw, ch)
                                                  styleMask:style];
        if ((*desc).centered || ((*desc).x == 0 && (*desc).y == 0)) {
            NSRect avail = [[NSScreen mainScreen] visibleFrame];
            wantFrame.origin.x = avail.origin.x + (avail.size.width - wantFrame.size.width) * 0.5;
            wantFrame.origin.y = avail.origin.y + (avail.size.height - wantFrame.size.height) * 0.5;
        } else {
            NSRect screen = [[NSScreen mainScreen] frame];
            wantFrame.origin.x = (CGFloat)(*desc).x;
            wantFrame.origin.y = screen.size.height - (CGFloat)(*desc).y - wantFrame.size.height;
        }
        wantFrame.origin.x = (CGFloat) floor(wantFrame.origin.x + 0.5);
        wantFrame.origin.y = (CGFloat) floor(wantFrame.origin.y + 0.5);
        NSRect frame = [NSWindow contentRectForFrameRect:wantFrame
                                              styleMask:style];

        VexWindow *window = [[VexWindow alloc]
            initWithContentRect:frame
                       styleMask:style
                          backing:NSBackingStoreBuffered
                            defer:NO];
        [window setTitle:[NSString stringWithUTF8String:(*desc).title]];
        [window setReleasedWhenClosed:NO];

        WindowContentView *contentView = [[WindowContentView alloc] initWithFrame:frame];
        [contentView setWantsLayer:YES];
        [window setContentView:contentView];

        // Green traffic light enters native fullscreen.
        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];

        // Live resize never stretches preserved content (the Continuous
        // Real-Time Live Resize Law).
        [window setPreservesContentDuringLiveResize:NO];

        // Hover delivery: without this, MouseMoved only fires mid-drag and
        // darling hover is dead on arrival. The Mouse ring gets every move.
        [window setAcceptsMouseMovedEvents:YES];

        // Trackpad touch delivery: the content view must opt in.
        [window.contentView setAllowedTouchTypes:NSTouchTypeMaskDirect | NSTouchTypeMaskIndirect];

        WindowDelegate *delegate = [[WindowDelegate alloc] init];
        [window setDelegate:delegate];

        Window *w = (Window*) calloc(1, sizeof(Window));
        if (w == nullptr)
            return nullptr;
        (*w).nsWindow = window;
        window.vexHandle = w;
        (*w).delegate = delegate;
        WindowEvent_init(&(*w).lifecycle);
        atomic_store_explicit(&(*w).shouldClose, false, memory_order_relaxed);
        atomic_store_explicit(&(*w).sizeGeneration, 0, memory_order_relaxed);
        NSRect initialContent = [window contentRectForFrameRect:[window frame]];
        atomic_store_explicit(&(*w).cachedWidth, (int) initialContent.size.width, memory_order_relaxed);
        atomic_store_explicit(&(*w).cachedHeight, (int) initialContent.size.height, memory_order_relaxed);
        NSRect initialFrame = [window frame];
        CGFloat screenH = [[NSScreen mainScreen] frame].size.height;
        (*w).cachedX = (double) initialFrame.origin.x;
        (*w).cachedY = (double) (screenH - initialFrame.origin.y - initialFrame.size.height);
        (*w).cachedContentX = (double) initialFrame.origin.x
                              + (double) (initialContent.origin.x - initialFrame.origin.x);
        (*w).cachedContentY = (double) (screenH - initialContent.origin.y - initialContent.size.height);
        atomic_store_explicit(&(*w).presentMode, WINDOW_PRESENT_FIFO, memory_order_relaxed);
        atomic_store_explicit(&(*w).transparent, false, memory_order_relaxed);
        atomic_store_explicit(&(*w).renderGeneration, 0, memory_order_relaxed);
        atomic_store_explicit(&(*w).topLayer, nullptr, memory_order_relaxed);
        atomic_store_explicit(&(*w).bottomLayer, nullptr, memory_order_relaxed);
        atomic_store_explicit(&(*w).enabled, true, memory_order_relaxed);
        atomic_store_explicit(&(*w).keyEnabled, true, memory_order_relaxed);
        (*w).lastFocused = false;
        atomic_store_explicit(&(*w).monitorId, 0, memory_order_relaxed);
        (*w).cursorType = WINDOW_CURSOR_DEFAULT;
        (*w).lightVisible[LIGHT_CLOSE] = true;
        (*w).lightVisible[LIGHT_MINI] = true;
        (*w).lightVisible[LIGHT_ZOOM] = true;
        (*w).lightOX = 0.0f;
        (*w).lightOY = 0.0f;
        (*w).resizeRenderFn = nullptr;
        (*w).resizeRenderUserdata = nullptr;
        (*w).id = windowIdAcquire(window, w);
        delegate.shouldClosePtr = &(*w).shouldClose;
        delegate.handlePtr = w;

        sLastWindow = window;

        return w;
    }
}

// Merge a caller's Desc over the defaults. Unset (zero) fields fall back —
// this is what makes partial designated initializers behave like overloads.
static WindowDesc descResolve(const WindowDesc *desc) {
    WindowDesc d = { .title = "vex", .width = 800, .height = 600, .x = 0, .y = 0, .centered = true };
    if (desc == nullptr)
        return d;
    if ((*desc).title)
        d.title = (*desc).title;
    if ((*desc).width > 0)
        d.width = (*desc).width;
    if ((*desc).height > 0)
        d.height = (*desc).height;
    if ((*desc).x != 0 || (*desc).y != 0) {
        // Explicit placement defeats the centered default (a zeroed Desc
        // cannot express "top-left", so centered wins ties — documented).
        d.x = (*desc).x;
        d.y = (*desc).y;
        d.centered = false;
    }
    d.shown = (*desc).shown;
    return d;
}

// --- Constructors -----------------------------------------------------------

Window *Window_0(void) {
    return Window_new(nullptr);
}

Window *Window_1(const char *title) {
    return Window_new(&(WindowDesc){ .title = title });
}

Window *Window_3(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

Window *Window_new(const WindowDesc *desc) {
    WindowDesc d = descResolve(desc);
    Window *w = windowAlloc(&d);
    if (w == nullptr)
        return nullptr;
    // Placement already happened inside windowAlloc (centered default or the
    // caller's x/y) — no post-creation move, so AppKit's move tracker stays
    // quiet. Window_center/Window_setLocation remain for later re-placement
    // of a live window.
    if (d.shown)
        Window_show(w);
    return w;
}

Window *Window_create(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Tear down the window and free the handle. Safe to call whether the user
// already closed the window or not: if it's still open we close it, and we
// detach the delegate first so no callback can touch our freed memory.
void Window_destroy(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (sPendingKeyWindow == (*window).nsWindow)
            sPendingKeyWindow = nil;
        [(*window).nsWindow setDelegate:nil];
        if (!atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed))
            [(*window).nsWindow close];
        Key_detachWindowAll((*window).id);
        Mouse_detachWindowAll((*window).id);
        Touch_detachWindowAll((*window).id);
        windowIdRelease((*window).id);
    }
    free(window);
}

bool Window_shouldClose(Window *window) {
    return window ? atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed) : true;
}

void Window_setShouldClose(Window *window, bool shouldClose) {
    if (window != nullptr)
        atomic_store_explicit(&(*window).shouldClose, shouldClose, memory_order_relaxed);
}

// --- Present policy (pure state; a future render path consumes it) ----------

void Window_setPresentMode(Window *window, int mode) {
    if (window == nullptr)
        return;
    int prev = atomic_exchange_explicit(&(*window).presentMode, mode, memory_order_relaxed);
    if (prev != mode)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

int Window_getPresentMode(const Window *window) {
    if (window == nullptr)
        return WINDOW_PRESENT_FIFO;
    return atomic_load_explicit(&(*window).presentMode, memory_order_relaxed);
}

void Window_setTransparent(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    bool prev = atomic_exchange_explicit(&(*window).transparent, transparent, memory_order_relaxed);
    if (prev != transparent)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

bool Window_isTransparent(const Window *window) {
    return window ? atomic_load_explicit(&(*window).transparent, memory_order_relaxed) : false;
}

uint64_t Window_renderGeneration(const Window *window) {
    return window ? atomic_load_explicit(&(*window).renderGeneration, memory_order_acquire) : 0;
}

// --- Graphics board slots (stored + ordered; rendering lives elsewhere) -----

// ;;INTENTION("orderLayers on the fresh window is a pure no-op — the window no longer parents render layers; the still-unmigrated darling compositor retains the call. It retires with the composite seam.")
void Window_orderLayers(Window *window) {
    (void) window;
}

void Window_setBottomLayer(Window *window, void *layer) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).bottomLayer, layer, memory_order_release);
    Window_orderLayers(window);
}

void *Window_getBottomLayer(const Window *window) {
    return window ? atomic_load_explicit(&(*window).bottomLayer, memory_order_acquire) : nullptr;
}

void Window_setTopLayer(Window *window, void *layer) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).topLayer, layer, memory_order_release);
    Window_orderLayers(window);
}

void *Window_getTopLayer(const Window *window) {
    return window ? atomic_load_explicit(&(*window).topLayer, memory_order_acquire) : nullptr;
}

// ;;INTENTION("Pane/board compositing was the GPU-era shim's job; on this window it is inert (still-unmigrated darling compositor retains the call). Pane work migrates to the render repos' own pass; retires together with the composite seam.")
bool Window_attachPanes(Window *window, Panel *panel, int width, int height) {
    (void) window;
    (void) panel;
    (void) width;
    (void) height;
    return false;
}

bool Window_resizePanes(Window *window, Panel *panel, int width, int height) {
    (void) window;
    (void) panel;
    (void) width;
    (void) height;
    return false;
}

void Window_compositePanes(Window *window, Panel *contentPanel) {
    (void) window;
    (void) contentPanel;
}

void Window_compositeBoards(Window *window) {
    (void) window;
}

// --- Runtime state ----------------------------------------------------------

void Window_setEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).enabled, enabled, memory_order_relaxed);
}

bool Window_isEnabled(const Window *window) {
    return window ? atomic_load_explicit(&(*window).enabled, memory_order_relaxed) : false;
}

bool Window_isLiveResizing(const Window *window) {
    return window ? atomic_load_explicit(&(*window).liveResizing, memory_order_relaxed) : false;
}

// --- Chrome / state API -----------------------------------------------------

static NSWindowStyleMask styleMaskOf(Window *window) {
    return [(*window).nsWindow styleMask];
}

static bool hasStyleBit(Window *window, NSWindowStyleMask bit) {
    return (styleMaskOf(window) & bit) != 0;
}

// Blur is banned on DECORATED chrome: the opaque titlebar + frosted glass
// reads as a rendering bug. Detect it as titled-with-full-size-content-view
// (NAKED) or zero-style (BORDERLESS) being the allowed blur carriers.
static bool windowChromeIsDecorated(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskTitled)
        && !hasStyleBit(window, NSWindowStyleMaskFullSizeContentView);
}

// Single mask-rewrite path for all capability toggles. While native fullscreen
// AppKit owns the mask, so style mutations are skipped then.
static void updateStyleMask(Window *window, NSWindowStyleMask add, NSWindowStyleMask clear) {
    NSWindowStyleMask mask = styleMaskOf(window);
    if ((mask & NSWindowStyleMaskFullScreen) != 0)
        return;
    [(*window).nsWindow setStyleMask:(mask & ~clear) | add];
}

// Re-apply the traffic-light chrome: per-button visibility, then re-seat the
// cluster offset relative to the native layout (snapshotted on first touch;
// the snapshot is invalidated whenever the style mask changes). Pure public
// AppKit (standardWindowButton: + setHidden:/setFrameOrigin:) — no private API.
// BORDERLESS windows own no lights to re-seat.
static void refreshChrome(Window *w) {
    NSWindow *nsw = (*w).nsWindow;
    NSButton *buttons[3];
    buttons[LIGHT_CLOSE] = [nsw standardWindowButton:NSWindowCloseButton];
    buttons[LIGHT_MINI] = [nsw standardWindowButton:NSWindowMiniaturizeButton];
    buttons[LIGHT_ZOOM] = [nsw standardWindowButton:NSWindowZoomButton];

    if (!(*w).lightBaseSet) {
        for (int i = 0; i < 3; i++)
            (*w).lightBase[i] = buttons[i] ? [buttons[i] frame] : NSZeroRect;
        (*w).lightBaseSet = true;
    }

    for (int i = 0; i < 3; i++) {
        if (buttons[i] == nil)
            continue;
        [buttons[i] setHidden:(*w).lightVisible[i] ? NO : YES];
        if ((*w).lightVisible[i] && !NSIsEmptyRect((*w).lightBase[i])) {
            NSPoint origin = (*w).lightBase[i].origin;
            origin.x += (CGFloat)(*w).lightOX;
            origin.y += (CGFloat)(*w).lightOY;
            [buttons[i] setFrameOrigin:origin];
        }
    }
}

void Window_setTitle(Window *window, const char *title) {
    if (window == nullptr || title == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setTitle:[NSString stringWithUTF8String:title]];

        // macOS 15+ re-reveals the native title view whenever the title string
        // changes, even when titleVisibility is hidden. Re-apply the hidden
        // state for FullSizeContentView (NAKED) windows, then re-seat the
        // traffic-light chrome so a title change can't resurrect hidden lights.
        if ((styleMaskOf(window) & NSWindowStyleMaskFullSizeContentView) != 0) {
            [(*window).nsWindow setTitlebarAppearsTransparent:YES];
            [(*window).nsWindow setTitleVisibility:NSWindowTitleHidden];
        }
        refreshChrome(window);
    }
}

int Window_width(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed);
}

int Window_height(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed);
}

void Window_setSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setContentSize:NSMakeSize((CGFloat) width, (CGFloat) height)];
    }
}

void Window_setLocation(Window *window, int x, int y) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSRect screen = [[NSScreen mainScreen] frame];
        [(*window).nsWindow setFrameTopLeftPoint:NSMakePoint((CGFloat) x, screen.size.height - (CGFloat) y)];
    }
}

void Window_getLocation(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (int) (*window).cachedX : 0;
    if (outY)
        *outY = window ? (int) (*window).cachedY : 0;
}

void Window_getContentOrigin(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (int) (*window).cachedContentX : 0;
    if (outY)
        *outY = window ? (int) (*window).cachedContentY : 0;
}

void Window_center(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        // Explicit main-screen centering (not [nsWindow center]'s
        // current-screen heuristic): the window lands in the usable area,
        // clear of the menu bar and Dock.
        NSRect avail = [[NSScreen mainScreen] visibleFrame];
        NSRect wf = [(*window).nsWindow frame];
        CGFloat x = avail.origin.x + (avail.size.width - wf.size.width) * 0.5;
        CGFloat y = avail.origin.y + (avail.size.height - wf.size.height) * 0.5;
        [(*window).nsWindow setFrameOrigin:NSMakePoint((CGFloat) floor(x + 0.5), (CGFloat) floor(y + 0.5))];
    }
}

// Claim key synchronously: activation is async, so spin the runloop briefly
// until [NSApp isActive] commits, then open animated + take key while the
// grant is fresh. Falls back to the pump's sPendingKeyWindow re-assertion if
// activation is denied outright.
static void claimKeyAfterActivation(NSWindow *nsw) {
    if (nsw == nil)
        return;
    // Foreground claim: a fresh open (e.g. launched from an IDE/debugger)
    // must take focus instead of landing behind the currently-active app.
    // (The old IgnoringOtherApps flag is deprecated with no effect on
    // macOS 14+; NSApp.activate is its sanctioned replacement.)
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [[NSRunningApplication currentApplication]
            activateWithOptions:NSApplicationActivateAllWindows];
    // Show FIRST so the window appears instantly instead of waiting behind
    // the activation spin below. makeKeyAndOrderFront (not a bare
    // orderFront:) so the open still plays the system animation; the key
    // half is re-asserted after the spin + by the pump fallback.
    [nsw makeKeyAndOrderFront:nil];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:0.5];
    while (![NSApp isActive] &&
           [[NSDate date] compare:deadline] == NSOrderedAscending) {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (e)
            [NSApp sendEvent:e];
        [NSApp updateWindows];
    }
    // Animated open already issued above; now settle the key grant the
    // open requested. Ordering front was guaranteed by that call either
    // way; key follows when the grant allows.
    if (![NSApp isActive])
        NSLog(@"window: activation denied — opening behind the active app; click the Dock icon to take focus");
    if (![nsw isKeyWindow] && [nsw canBecomeKeyWindow])
        [nsw makeKeyWindow];
    sPendingKeyWindow = nsw;
}

void Window_show(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        claimKeyAfterActivation((*window).nsWindow);
        windowRefreshMonitor(window);
    }
}

void Window_hide(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (sPendingKeyWindow == (*window).nsWindow)
            sPendingKeyWindow = nil;
        [(*window).nsWindow orderOut:nil];
    }
}

void Window_setVisible(Window *window, bool visible) {
    if (visible)
        Window_show(window);
    else
        Window_hide(window);
}

bool Window_isResizable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskResizable);
}

void Window_setResizable(Window *window, bool resizable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, resizable ? NSWindowStyleMaskResizable : 0, resizable ? 0 : NSWindowStyleMaskResizable);
}

bool Window_isClosable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskClosable);
}

void Window_setClosable(Window *window, bool closable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, closable ? NSWindowStyleMaskClosable : 0, closable ? 0 : NSWindowStyleMaskClosable);
}

bool Window_isMiniaturizable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskMiniaturizable);
}

void Window_setMiniaturizable(Window *window, bool miniaturizable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, miniaturizable ? NSWindowStyleMaskMiniaturizable : 0, miniaturizable ? 0 : NSWindowStyleMaskMiniaturizable);
}

void Window_setFullscreenButton(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindowCollectionBehavior behavior = [(*window).nsWindow collectionBehavior];
        if (enabled)
            behavior |= NSWindowCollectionBehaviorFullScreenPrimary;
        else
            behavior &= ~NSWindowCollectionBehaviorFullScreenPrimary;
        [(*window).nsWindow setCollectionBehavior:behavior];
    }
}

void Window_setUndecorated(Window *window, int mode) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindowStyleMask mask = styleMaskOf(window);
        if ((mask & NSWindowStyleMaskFullScreen) != 0)
            return;

        NSWindowStyleMask next;
        if (mode == WINDOW_UNDECORATED_BORDERLESS)
            next = 0;
        else if (mode == WINDOW_UNDECORATED_NAKED)
            next = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                 | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                 | NSWindowStyleMaskFullSizeContentView;
        else
            next = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                 | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        [(*window).nsWindow setStyleMask:next];

        // setStyleMask: resets the collectionBehavior, dropping FullScreenPrimary.
        [(*window).nsWindow setCollectionBehavior:([(*window).nsWindow collectionBehavior] | NSWindowCollectionBehaviorFullScreenPrimary)];

        bool transparent = (mode == WINDOW_UNDECORATED_NAKED);
        [(*window).nsWindow setTitlebarAppearsTransparent:transparent];
        [(*window).nsWindow setTitleVisibility:(transparent ? NSWindowTitleHidden : NSWindowTitleVisible)];

        // Chrome can never outrun the blur ban: switching to DECORATED strips
        // any active blur so a frosted titlebar never renders.
        if (mode != WINDOW_UNDECORATED_NAKED && mode != WINDOW_UNDECORATED_BORDERLESS) {
            for (NSView *v in [[(*window).nsWindow contentView] subviews]) {
                if ([v isKindOfClass:[NSVisualEffectView class]]) {
                    [(NSVisualEffectView*) v removeFromSuperview];
                    Window_setTransparent(window, false);
                    break;
                }
            }
        }

        // Mask (and therefore the native light layout) changed: re-snapshot and
        // re-apply per-button visibility + offset against the new layout.
        (*window).lightBaseSet = false;
        refreshChrome(window);
    }
}

void Window_setDecorated(Window *window, bool decorated) {
    Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

bool Window_isDecorated(const Window *window) {
    return windowChromeIsDecorated((Window*) window);
}

void Window_setNaked(Window *window, bool naked) {
    Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

bool Window_isNaked(const Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit((Window*) window, NSWindowStyleMaskFullSizeContentView);
}

void Window_setBorderless(Window *window, bool borderless) {
    Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

bool Window_isBorderless(const Window *window) {
    if (window == nullptr)
        return false;
    return !hasStyleBit((Window*) window, NSWindowStyleMaskTitled);
}

void Window_setFloatingTrafficLights(Window *window, bool floating) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (floating) {
            updateStyleMask(window, NSWindowStyleMaskFullSizeContentView, 0);
            [(*window).nsWindow setTitlebarAppearsTransparent:YES];
            [(*window).nsWindow setTitleVisibility:NSWindowTitleHidden];
        } else {
            updateStyleMask(window, 0, NSWindowStyleMaskFullSizeContentView);
            [(*window).nsWindow setTitlebarAppearsTransparent:NO];
            [(*window).nsWindow setTitleVisibility:NSWindowTitleVisible];
        }
        // Native light layout changed with the mask: re-snapshot, re-seat.
        (*window).lightBaseSet = false;
        refreshChrome(window);
    }
}

static int lightIndex(WindowTrafficLight light) {
    if (light == WINDOW_TRAFFIC_LIGHT_CLOSE)
        return LIGHT_CLOSE;
    if (light == WINDOW_TRAFFIC_LIGHT_MINIMIZE)
        return LIGHT_MINI;
    if (light == WINDOW_TRAFFIC_LIGHT_ZOOM)
        return LIGHT_ZOOM;
    return -1;
}

void Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
    if (window == nullptr)
        return;
    int i = lightIndex(light);
    if (i < 0)
        return;
    @autoreleasepool {
        (*window).lightVisible[i] = visible;
        refreshChrome(window);
    }
}

bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
    int i = lightIndex(light);
    if (window == nullptr || i < 0)
        return false;
    return (*window).lightVisible[i];
}

void Window_macOS_setTrafficLightHeaderPosition(Window *window, float x, float y) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindow *nsw = (*window).nsWindow;
        NSButton *close = [nsw standardWindowButton:NSWindowCloseButton];
        if (close == nil)
            return; // BORDERLESS: no lights to seat.
        if (!(*window).lightBaseSet)
            refreshChrome(window); // snapshot the native layout first (offset 0: no-op)
        NSRect base = (*window).lightBase[LIGHT_CLOSE];
        if (NSIsEmptyRect(base))
            return;
        CGFloat h = [[nsw contentView] bounds].size.height;
        CGFloat wantX = (CGFloat) x;
        CGFloat wantY = h - (CGFloat) y - base.size.height;
        (*window).lightOX = (float) (wantX - base.origin.x);
        (*window).lightOY = (float) (wantY - base.origin.y);
        refreshChrome(window);
    }
}

void Window_macOS_getTrafficLightHeaderPosition(const Window *window, float *outX, float *outY) {
    float x = 0.0f;
    float y = 0.0f;
    if (window != nullptr) {
        @autoreleasepool {
            NSWindow *nsw = (*window).nsWindow;
            NSButton *close = [nsw standardWindowButton:NSWindowCloseButton];
            NSRect base = (*window).lightBase[LIGHT_CLOSE];
            if (close != nil && (*window).lightBaseSet && !NSIsEmptyRect(base)) {
                CGFloat h = [[nsw contentView] bounds].size.height;
                CGFloat curX = base.origin.x + (CGFloat)(*window).lightOX;
                CGFloat curY = base.origin.y + (CGFloat)(*window).lightOY;
                x = (float) curX;
                y = (float) (h - curY - base.size.height);
            }
        }
    }
    if (outX)
        *outX = x;
    if (outY)
        *outY = y;
}

void Window_setOpacity(Window *window, float opacity) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setAlphaValue:(CGFloat) opacity];
    }
}

void Window_setTransparentBackground(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setOpaque:!transparent];
        [(*window).nsWindow setBackgroundColor:(transparent ? [NSColor clearColor] : [NSColor windowBackgroundColor])];
        if ([(*window).nsWindow contentView].layer)
            [(*window).nsWindow contentView].layer.opaque = !transparent;
        Window_setTransparent(window, transparent);
    }
}

void Window_setBlur(Window *window, float blur) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (blur > 0.01f && windowChromeIsDecorated(window)) {
            NSLog(@"window: blur rejected — decorated chrome cannot be blurred (set WINDOW_UNDECORATED_NAKED/BORDERLESS first)");
            return;
        }

        NSWindow *nsw = (*window).nsWindow;
        NSView *contentView = [nsw contentView];

        NSVisualEffectView *blurView = nil;
        for (NSView *v in [contentView subviews]) {
            if ([v isKindOfClass:[NSVisualEffectView class]]) {
                blurView = (NSVisualEffectView*) v;
                break;
            }
        }

        if (blur > 0.01f) {
            if (blurView == nil) {
                blurView = [[NSVisualEffectView alloc] initWithFrame:[contentView bounds]];
                [blurView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
                [blurView setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
                [blurView setMaterial:NSVisualEffectMaterialHUDWindow];
                [blurView setState:NSVisualEffectStateActive];
                [contentView addSubview:blurView positioned:NSWindowBelow relativeTo:nil];
            }
            [blurView setAlphaValue:(CGFloat) blur];
            [nsw setBackgroundColor:[NSColor clearColor]];
            [nsw setOpaque:NO];
            if (contentView.layer)
                contentView.layer.opaque = NO;
            Window_setTransparent(window, true);
        } else if (blurView) {
            [blurView removeFromSuperview];
            Window_setTransparent(window, false);
        }
    }
}

void Window_setAlwaysOnTop(Window *window, bool onTop) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setLevel:(onTop ? NSFloatingWindowLevel : NSNormalWindowLevel)];
    }
}

void Window_setClickThrough(Window *window, bool clickThrough) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setIgnoresMouseEvents:clickThrough];
    }
}

void Window_setShadow(Window *window, bool shadow) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setHasShadow:shadow];
    }
}

void Window_setMovableByBackground(Window *window, bool movable) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setMovableByWindowBackground:movable];
    }
}

void Window_bringToFront(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow orderFrontRegardless];
    }
}

void Window_attachChild(Window *parent, Window *child) {
    if (parent == nullptr || child == nullptr)
        return;
    @autoreleasepool {
        NSWindow *p = (*parent).nsWindow;
        NSWindow *c = (*child).nsWindow;
        if (p == nil || c == nil || p == c)
            return;
        [p addChildWindow:c ordered:NSWindowAbove];
    }
}

void Window_detachChild(Window *parent, Window *child) {
    if (parent == nullptr || child == nullptr)
        return;
    @autoreleasepool {
        NSWindow *p = (*parent).nsWindow;
        NSWindow *c = (*child).nsWindow;
        if (p == nil || c == nil)
            return;
        [p removeChildWindow:c];
    }
}

// --- Minimize ---

void Window_minimize(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow miniaturize:nil];
    }
}

void Window_restore(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow deminiaturize:nil];
    }
}

bool Window_isMinimized(Window *window) {
    if (window == nullptr)
        return false;
    if (atomic_load_explicit(&(*window).miniaturizing, memory_order_relaxed))
        return true;
    @autoreleasepool {
        return [(*window).nsWindow isMiniaturized];
    }
}

// --- Fullscreen ---

bool Window_isFullscreen(Window *window) {
    if (window == nullptr)
        return false;
    @autoreleasepool {
        return (styleMaskOf(window) & NSWindowStyleMaskFullScreen) != 0;
    }
}

void Window_setFullscreen(Window *window, bool fullscreen) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (fullscreen != Window_isFullscreen(window))
            [(*window).nsWindow toggleFullScreen:nil];
    }
}

void Window_toggleFullscreen(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow toggleFullScreen:nil];
    }
}

// --- DRM / sharing ---

void Window_setDRM(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setSharingType:(enabled ? NSWindowSharingNone : NSWindowSharingReadOnly)];
    }
}

// --- Size constraints ---

void Window_setMinSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setContentMinSize:NSMakeSize((CGFloat) width, (CGFloat) height)];
    }
}

void Window_setMaxSize(Window *window, int width, int height) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [(*window).nsWindow setContentMaxSize:NSMakeSize((CGFloat) width, (CGFloat) height)];
    }
}

// --- Cursor control ---

// FPS-style relative cursor: hides the pointer, decouples it from movement,
// and re-warps to the window centre each pump pass while deltas flow into the
// mouse stream as move-delta events.
void Window_setCursorLocked(Window *window, bool locked) {
    if (locked == s_cursorLocked)
        return;
    if (locked) {
        NSWindow *anchor = window ? (*window).nsWindow : sLastWindow;
        NSRect frame = anchor ? [anchor frame] : [[NSScreen mainScreen] frame];
        s_lockCenter = NSMakePoint(frame.origin.x + frame.size.width / 2.0,
                                   frame.origin.y + frame.size.height / 2.0);
        CGAssociateMouseAndMouseCursorPosition(NO);
        CGWarpMouseCursorPosition(s_lockCenter);
        CGDisplayHideCursor(kCGDirectMainDisplay);
        s_cursorLocked = true;
    } else {
        s_cursorLocked = false;
        CGDisplayShowCursor(kCGDirectMainDisplay);
        CGAssociateMouseAndMouseCursorPosition(YES);
    }
}

void Window_setCursorType(Window *window, WindowCursorType type) {
    if (window == nullptr)
        return;
    WindowCursorType prev = (*window).cursorType;
    (*window).cursorType = type;
    @autoreleasepool {
        if (prev == WINDOW_CURSOR_HIDDEN && type != WINDOW_CURSOR_HIDDEN)
            [NSCursor unhide];
        switch (type) {
            case WINDOW_CURSOR_IBEAM:
                [[NSCursor IBeamCursor] set];
                break;
            case WINDOW_CURSOR_POINTING_HAND:
                [[NSCursor pointingHandCursor] set];
                break;
            case WINDOW_CURSOR_CROSSHAIR:
                [[NSCursor crosshairCursor] set];
                break;
            case WINDOW_CURSOR_RESIZE_EW:
                [[NSCursor resizeLeftRightCursor] set];
                break;
            case WINDOW_CURSOR_RESIZE_NS:
                [[NSCursor resizeUpDownCursor] set];
                break;
            case WINDOW_CURSOR_NOT_ALLOWED:
                [[NSCursor operationNotAllowedCursor] set];
                break;
            case WINDOW_CURSOR_HIDDEN:
                [NSCursor hide];
                break;
            case WINDOW_CURSOR_DEFAULT:
            default:
                [[NSCursor arrowCursor] set];
                break;
        }
    }
}

WindowCursorType Window_getCursorType(const Window *window) {
    if (window == nullptr)
        return WINDOW_CURSOR_DEFAULT;
    return (*window).cursorType;
}

// --- Software frame presentation (inert without a render target) ------------

// ;;INTENTION("Window_present on the fresh window is inert (`return false`): a pure AppKit window has no raster target of its own. Software present was GPU-era shim logic; the render repos own pixel paths. Retires with the composite seam.")
bool Window_present(Window *window, const Buffer *frame) {
    (void) window;
    (void) frame;
    return false;
}

// --- Surface anchors (thread 0) ----------------------------------------------

void *Window_contentView(Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return nullptr;
    return (__bridge void*) [(*window).nsWindow contentView];
}

void *Window_nativeHandle(const Window *window) {
    if (window == nullptr || (*window).nsWindow == nil)
        return nullptr;
    return (__bridge void*) (*window).nsWindow;
}

// ;;INTENTION("Window_metalLayer is gone from this file by design (no Metal here). Returns nullptr so the VK_EXT_metal_surface path degrades cleanly until the render repos create their own CAMetalLayer on the content view. Retires with the composite seam.")
void *Window_metalLayer(Window *window) {
    (void) window;
    return nullptr;
}

// ;;INTENTION("Gravity ownership moved out of the window (no layer here). No-op; retires with the composite seam.")
void Window_setGravityTopLeft(Window *window) {
    (void) window;
}

// ;;INTENTION("Worker present transaction was GPU-era (presentsWithTransaction drawables). No layer means nothing to commit; no-op. Retires with the composite seam.")
void Window_workerPresentBegin(void) {
}

void Window_workerPresentEnd(void) {
}

// --- Event wiring (rings are engine-global; adapters attach by window id) ----

void Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (window == nullptr)
        return;
    Key_attachWindow((*window).id, adapter);
}

bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (window == nullptr)
        return false;
    return Key_detachWindow((*window).id, adapter);
}

void Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (window == nullptr)
        return;
    Mouse_attachWindow((*window).id, adapter);
}

bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (window == nullptr)
        return false;
    return Mouse_detachWindow((*window).id, adapter);
}

void Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (window == nullptr)
        return;
    Touch_attachWindow((*window).id, adapter);
}

bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (window == nullptr)
        return false;
    return Touch_detachWindow((*window).id, adapter);
}

void Window_dispatchEvents(Window *window) {
    (void) window;
    Key_dispatchEvents();
    Mouse_dispatchEvents();
    Touch_dispatchEvents();
}

// --- Focus (the spotlight: one focused window per machine) -------------------

uint32_t Window_id(Window *window) {
    return window ? (*window).id : FOCUS_BROADCAST;
}

void Window_focus(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        claimKeyAfterActivation((*window).nsWindow);
        Focus_set((*window).id);
    }
}

bool Window_isFocused(Window *window) {
    return window && Focus_isFocused((*window).id);
}

void Window_setKeyEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(&(*window).keyEnabled, enabled, memory_order_relaxed);
}

bool Window_isKeyEnabled(const Window *window) {
    if (window == nullptr)
        return false;
    return atomic_load_explicit(&(*window).keyEnabled, memory_order_relaxed);
}

// --- Lifecycle registry ------------------------------------------------------
// The ONLY outside path to the embedded WindowEvent: fill slots with
// WindowEvent_setOn*(Window_getLifecycle(w), fn). Null-safe.

WindowEvent *Window_getLifecycle(Window *window) {
    if (window == nullptr)
        return nullptr;
    return &(*window).lifecycle;
}

// --- Monitor identity ---------------------------------------------------------

uint32_t Window_getMonitorId(const Window *window) {
    return window ? atomic_load_explicit(&(*window).monitorId, memory_order_acquire) : 0;
}

// --- Resize reflection ---------------------------------------------------------

uint64_t Window_sizeGeneration(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(&(*window).sizeGeneration, memory_order_acquire);
}

void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata) {
    if (window == nullptr)
        return;
    (*window).resizeRenderFn = fn;
    (*window).resizeRenderUserdata = userdata;
}