// window_cocoa.m — the AppKit shim (the ".m" glue file).
//
// vex's core is pure C11; this is the ONE Objective-C file in the project.
// It exists only to talk to AppKit, because NSWindow/NSApplication are ObjC
// objects and there is no pure-C way to create them. Everything above this
// boundary stays C; everything here is "dip into the OS, hand back a handle".
//
// Design notes:
//   - sAppDelegate is created once per process (the app lifecycle delegate).
//   - Each window gets its own WindowDelegate so we can learn about the
//     user clicking the red close button -> sets shouldClose -> engine loop
//     sees it and exits (see window_demo.c).
//   - setReleasedWhenClosed:NO is CRITICAL. The default (YES for programmatic
//     windows) makes NSWindow free itself the moment it closes; our destroy()
//     would then call close on freed memory = the segfault you saw. We own the
//     window object; only destroy() releases it.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <stdatomic.h>

#include "buffer/color_buffer.h"
#include "window/window.h"
// NOTE: no darling/panel.h here on purpose — it transitively typedefs
// `Collection`, which collides with CarbonCore under AppKit imports. This
// file only stores Panel pointers (opaque slots), never dereferences them;
// window.h's forward typedef is all it needs.

#include "input/focus.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "time/nanotime.h"
#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
  * CLASS: Window (objc/window_cocoa.m)
  * LEVEL: L4 — Self-Management (AppKit OS window shim owned by the OS)
  * ============================================================================
  * the AppKit shim (the ".m" glue file).
  *
  * STRUCT FIELDS (local to this file — exactly this file's class):
  * ----------------------------------------------------------------------------
  *   NSWindow *nsWindow;                      // AppKit window (we own it)
  *   WindowDelegate *delegate;            // per-window close/focus delegate
  *   bool shouldClose;                        // true once close requested
  *   uint32_t id;                             // engine window id (1..7, 0 = broadcast)
  *   _Atomic uint64_t sizeGeneration;         // resize-reflection counter (thread 0 bumps)
  *   _Atomic int cachedWidth;                 // content width at last thread-0 pump
  *   _Atomic int cachedHeight;                // content height at last thread-0 pump
  *   double cachedX;                          // top-left screen X at last pump
  *   double cachedY;                          // top-left screen Y at last pump
  *   double cachedContentX;                   // content top-left X (below title bar)
  *   double cachedContentY;                   // content top-left Y (below title bar)
  *   _Atomic int presentMode;                 // present pacing (FIFO/IMMEDIATE)
  *   _Atomic bool transparent;                // composite transparency request
  *   _Atomic uint64_t renderGeneration;       // policy-reflection counter (swapchain rebuild)
  *   _Atomic(Panel*) container;               // content root (nullptr = clear-only pass)
  *   _Atomic(Panel*) contentPanel;            // UI tree (board or child panes when native)
  *   _Atomic(Panel*) scenePanel;              // scene tree (Vulkan-backed)
  *   _Atomic bool enabled;                    // false mutes ALL OS input
  *   bool lastFocused;                        // focus-flip detection during pump
  *   _Atomic uint32_t monitorId;              // CGDirectDisplayID mirror (0 = unmapped)
  *   const WindowEvent *windowAdapters[WINDOW_ADAPTER_MAX]; // lifecycle listeners (max 16)
  *   int windowAdapterCount;                  // used slots in windowAdapters[]
  *   WindowResizeRenderFn resizeRenderFn;     // resize-cadence render hook
  *   void *resizeRenderUserdata;              // hook userdata
   *   WindowCursorType cursorType;             // active OS cursor style
   *
   * PRIVATE HELPERS (file-local ObjC, no external API):
   * ----------------------------------------------------------------------------
  *   VulkanView : NSView — Vulkan backing layer + live-resize / zoom bridge
  *     BOOL _liveResizing;        // true during NSViewLiveResize (thread 0)
  *     BOOL _zooming;             // true during instant-zoom bridge (thread 0)
  *     NSSize _pendingSize;       // stashed size during drag/zoom, consumed at settle
  *   WindowDelegate : NSObject <NSWindowDelegate> — per-window close/focus/resize delegate
*   settleAfterResize — idempotent: clears live-resizing, reasserts TopLeft
   *     gravity, applies _pendingSize once, exactly one rebuild/re-render;
   *     repeat calls with no live state return early
   *
   * Zoom bridge contract: double-click titlebar → animationResizeTime returns 0.0
   * (instant), sets _zooming + kCAGravityResize; setFrameSize freezes drawableSize;
   * windowDidResize clears _zooming, restores kCAGravityTopLeft, applies true size
   * in one shot — no intermediate rebuild, no TopLeft crop smear.
   *
  * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Window_0(void)
 *   - Window_1(title)
 *   - Window_new(desc)
 *   - Window_create(title, width, height)
 *
  * Core Functions:
  *   - windowIdAcquire(window, handle)
  *   - windowIdRelease(id)
  *   - windowIdOf(window)
  *   - windowHandleOf(window)
  *   - windowFireClose(window)
  *   - applyLayerGravity(window)
  *   - findVulkanView(window)                 : resolve VulkanView from contentView subviews
  *   - Window_compositePanes(w, contentPanel)
  *   - Window_compositeBoards(w)                : scene/content Metal boards
  *   - windowFireFocus(window, focused)
  *   - windowFireResized(window, width, height)
  *   - windowFireMoved(window, x, y)
  *   - windowFireMonitorChanged(window, oldId, newId)
  *   - resolveMonitorId(window)
  *   - refreshMonitorId(window)
  *   - recenterIfLocked(void)
  *   - mouseLocation(event, outX, outY)
  *   - touchAction(phase)
  *   - dispatchTouches(event, wid)
  *   - routeEvent(event)
  *   - Window_pollEvents(void)
  *   - windowAlloc(desc)
  *   - descResolve(desc)
  *   - Window_center(w)
  *   - Window_show(w)
  *   - Window_destroy(window)
  *   - Window_shouldClose(window)
  *   - Window_renderGeneration(window)
  *   - Window_attachPanes(window, contentPanel, w, h)
  *   - Window_resizePanes(window, panel, width, height)
  *   - styleMaskOf(window)
  *   - updateStyleMask(window, add, clear)
  *   - Window_width(window)
  *   - Window_height(window)
  *   - Window_hide(window)
  *   - Window_minimize(window)
  *   - Window_restore(window)
  *   - Window_toggleFullscreen(window)
  *   - Window_addKeyAdapter(window, adapter)
 *   - Window_removeKeyAdapter(window, adapter)
 *   - Window_addMouseAdapter(window, adapter)
 *   - Window_removeMouseAdapter(window, adapter)
 *   - Window_addTouchAdapter(window, adapter)
 *   - Window_removeTouchAdapter(window, adapter)
 *   - Window_addWindowAdapter(window, adapter)
 *   - Window_removeWindowAdapter(window, adapter)
 *   - Window_dispatchEvents(window)
 *   - Window_id(window)
 *   - Window_focus(window)
 *   - Window_bringToFront(window)
 *   - Window_sizeGeneration(window)
 *   - Window_present(window, frame)
 *   - Window_contentView(window)
 *   - Window_metalLayer(window)
  *   - Window_workerPresentBegin(void) / Window_workerPresentEnd(void)
  *     : explicit CATransaction per worker board+pane walk (Rule 11
  *     pane-of-glass: worker owns no runloop); layers stay
  *     presentsWithTransaction=YES from makeBackingLayer through drag —
  *     gravity alone flips (Resize mid-drag, TopLeft at settle).
  *     Single-arm: exactly one walk armed at a time (nested Begin drops,
  *     End no-ops when disarmed); End logs the commit identity in debug
  *     only, release stays log-free (Rule 35 hot-minimal)
 *
 * Setters:
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)
 *   - Window_setContainer(window, root)
 *   - Window_setContentPanel(window, panel)
 *   - Window_setScenePanel(window, panel)
 *   - Window_setEnabled(window, enabled)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setVisible(window, visible)
 *   - Window_setResizable(window, resizable)
 *   - Window_setClosable(window, closable)
 *   - Window_setMiniaturizable(window, miniaturizable)
 *   - Window_setFullscreenButton(window, enabled)
 *   - Window_setUndecorated(window, mode)
 *   - Window_setFloatingTrafficLights(window, floating)
 *   - Window_setOpacity(window, opacity)
 *   - Window_setTransparentBackground(window, transparent)
 *   - Window_setBlur(window, blur)
 *   - Window_setAlwaysOnTop(window, onTop)
 *   - Window_setClickThrough(window, clickThrough)
 *   - Window_setShadow(window, shadow)
 *   - Window_setMovableByBackground(window, movable)
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)
 *   - Window_setMinSize(window, width, height)
 *   - Window_setMaxSize(window, width, height)
 *   - Window_setCursorLocked(window, locked)
 *   - Window_setCursorType(window, type)
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setGravityTopLeft(window)
 *
 * Getters:
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_getContainer(window)
 *   - Window_getContentPanel(window)
 *   - Window_getScenePanel(window)
  *   - Window_getPanelLayer(window, panel)
  *   - Window_isEnabled(window)
 *   - hasStyleBit(window, bit)
 *   - Window_getLocation(window, outX, outY)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_isResizable(window)
 *   - Window_isClosable(window)
 *   - Window_isMiniaturizable(window)
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *   - Window_isFocused(window)
  *   - Window_getMonitorId(window)
  *   - Window_getCursorType(window)
  * ============================================================================
  */


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

// Cursor-lock state (Legacy: macOSWindow.lockWindowPtr/recenterIfLocked).
// While locked the pointer is decoupled from motion and re-warped to the
// window centre every pump pass; NSEvent deltas feed input.Mouse directly.
static bool s_cursorLocked = false;
static CGPoint s_lockCenter = {0, 0};

// Window-lifecycle adapter registry capacity. Attach/dispatch are thread-0
// operations, so the registry itself needs no synchronization.
#define WINDOW_ADAPTER_MAX 16

@class WindowDelegate;

// One opaque handle handed back to C. Holds both NS objects we must keep
// alive: the window itself and its delegate. `id` is the engine's small
// window number (1..7; 0 is the broadcast reserved id) used to tag input
// events and route them to per-window listeners. sizeGeneration is the
// resize-reflection counter: Thread 0 bumps it when the content rect moves.
// renderGeneration is the same reflection trick for present POLICY: bumped
// by the setters whenever the swapchain itself must be rebuilt
// (presentMode / transparent). The container slot is polled fresh every
// frame and never bumps it — its panel carries the clear color too.
struct Window {
    NSWindow *nsWindow;
    WindowDelegate *delegate;
    bool shouldClose;
    uint32_t id;
    _Atomic uint64_t sizeGeneration;
    _Atomic int cachedWidth; // content width at last thread-0 pump (worker reads)
    _Atomic int cachedHeight; // content height at last thread-0 pump (worker reads)
    double cachedX;          // top-left screen coords at last pump (move reflection)
    double cachedY;
    double cachedContentX;   // CONTENT top-left (below title bar), same space
    double cachedContentY;

    // --- present policy (renderer-facing atomic words) ---
    _Atomic int presentMode;
    _Atomic bool transparent;
    _Atomic uint64_t renderGeneration;
    _Atomic bool liveResizing; // thread 0 during NSViewLiveResize; renderer consumes

    // --- content root: nullptr => clear-only pass --
    _Atomic(Panel*) container;
    _Atomic(Panel*) contentPanel;  // UI tree (board-backed or child panes)
    _Atomic(Panel*) scenePanel;    // Scene tree (Vulkan-backed)

    // --- runtime state --
    _Atomic bool enabled;    // false mutes ALL OS input for this window
    bool lastFocused;        // focus-flip detection during the pump
    _Atomic uint32_t monitorId; // CGDirectDisplayID mirror; 0 = unmapped

    // --- window-lifecycle adapters (thread 0 only) ---
    const WindowEvent *windowAdapters[WINDOW_ADAPTER_MAX];
    int windowAdapterCount;

    // --- resize-cadence render bridge (c -> objc -> c) ---
    WindowResizeRenderFn resizeRenderFn;
    void *resizeRenderUserdata;

    // --- cursor styling ---
    WindowCursorType cursorType;
};

// Window id registry: slot i holds the NSWindow currently owning id i (and
// the C handle that owns that NSWindow). Ids are scarce on purpose (8 total)
// — an engine does not open hundreds of windows. Slot 0 stays empty forever
// (FOCUS_BROADCAST).
#define WINDOW_ID_SLOTS 8
static NSWindow *s_idToWindow[WINDOW_ID_SLOTS] = { nil };
static Window *s_idToHandle[WINDOW_ID_SLOTS] = { nullptr };

// Resolve an id for a newly created window, or 0 when the table is full.
static uint32_t windowIdAcquire(NSWindow *window, Window *handle) {
    for (uint32_t i = 1; i < WINDOW_ID_SLOTS; i++) {
        if (s_idToWindow[i] == nil) {
            s_idToWindow[i] = window;
            s_idToHandle[i] = handle;
            return i;
        }
    }
    return FOCUS_BROADCAST;
}

static void windowIdRelease(uint32_t id) {
    if (id != FOCUS_BROADCAST && id < WINDOW_ID_SLOTS) {
        s_idToWindow[id] = nil;
        s_idToHandle[id] = nullptr;
    }
}

// Reverse lookup: which engine id does this OS window carry? 0 if unknown.
static uint32_t windowIdOf(NSWindow *window) {
    if (!window) return FOCUS_BROADCAST;
    for (uint32_t i = 1; i < WINDOW_ID_SLOTS; i++)
        if (s_idToWindow[i] == window) return i;
    return FOCUS_BROADCAST;
}

static Window *windowHandleOf(NSWindow *window) {
    if (!window) return nullptr;
    for (uint32_t i = 1; i < WINDOW_ID_SLOTS; i++)
        if (s_idToWindow[i] == window) return s_idToHandle[i];
    return nullptr;
}

// Flipped content view so all sublayers and Cocoa geometry natively speak
// TOP-LEFT coordinates (matching darling Container_resolve layout 1:1).
@interface WindowContentView : NSView
@end

@implementation WindowContentView
- (BOOL)isFlipped {
    return YES;
}
@end

// App-level delegate: receives lifecycle events for the whole application.
// applicationShouldTerminateAfterLastWindowClosed lets the process end when
// the last window goes away (normal for a game/engine run).
@interface WindowAppDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, assign) bool *shouldClosePtr;
@end

@implementation WindowAppDelegate
- (void)applicationWillTerminate:(NSNotification*) notification {
    (void) notification;
    if (self.shouldClosePtr) *self.shouldClosePtr = true;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*) sender {
    (void) sender;
    return YES;
}
@end

// Window-level delegate: this is how we learn the user clicked the close
// button. windowWillClose fires as the window is being torn down; we flip the
// bool the engine loop polls, then hand onCloseRequested to every attached
// window adapter. The pointers are (assign) because the delegate must not
// own our C struct.
@interface WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) bool *shouldClosePtr;
@property(nonatomic, assign) Window *handlePtr;
@end

static void windowFireClose(Window *window);
static void applyLayerGravity(Window *window);

// VulkanView: Vulkan backing layer + live-resize / zoom bridge.
// Full interface forward-declared here so the delegate can access zoom
// accessors before the @implementation below.
@interface VulkanView : NSView {
@public
    BOOL _liveResizing;
    BOOL _zooming; // instant-zoom bridge: true between animationResizeTime + windowDidResize
    BOOL _fullScreenTransitioning; // true during native macOS fullscreen transitions
    NSSize _pendingSize;
}
- (BOOL)isZooming;
- (void)setZooming:(BOOL)zooming;
- (BOOL)isFullScreenTransitioning;
- (void)setFullScreenTransitioning:(BOOL)transitioning;
- (void)settleAfterResize;
@end

static VulkanView *findVulkanView(NSWindow *window);

@implementation WindowDelegate
- (void) windowWillClose:(NSNotification*) notification {
    (void) notification;
    if (self.shouldClosePtr) *self.shouldClosePtr = true;
    if (self.handlePtr) windowFireClose(self.handlePtr);
}

// Fullscreen transition lifecycle hooks:
// When entering or exiting native macOS fullscreen (green orb or toggleFullScreen:),
// macOS performs a desktop Spaces slide animation lasting 1.5 - 2.0 seconds.
// Prematurely settling in viewDidEndLiveResize (which Cocoa fires at ~11ms on initial frame layout)
// causes swapchain rebuild churn and locks CAMetalLayer presents against frozen Core Animation
// transactions. By maintaining liveResizing = true with presentsWithTransaction = YES
// throughout the Spaces slide, the background worker keeps presenting inside
// its own explicit per-walk CATransaction (Window_workerPresentBegin/End —
// worker threads own no runloop to commit an implicit one), and the true
// settle executes upon windowDidEnterFullScreen / windowDidExitFullScreen.

- (void)windowWillEnterFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
    }
    NSWindow *window = [notification object];
    VulkanView *vulkanView = findVulkanView(window);
    if (vulkanView) {
        [vulkanView setFullScreenTransitioning:YES];
        if ([[vulkanView layer] isKindOfClass:[CAMetalLayer class]]) {
            // YES persists from makeBackingLayer through the transition: the
            // worker commits its own explicit transaction per present walk.
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [(CAMetalLayer*) [vulkanView layer] setContentsGravity:kCAGravityResize];
            [CATransaction commit];
        }
    }
}

- (void)windowDidEnterFullScreen:(NSNotification*) notification {
    (void) notification;
    NSWindow *window = [notification object];
    VulkanView *vulkanView = findVulkanView(window);
    if (vulkanView) {
        [vulkanView setFullScreenTransitioning:NO];
        [vulkanView settleAfterResize];
    }
}

- (void)windowDidFailToEnterFullScreen:(NSWindow*) window {
    VulkanView *vulkanView = findVulkanView(window);
    if (vulkanView) {
        [vulkanView setFullScreenTransitioning:NO];
        [vulkanView settleAfterResize];
    }
}

- (void)windowWillExitFullScreen:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);
    }
    NSWindow *window = [notification object];
    VulkanView *vulkanView = findVulkanView(window);
    if (vulkanView) {
        [vulkanView setFullScreenTransitioning:YES];
        if ([[vulkanView layer] isKindOfClass:[CAMetalLayer class]]) {
            // YES persists from makeBackingLayer through the transition: the
            // worker commits its own explicit transaction per present walk.
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [(CAMetalLayer*) [vulkanView layer] setContentsGravity:kCAGravityResize];
            [CATransaction commit];
        }
    }
}

- (void)windowDidExitFullScreen:(NSNotification*) notification {
    (void) notification;
    NSWindow *window = [notification object];
    VulkanView *vulkanView = findVulkanView(window);
    if (vulkanView) {
        [vulkanView setFullScreenTransitioning:NO];
        [vulkanView settleAfterResize];
    }
}

- (void)windowDidFailToExitFullScreen:(NSWindow*) window {
    VulkanView *vulkanView = findVulkanView(window);
    if (vulkanView) {
        [vulkanView setFullScreenTransitioning:NO];
        [vulkanView settleAfterResize];
    }
}

// Kill every animated frame change for this window. The OS's zoom animation
// (double-click title bar) never renders in-process — WindowServer just scales
// the window's stale committed bitmap between the two sizes, which reads as
// smear/stretch no matter what the renderer does. Returning zero makes
// setFrame:display:animate:YES land instantly: ONE real resize that the
// TopLeft gravity law + resize-cadence bridge present honestly.
//
// Zoom bridge: set _zooming + Resize gravity so setFrameSize: freezes
// drawableSize (no intermediate scales), then windowDidResize restores
// TopLeft and applies the true final size in one shot.
- (NSTimeInterval)window:(NSWindow*) window animationResizeTime:(NSRect)newFrame {
    (void) newFrame;
    (void) window;
    return 0.0;
}

// Resize-cadence choke points, both thread 0, both feeding the SAME render
// hook the pump uses:
//   willResize fires BEFORE the proposed size applies — the hook drains the
//   runway (fence retire / final old-size present), so the frozen drawable
//   CA shows during the border step is fresh and exact-sized (TopLeft crops
//   it, never stretches).
//   didResize fires AFTER each applied step — extents have moved, so the
//   blocking sync present lands the new size's frame inside the same runloop
//   turn the border moved.
// Accept-always policy: return frameSize unchanged. Pacing lives in the
// renderer's rebuild gate, not here.
- (NSSize)windowWillResize:(NSWindow*) sender toSize:(NSSize)frameSize {
    (void) sender;
    Window *w = self.handlePtr;
    if (!w)
        return frameSize;
    NSRect content = [(*w).nsWindow contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)];
    atomic_store_explicit(&(*w).cachedWidth, (int)content.size.width, memory_order_relaxed);
    atomic_store_explicit(&(*w).cachedHeight, (int)content.size.height, memory_order_relaxed);
    return frameSize;
}

- (void)windowDidResize:(NSNotification*) notification {
    (void) notification;
    Window *w = self.handlePtr;
    if (!w) return;

    NSRect content = [(*w).nsWindow contentRectForFrameRect:[(*w).nsWindow frame]];
    atomic_store_explicit(&(*w).cachedWidth, (int)content.size.width, memory_order_relaxed);
    atomic_store_explicit(&(*w).cachedHeight, (int)content.size.height, memory_order_relaxed);

    VulkanView *vulkanView = findVulkanView((*w).nsWindow);
    // LIVE RESIZE / FULLSCREEN GATE: during an active mouse drag or fullscreen space transition,
    // intermediate steps are handled by setFrameSize. viewDidEndLiveResize owns the final drag settle.
    if ([(*w).nsWindow inLiveResize] || (vulkanView && (vulkanView->_liveResizing || vulkanView->_fullScreenTransitioning))) {
        return;
    }

    if (vulkanView) {
        [vulkanView settleAfterResize];
    }

    // Programmatic / zoom resize settle pass
    if (vulkanView && [[vulkanView layer] isKindOfClass:[CAMetalLayer class]]) {
        CGFloat scale = [(*w).nsWindow backingScaleFactor];
        if (scale <= 0.0)
            scale = 1.0;
        int pxW = (int)(content.size.width * scale + 0.5);
        int pxH = (int)(content.size.height * scale + 0.5);
        if (pxW > 0 && pxH > 0) {
            CAMetalLayer *metal = (CAMetalLayer*) [vulkanView layer];
            metal.drawableSize = CGSizeMake((CGFloat) pxW, (CGFloat) pxH);
        }
    }
    Panel *contentPanel = atomic_load_explicit(&(*w).contentPanel, memory_order_acquire);
    if (contentPanel) {
        extern void Darling_setPanelSize(Panel *p, float w, float h);
        Darling_setPanelSize(contentPanel, (float)content.size.width, (float)content.size.height);
        Window_compositePanes(w, contentPanel);
        extern void Window_compositeBoards(Window *window);
        Window_compositeBoards(w);
    }
    if ((*w).resizeRenderFn)
        (*w).resizeRenderFn((*w).resizeRenderUserdata);
}
@end

static WindowAppDelegate *sAppDelegate = nil; // one app delegate for the whole process
static NSWindow *sLastWindow = nil;
// Key-window claim pending: set by Window_show/Window_focus, drained by the
// pump (Window_pollEvents) once the app is active and the claim sticks.
static NSWindow *sPendingKeyWindow = nil;

// Worker present single-arm (Rule 35 hot-minimal): exactly one board+pane
// walk transaction armed at a time. Begin is idempotent (a nested arm drops);
// End commits only when armed, then logs the commit identity in debug only
// (release stays log-free — the flag flip is the whole release cost).
static bool s_workerPresentArmed = false;

// Window-lifecycle adapter fan-out. All fire from thread 0 only.
static void windowFireClose(Window *window) {
    if (!window)
        return;
    for (int i = 0; i < (*window).windowAdapterCount; i++) {
        const WindowEvent *a = (*window).windowAdapters[i];
        if ((*a).onCloseRequested)
            (*a).onCloseRequested((*a).self, window);
    }
}

static void windowFireFocus(Window *window, bool focused) {
    if (!window)
        return;
    for (int i = 0; i < (*window).windowAdapterCount; i++) {
        const WindowEvent *a = (*window).windowAdapters[i];
        if ((*a).onFocusChanged)
            (*a).onFocusChanged((*a).self, window, focused);
    }
}

static void windowFireResized(Window *window, int width, int height) {
    if (!window)
        return;
    for (int i = 0; i < (*window).windowAdapterCount; i++) {
        const WindowEvent *a = (*window).windowAdapters[i];
        if ((*a).onResized)
            (*a).onResized((*a).self, window, width, height);
    }
}

static void windowFireMoved(Window *window, int x, int y) {
    if (!window)
        return;
    for (int i = 0; i < (*window).windowAdapterCount; i++) {
        const WindowEvent *a = (*window).windowAdapters[i];
        if ((*a).onMoved)
            (*a).onMoved((*a).self, window, x, y);
    }
}

static void windowFireMonitorChanged(Window *window, uint32_t oldId, uint32_t newId) {
    if (!window)
        return;
    for (int i = 0; i < (*window).windowAdapterCount; i++) {
        const WindowEvent *a = (*window).windowAdapters[i];
        if ((*a).onMonitorChanged)
            (*a).onMonitorChanged((*a).self, window, oldId, newId);
    }
}

// Resolve the CGDirectDisplayID of the screen carrying the greatest share of
// this window. Returns 0 when no screen qualifies (headless, fully off-screen
// with no intersection, or screens list empty).
static uint32_t resolveMonitorId(Window *window) {
    if (!window)
        return 0;
    @autoreleasepool {
        NSScreen *screen = [(*window).nsWindow screen];
        if (!screen)
            return 0;
        NSNumber *displayId = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        return displayId ? (uint32_t)[displayId unsignedIntValue] : 0;
    }
}

// Mirror the OS's monitor assignment into the atomic word; fires adapters on
// flip. Thread 0 only.
static void refreshMonitorId(Window *window) {
    if (!window)
        return;
    uint32_t next = resolveMonitorId(window);
    uint32_t prev = atomic_exchange_explicit(&(*window).monitorId, next,
                                             memory_order_acq_rel);
    if (prev != next)
        windowFireMonitorChanged(window, prev, next);
}

// Re-centre the cursor during the pump while locked, so the warp registers
// before the next event loop exit (legacy recenterIfLocked).
static void recenterIfLocked(void) {
    if (!s_cursorLocked) return;
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
        y = content.size.height - y; // flip to top-left origin
    }
    *outX = x;
    *outY = y;
}

// Map an NSTouch phase onto the Touch_* action codes.
static int touchAction(NSTouchPhase phase) {
    if (phase & NSTouchPhaseBegan) return TOUCH_DOWN;
    if (phase & (NSTouchPhaseMoved | NSTouchPhaseStationary)) return TOUCH_MOVE;
    if (phase & NSTouchPhaseEnded) return TOUCH_UP;
    return TOUCH_CANCEL;
}

// Feed one gesture/touch carrier event into input.Touch: resolve each active
// touch into its slot (identity hash % TOUCH_MAX), normalize position into
// content coords, and estimate pressure from the resting flag (legacy parity).
static void dispatchTouches(NSEvent *event, uint32_t wid) {
    NSWindow *eventWindow = [event window];
    if (!eventWindow) return;
    NSView *contentView = [eventWindow contentView];
    if (!contentView) return;

    NSSet *touches = [event touchesMatchingPhase:NSTouchPhaseAny inView:contentView];
    if (!touches || touches.count == 0) return;

    NSRect frame = [contentView frame];
    double winW = frame.size.width;
    double winH = frame.size.height;

    for (NSTouch *touch in touches) {
        NSUInteger slot = [[touch identity] hash] % TOUCH_MAX;
        NSPoint norm = [touch normalizedPosition];
        double posX = norm.x * winW;
        double posY = (1.0 - norm.y) * winH;
        double pressure = [touch isResting] ? 0.2 : 0.8;
        Touch_pushTouchEvent(wid, (int)slot, touchAction([touch phase]),
                             posX, posY, pressure, kTapThresholdNanos);
    }
}

// Intercept-and-forward: read everything the engine cares about off each OS
// event, push it into the input modules, then hand the event back to AppKit
// so the responder chain keeps working. This is the Thread-0 producer side of
// the input pipeline.
static void routeEvent(NSEvent *event) {
    NSEventType type = [event type];
    // Which engine window did the OS deliver this to? Tagged on every queued
    // event so dispatch routes it to that window's listeners only.
    uint32_t wid = windowIdOf([event window]);

    // Disabled windows receive nothing: events never enter the device rings,
    // so adapters stay silent and polling state freezes. AppKit still gets
    // the event via sendEvent — only OUR input pipeline is muted.
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
                        unsigned char c0 = (unsigned char)[chars characterAtIndex:0];
                        if (c0 > 0) Key_pushCharEvent(wid, c0);
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
                                                              : (int)[event buttonNumber]);
            // Refresh tracked cursor position BEFORE button event so Mouse_x()/y() are current in callbacks.
            double x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushButtonEvent(wid, button, KEY_ACTION_DOWN, kTapThresholdNanos);
            break;
        }

        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseUp: {
            int button = (type == NSEventTypeLeftMouseUp) ? MOUSE_LEFT
                       : ((type == NSEventTypeRightMouseUp) ? MOUSE_RIGHT
                                                             : (int)[event buttonNumber]);
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
                // Locked: AppKit reports a constant anchor with real deltas.
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
                                                                      : (int)[event buttonNumber]);
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
void Window_pollEvents(void) {
    // Per-tick transaction: the pump is a manual event drain, never a real
    // runloop turn — so without this, main-thread CoreAnimation transactions
    // may never commit at idle, holding every presentsWithTransaction=YES
    // drawable hostage (motion-during-resize, freeze-at-idle: the drag's
    // modal loop commits continuously, the idle pump does not). One explicit
    // commit per tick releases worker + board presents on frame cadence and
    // batches this pass's layer mutations into the same vsync. Empty at
    // idle = negligible cost. Never runs during a drag (the modal tracking
    // loop owns thread 0 then), so the live-resize NO-contract is untouched.
    [CATransaction begin];
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
        // Drain a pending key claim: if activation was denied outright the
        // synchronous claim in claimKeyAfterActivation was a no-op. Once the
        // app is active, re-assert until AppKit grants key; the claim then
        // sticks with no hover/refocus needed.
        if (sPendingKeyWindow) {
            if ([NSApp keyWindow] == sPendingKeyWindow) {
                sPendingKeyWindow = nil;
            } else if ([NSApp isActive] && [sPendingKeyWindow canBecomeKeyWindow]) {
                [sPendingKeyWindow makeKeyWindow];
                if ([NSApp keyWindow] == sPendingKeyWindow)
                    sPendingKeyWindow = nil;
            }
        }

        // Resize + move reflection: compare the live frame against the cache
        // and bump the generation / fire adapters only on an actual change,
        // so a renderer polling once per frame pays one int compare.
        for (uint32_t i = 1; i < WINDOW_ID_SLOTS; i++) {
            NSWindow *w = s_idToWindow[i];
            if (!w) continue;
            Window *handle = windowHandleOf(w);
            if (!handle) continue;
            NSRect content = [w contentRectForFrameRect:[w frame]];
            int cw = (int)content.size.width;
            int ch = (int)content.size.height;
            bool rectChanged = false;
            if (cw != atomic_load_explicit(&(*handle).cachedWidth, memory_order_relaxed)
                || ch != atomic_load_explicit(&(*handle).cachedHeight, memory_order_relaxed)) {
                atomic_store_explicit(&(*handle).cachedWidth, cw, memory_order_relaxed);
                atomic_store_explicit(&(*handle).cachedHeight, ch, memory_order_relaxed);
                atomic_fetch_add_explicit(&(*handle).sizeGeneration, 1, memory_order_release);
                windowFireResized(handle, cw, ch);
                rectChanged = true;
            }

            // Move reflection: top-left screen coords, same space setLocation
            // speaks (AppKit origin is bottom-left; convert on the way out).
            NSRect frame = [w frame];
            CGFloat screenHeight = [[NSScreen mainScreen] frame].size.height;
            double tx = (double)frame.origin.x;
            double ty = (double)(screenHeight - frame.origin.y - frame.size.height);
            if (tx != (*handle).cachedX || ty != (*handle).cachedY) {
                (*handle).cachedX = tx;
                (*handle).cachedY = ty;
                windowFireMoved(handle, (int)tx, (int)ty);
                rectChanged = true;
            }

            // Content origin: chrome-aware twin of the frame cache — the
            // collage joins against this, not the title-bar-included frame.
            double cx = (double)frame.origin.x;
            double cy = (double)(screenHeight - content.origin.y - content.size.height);
            (*handle).cachedContentX = cx;
            (*handle).cachedContentY = cy;

            // Focus flip: mirror the OS spotlight into per-window adapters.
            bool focused = (windowIdOf([NSApp keyWindow]) == (*handle).id);
            if (focused != (*handle).lastFocused) {
                (*handle).lastFocused = focused;
                windowFireFocus(handle, focused);
            }

            // Monitor mirror: resolve the carrying display, flip the atomic,
            // fire adapters only when the window changed screens.
            refreshMonitorId(handle);

            // Gravity contract: TopLeft is STEADY-STATE POLICY, asserted
            // every pass on thread 0 before any present can sample the
            // layer. During live resize (Rule 11.6) the drag already pins
            // TopLeft (freeze-exact); the gate skips the redundant
            // reassert mid-drag so the tracking loop stays clean.
            if (!Window_isLiveResizing(handle))
                applyLayerGravity(handle);

            // Resize-cadence bridge: geometry moved this pass -> hand thread
            // 0's fresh caches straight to the compositor renderer. Runs
            // INSIDE AppKit's event servicing, at the OS's own rhythm.
            // Live-gated (Rule 11.6): mid-drag the settle pass owns the one
            // rebuild — per-step GPU work here would stall edge tracking.
            if (rectChanged && (*handle).resizeRenderFn && !Window_isLiveResizing(handle))
                (*(*handle).resizeRenderFn)((*handle).resizeRenderUserdata);

            // Child panes: attach/position child CALayers on the content view
            Panel *contentPanel = atomic_load_explicit(&(*handle).contentPanel, memory_order_acquire);
            if (contentPanel) {
                Window_compositePanes(handle, contentPanel);
            }

            // Discriminator probe: who is stretching? Log what the layer
            // ACTUALLY carries while the window is being abused. If gravity
            // ever reads anything but topLeft, someone replaced our layer.
            static int s_probeInit = 0;
            static bool s_probe = false;
            static uint64_t s_probeTick = 0;
            if (!s_probeInit) {
                s_probeInit = 1;
                s_probe = getenv("ANTI_VK_TRACE") != nullptr;
            }
            if (s_probe && (++s_probeTick % 30 == 0)) {
                @autoreleasepool {
                    NSView *view = [(*handle).nsWindow contentView];
                    NSString *g = [(id)view.layer contentsGravity];
                    CGSize ds = CGSizeZero;
                    if ([view.layer isKindOfClass:[CAMetalLayer class]])
                        ds = ((CAMetalLayer*) view.layer).drawableSize;
                    extern uint64_t VkPane_presentCount(int index);
                    extern uint64_t VkPane_skipCount(int index);
                    extern int VkPane_count(void);
                    uint64_t presents = 0, skips = 0;
                    int panes = VkPane_count();
                    for (int pi = 0; pi < panes; pi++) {
                        presents += VkPane_presentCount(pi);
                        skips += VkPane_skipCount(pi);
                    }
                    NSLog(@"vk:probe frame=%.0fx%.0f content=%dx%d gravity=%@ drawable=%.0fx%.0f panes=%d presents=%llu skips=%llu",
                          [(*handle).nsWindow frame].size.width,
                          [(*handle).nsWindow frame].size.height,
                          (*handle).cachedWidth, (*handle).cachedHeight,
                          g, ds.width, ds.height, panes,
                          presents, skips);
                }
            }
        }
    }
    // Per-tick commit (see entry): releases YES-presents on frame cadence.
    [CATransaction commit];
}

// Build the NSWindow + C handle. Shared by every constructor. The window is
// created HIDDEN — visibility is an explicit Window_show() decision, so
// construct -> mutate -> show never flashes a half-configured window.
static Window *windowAlloc(const WindowDesc *desc) {
    @autoreleasepool {
        if (!NSApp) {
            [NSApplication sharedApplication];   // bootstrap the app object once
        }
        if (!sAppDelegate) {
            sAppDelegate = [[WindowAppDelegate alloc] init];
            [NSApp setDelegate:sAppDelegate];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp activateIgnoringOtherApps:YES];
        }

        NSRect frame = NSMakeRect(0, 0, (CGFloat)(*desc).width, (CGFloat)(*desc).height);

        NSWindowStyleMask style = NSWindowStyleMaskTitled
                                | NSWindowStyleMaskClosable
                                | NSWindowStyleMaskMiniaturizable
                                | NSWindowStyleMaskResizable;

        NSWindow *window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:style
                         backing:NSBackingStoreBuffered
                           defer:NO];
        [window setTitle:[NSString stringWithUTF8String:(*desc).title]];
        [window setReleasedWhenClosed:NO];   // we own the window object; close must not free it

        WindowContentView *contentView = [[WindowContentView alloc] initWithFrame:frame];
        [contentView setWantsLayer:YES];
        [window setContentView:contentView];

        // Green traffic light enters native fullscreen (mirrors legacy allocate()).
        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];

        // Disable AppKit's live-resize content-preservation scale so pinned
        // content is never stretched to the new size during a drag.
        [window setPreservesContentDuringLiveResize:NO];

        // Trackpad touch delivery: the content view must opt in. The modern
        // allowedTouchTypes API replaces the deprecated setAcceptsTouchEvents:.
        [window.contentView setAllowedTouchTypes:NSTouchTypeMaskDirect | NSTouchTypeMaskIndirect];

        WindowDelegate *delegate = [[WindowDelegate alloc] init];
        [window setDelegate:delegate];

        Window *w = (Window*) calloc(1, sizeof(Window));
        (*w).nsWindow = window;
        (*w).delegate = delegate;
        (*w).shouldClose = false;
        atomic_store_explicit(&(*w).sizeGeneration, 0, memory_order_relaxed);
        NSRect initialContent = [window contentRectForFrameRect:[window frame]];
        atomic_store_explicit(&(*w).cachedWidth, (int)initialContent.size.width, memory_order_relaxed);
        atomic_store_explicit(&(*w).cachedHeight, (int)initialContent.size.height, memory_order_relaxed);
        NSRect initialFrame = [window frame];
        CGFloat screenH = [[NSScreen mainScreen] frame].size.height;
        (*w).cachedX = (double)initialFrame.origin.x;
        (*w).cachedY = (double)(screenH - initialFrame.origin.y - initialFrame.size.height);
        NSRect initialContentRect = initialContent;
        (*w).cachedContentX = (double)initialFrame.origin.x
                              + (double)(initialContentRect.origin.x - initialFrame.origin.x);
        (*w).cachedContentY = (double)(screenH - initialContentRect.origin.y - initialContentRect.size.height);
        atomic_store_explicit(&(*w).presentMode, WINDOW_PRESENT_FIFO, memory_order_relaxed);
        atomic_store_explicit(&(*w).transparent, false, memory_order_relaxed);
        atomic_store_explicit(&(*w).renderGeneration, 0, memory_order_relaxed);
        atomic_store_explicit(&(*w).container, nullptr, memory_order_relaxed);
        atomic_store_explicit(&(*w).enabled, true, memory_order_relaxed);
        (*w).lastFocused = false;
        atomic_store_explicit(&(*w).monitorId, 0, memory_order_relaxed);
        (*w).windowAdapterCount = 0;
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
    WindowDesc d = { .title = "vex", .width = 800, .height = 600, .x = 0, .y = 0 };
    if (!desc)
        return d;
    if ((*desc).title) d.title = (*desc).title;
    if ((*desc).width > 0) d.width = (*desc).width;
    if ((*desc).height > 0) d.height = (*desc).height;
    d.x = (*desc).x;
    d.y = (*desc).y;
    d.centered = (*desc).centered;
    d.shown = (*desc).shown;
    return d;
}

// Default constructor: hidden, 800x600, "vex".
Window *Window_0(void) {
    return windowAlloc(&(WindowDesc){ .title = "vex", .width = 800, .height = 600 });
}

// One-arg overload: titled, hidden.
Window *Window_1(const char *title) {
    return windowAlloc(&(WindowDesc){ .title = title, .width = 800, .height = 600 });
}

// Parameterized constructor: Desc fields applied on top of defaults.
Window *Window_new(const WindowDesc *desc) {
    WindowDesc d = descResolve(desc);
    Window *w = windowAlloc(&d);
    if (!w)
        return nullptr;
    if (d.centered)
        Window_center(w);
    else if (d.x != 0 || d.y != 0)
        Window_setLocation(w, d.x, d.y);
    if (d.shown)
        Window_show(w);
    return w;
}

// Legacy-style convenience constructor: titled + sized, still hidden.
Window *Window_create(const char *title, int width, int height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Tear down the window and free the handle. Safe to call whether the user
// already closed the window or not: if it's still open we close it, and we
// detach the delegate first so no callback can touch our freed memory.
void Window_destroy(Window *window) {
    if (!window) return;
    @autoreleasepool {
        if (sPendingKeyWindow == (*window).nsWindow)
            sPendingKeyWindow = nil;
        [(*window).nsWindow setDelegate:nil];   // detach: no callbacks into freed struct
        if (!(*window).shouldClose) {
            [(*window).nsWindow close];
        }
        // Drop the id and any listeners still scoped to it so nothing dangles.
        Key_detachWindowAll((*window).id);
        Mouse_detachWindowAll((*window).id);
        Touch_detachWindowAll((*window).id);
        windowIdRelease((*window).id);
    }
    free(window);
}

bool Window_shouldClose(Window *window) {
    return window ? (*window).shouldClose : true;
}

// --- Present policy -----------------------------------------------------------
// Plain atomic words on the handle. presentMode and transparent participate
// in swapchain creation, so changing either bumps renderGeneration; color and
// container are per-frame polled and never do.

void Window_setPresentMode(Window *window, int mode) {
    if (!window)
        return;
    int prev = atomic_exchange_explicit(&(*window).presentMode, mode, memory_order_relaxed);
    if (prev != mode)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

int Window_getPresentMode(const Window *window) {
    return window ? atomic_load_explicit(&(*window).presentMode, memory_order_relaxed)
                  : WINDOW_PRESENT_FIFO;
}

void Window_setTransparent(Window *window, bool transparent) {
    if (!window)
        return;
    bool prev = atomic_exchange_explicit(&(*window).transparent, transparent, memory_order_relaxed);
    if (prev != transparent)
        atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

bool Window_isTransparent(const Window *window) {
    return window ? atomic_load_explicit(&(*window).transparent, memory_order_relaxed)
                  : false;
}

uint64_t Window_renderGeneration(const Window *window) {
    return window ? atomic_load_explicit(&(*window).renderGeneration, memory_order_acquire) : 0;
}

// --- Content: the ONE container slot ------------------------------------------

void Window_setContainer(Window *window, Panel *root) {
    if (!window)
        return;
    atomic_store_explicit(&(*window).container, root, memory_order_release);
}

Panel *Window_getContainer(const Window *window) {
    return window ? atomic_load_explicit(&(*window).container, memory_order_acquire) : nullptr;
}

void Window_setContentPanel(Window *window, Panel *panel) {
    if (!window)
        return;
    atomic_store_explicit(&(*window).contentPanel, panel, memory_order_release);
    // Content panel itself is a logical placeholder — Metal backing
    // goes on its children, not the panel itself.
}

Panel *Window_getContentPanel(const Window *window) {
    return window ? atomic_load_explicit(&(*window).contentPanel, memory_order_acquire) : nullptr;
}

void Window_setScenePanel(Window *window, Panel *panel) {
    if (!window)
        return;
    atomic_store_explicit(&(*window).scenePanel, panel, memory_order_release);
}

Panel *Window_getScenePanel(const Window *window) {
    return window ? atomic_load_explicit(&(*window).scenePanel, memory_order_acquire) : nullptr;
}

// --- Metal pane bridge (C callable from renderer) ------------------------
//
// Child-iteration logic lives in panel_bridge.c (a pure-C file that can
// see panel.h). This file just calls into it. Thread 0 only.

bool Window_attachPanes(Window *window, Panel *panel, int width, int height) {
    if (!window || !panel) return false;
    extern int Darling_attachPanes(Window *, Panel *, int, int);
    // Attach Metal pane backing to the scene children of the content panel
    return Darling_attachPanes(window, panel, width, height) >= 0;
}

bool Window_resizePanes(Window *window, Panel *panel, int width, int height) {
    if (!window || !panel) return false;
    extern int Darling_resizePanes(Window *, Panel *, int, int);
    return Darling_resizePanes(window, panel, width, height) >= 0;
}

void *Window_getPanelLayer(Window *window, Panel *panel) {
    if (!window || !panel) return nullptr;
    extern void *PanelCocoa_fromPanel(void *panel);
    extern void *PanelCocoa_layer(void *pc);
    void *pc = PanelCocoa_fromPanel(panel);
    return pc ? PanelCocoa_layer(pc) : nullptr;
}

void Window_compositePanes(Window *window, Panel *contentPanel) {
    if (!window || !contentPanel) return;

    // LIVE-RESIZE CONTRACT (Rule 11.6): while the window is being dragged,
    // pane/corner layers are anchored by CoreAnimation AUTORESIZING
    // (PanelCocoa_setAnchors' autoresizingMask + anchorPoint), which
    // WindowServer lays out INSIDE the window-resize transaction — moving the
    // sublayer in lockstep with the window edge on the same vsync, zero CPU
    // math, zero catch-up. Per-event explicit frames (Darling_getChildLayout)
    // would fight that accelerated pass and trail the live edge by a beat —
    // the right/bottom "catching up" artifact. Darling_preFrame is already
    // gated; this is the thread-0 twin. The settle pass re-applies exact
    // frames once the flag clears.
    if (Window_isLiveResizing(window))
        return;

    // CoreAnimation strictly requires layer tree mutations to occur on the main thread.
    // If the Vulkan background worker calls this, it must be asynchronously dispatched,
    // but if we're already on the main thread (e.g. during live resize tracking), we must
    // execute it synchronously to prevent CALayers from lagging one frame behind.
    dispatch_block_t block = ^{
        NSWindow *nsWindow = (*window).nsWindow;
        if (!nsWindow) return;
        NSView *contentView = [nsWindow contentView];
        if (!contentView) return;
        
        NSView *vulkanView = nil;
        Class vkClass = NSClassFromString(@"VulkanView");
        for (NSView *v in [contentView subviews]) {
            if (vkClass && [v isKindOfClass:vkClass]) {
                vulkanView = v;
                break;
            }
        }
        CALayer *rootLayer = vulkanView ? [vulkanView layer] : [contentView layer];
        if (!rootLayer) return;

        [CATransaction begin];
        [CATransaction setDisableActions:YES];

        extern int Darling_getChildCount(Panel *contentPanel);
        extern Panel *Darling_getChildAt(Panel *contentPanel, int index);
        extern void Darling_getChildLayout(Panel *child, float winW, float winH, float *outX, float *outY, float *outW, float *outH);
        extern int Darling_getChildAnchor(Panel *child);
        extern int Darling_getChildPivot(Panel *child);
        extern void *PanelCocoa_fromPanel(void *panel);
        extern void *PanelCocoa_layer(void *pc);
        extern void PanelCocoa_setAnchors(void *pc, int anchor, int pivot);

        // Add/update CALayers for each child
        int childCount = Darling_getChildCount(contentPanel);
        for (int i = 0; i < childCount; i++) {
            Panel *child = Darling_getChildAt(contentPanel, i);
            if (!child) continue;
            void *pc = PanelCocoa_fromPanel(child);
            if (!pc) continue;

            CALayer *childLayer = (__bridge CALayer*) PanelCocoa_layer(pc);
            if (!childLayer) continue;

            // Surface is pre-rendered at native pixel resolution — match backingScaleFactor
            // so CoreAnimation displays it at 1:1 logical points instead of double size on Retina.
            CGFloat scale = [nsWindow backingScaleFactor];
            if (scale <= 0.0) scale = 1.0;
            childLayer.contentsScale = scale;

            // Apply anchor+pivot settings to layer (contentsGravity + autoresizingMask)
            int anchor = Darling_getChildAnchor(child);
            int pivot = Darling_getChildPivot(child);
            PanelCocoa_setAnchors(pc, anchor, pivot);

            // Get layout rect for positioning
            float rx, ry, rw, rh;
            Darling_getChildLayout(child, (float)Window_width(window), (float)Window_height(window), &rx, &ry, &rw, &rh);
            [childLayer setFrame:CGRectMake(rx, ry, rw, rh)];

            // Add to root layer if not already there
            if ([childLayer superlayer] != rootLayer) {
                [rootLayer addSublayer:childLayer];
            }
        }
        [CATransaction commit];
    };

    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

// Board composite: scene + content panels backed as full-window CAMetalLayer
// boards (PanelCocoa_newBoard, one VkPane chain each). Scene parents below
// content; both fill the window at TOP_LEFT — the two named boards of the
// NSWindow -> Metal -> Vulkan-rect-children stack. Child panes nested under
// either board keep compositing through Window_compositePanes.
// Thread 0 only. Live-gated like its sibling: mid-drag the WindowServer owns
// all frame motion through the autoresizing masks.
void Window_compositeBoards(Window *window) {
    if (!window) return;
    extern void *PanelCocoa_fromPanel(void *panel);
    extern void *PanelCocoa_layer(void *pc);
    extern bool PanelCocoa_isBoard(const void *pc);
    extern void PanelCocoa_setAnchors(void *pc, int anchor, int pivot);
    Panel *scenePanel = atomic_load_explicit(&(*window).scenePanel, memory_order_acquire);
    Panel *contentPanel = atomic_load_explicit(&(*window).contentPanel, memory_order_acquire);
    void *scenePc = PanelCocoa_fromPanel(scenePanel);
    void *contentPc = PanelCocoa_fromPanel(contentPanel);
    if (scenePc && !PanelCocoa_isBoard(scenePc))
        scenePc = nullptr;
    if (contentPc && !PanelCocoa_isBoard(contentPc))
        contentPc = nullptr;
    if (!scenePc && !contentPc)
        return;
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            Window_compositeBoards(window);
        });
        return;
    }
    @autoreleasepool {
        NSWindow *nsWindow = (*window).nsWindow;
        if (!nsWindow) return;
        NSView *contentView = [nsWindow contentView];
        if (!contentView) return;
        NSView *vulkanView = nil;
        Class vkClass = NSClassFromString(@"VulkanView");
        for (NSView *v in [contentView subviews]) {
            if (vkClass && [v isKindOfClass:vkClass]) {
                vulkanView = v;
                break;
            }
        }
        CALayer *rootLayer = vulkanView ? [vulkanView layer] : [contentView layer];
        if (!rootLayer) return;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        float winW = (float)Window_width(window);
        float winH = (float)Window_height(window);
        CALayer *sceneLayer = nullptr;
        CALayer *contentLayer = nullptr;
        if (scenePc) {
            sceneLayer = (__bridge CALayer*) PanelCocoa_layer(scenePc);
            if (sceneLayer) {
                sceneLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
                [sceneLayer setFrame:CGRectMake(0, 0, winW, winH)];
                if ([sceneLayer superlayer] != rootLayer)
                    [rootLayer addSublayer:sceneLayer];
            }
        }
        if (contentPc) {
            contentLayer = (__bridge CALayer*) PanelCocoa_layer(contentPc);
            if (contentLayer) {
                contentLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
                [contentLayer setFrame:CGRectMake(0, 0, winW, winH)];
                if ([contentLayer superlayer] != rootLayer)
                    [rootLayer addSublayer:contentLayer];
            }
        }
        // Order contract: scene below content. Re-assert every pass so a
        // re-added layer can never strand above its sibling.
        if (sceneLayer && contentLayer)
            [rootLayer insertSublayer:sceneLayer below:contentLayer];
        [CATransaction commit];
    }
}

// --- Runtime state -------------------------------------------------------------

void Window_setEnabled(Window *window, bool enabled) {
    if (!window)
        return;
    atomic_store_explicit(&(*window).enabled, enabled, memory_order_relaxed);
}

bool Window_isEnabled(const Window *window) {
    return window ? atomic_load_explicit(&(*window).enabled, memory_order_relaxed) : false;
}

bool Window_isLiveResizing(const Window *window) {
    return window ? atomic_load_explicit(&(*window).liveResizing, memory_order_relaxed) : false;
}

// ---------------------------------------------------------------------------
// Chrome / state API. Mirrors the legacy macOSWindow method surface.
// NSWindowStyleMask bits line up 1:1 with the legacy constants (Resizable 1<<3,
// FullScreen 1<<14, FullSizeContentView 1<<15).
// ---------------------------------------------------------------------------

static NSWindowStyleMask styleMaskOf(Window *window) {
    return [(*window).nsWindow styleMask];
}

static bool hasStyleBit(Window *window, NSWindowStyleMask bit) {
    return (styleMaskOf(window) & bit) != 0;
}

// Single mask-rewrite path for all capability toggles. While native fullscreen
// AppKit owns the mask (the FullScreen bit can only change inside a transition),
// so style mutations are skipped then — mirroring the Ghostty guard. When not
// fullscreen the bit is never present, so it is never forced via setStyleMask:.
static void updateStyleMask(Window *window, NSWindowStyleMask add, NSWindowStyleMask clear) {
    NSWindowStyleMask mask = styleMaskOf(window);
    if ((mask & NSWindowStyleMaskFullScreen) != 0)
        return;
    [(*window).nsWindow setStyleMask:(mask & ~clear) | add];
}

void Window_setTitle(Window *window, const char *title) {
    if (!window || !title)
        return;
    @autoreleasepool {
        [(*window).nsWindow setTitle:[NSString stringWithUTF8String:title]];

        // macOS 15+ re-reveals the native title view whenever the title string
        // changes, even when titleVisibility is hidden. Re-apply the hidden
        // state for FullSizeContentView (NAKED) windows, mirroring legacy.
        if ((styleMaskOf(window) & NSWindowStyleMaskFullSizeContentView) != 0) {
            [(*window).nsWindow setTitlebarAppearsTransparent:YES];
            [(*window).nsWindow setTitleVisibility:NSWindowTitleHidden];
        }
    }
}

int Window_width(Window *window) {
    if (!window)
        return 0;
    // Atomic cache, not a live AppKit call: thread 0 writes cachedWidth in
    // setFrameSize/willResize/didResize/the pollEvents pump; the present
    // worker reads it every frame (two-thread live-resize contract —
    // calling contentRectForFrameRect from the worker would race AppKit).
    return atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed);
}

int Window_height(Window *window) {
    if (!window)
        return 0;
    return atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed);
}

void Window_setSize(Window *window, int width, int height) {
    if (!window)
        return;
    @autoreleasepool {
        [(*window).nsWindow setContentSize:NSMakeSize((CGFloat)width, (CGFloat)height)];
    }
}

void Window_setLocation(Window *window, int x, int y) {
    if (!window)
        return;
    @autoreleasepool {
        NSRect screen = [[NSScreen mainScreen] frame];
        [(*window).nsWindow setFrameTopLeftPoint:NSMakePoint((CGFloat)x, screen.size.height - (CGFloat)y)];
    }
}

// Mirrors the cached top-left desktop coords the pump already maintains for
// move reflection — no AppKit call needed on the read path.
void Window_getLocation(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (int)(*window).cachedX : 0;
    if (outY)
        *outY = window ? (int)(*window).cachedY : 0;
}

void Window_getContentOrigin(const Window *window, int *outX, int *outY) {
    if (outX)
        *outX = window ? (int)(*window).cachedContentX : 0;
    if (outY)
        *outY = window ? (int)(*window).cachedContentY : 0;
}

void Window_center(Window *window) {
    if (!window)
        return;
    @autoreleasepool {
        [(*window).nsWindow center];
    }
}

// Claim key synchronously: activation is async, so spin the runloop briefly
// until [NSApp isActive] commits, then take key while the grant is hot.
// Falls back to the pump's sPendingKeyWindow re-assertion if activation is
// denied outright (another app forced foreground) — the claim then sticks
// the moment the user clicks us, with no extra code.
static void claimKeyAfterActivation(NSWindow *nsw) {
    if (!nsw)
        return;
    [nsw orderFront:nil];
    [[NSRunningApplication currentApplication]
        activateWithOptions:NSApplicationActivateAllWindows];
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
    if ([nsw canBecomeKeyWindow])
        [nsw makeKeyWindow];
    // If activation never committed the makeKey above is a no-op; the pump
    // re-asserts it once [NSApp isActive] until it sticks.
    sPendingKeyWindow = nsw;
}

void Window_show(Window *window) {
    if (!window)
        return;
    @autoreleasepool {
        // Show-then-focus, synchronously: visible first, activation commits
        // inside claimKey (brief runloop spin), key claimed while hot — so
        // open == focused, with colored traffic lights, no hover needed.
        // Order matters: the old fire-and-forget activate + makeKeyAndOrderFront
        // ran the key claim while the app was still inactive and AppKit
        // silently dropped it (grey lights until manual refocus).
        claimKeyAfterActivation((*window).nsWindow);
        // Prime the monitor mirror eagerly: a window that just became
        // visible should know where it lives before the first pump.
        refreshMonitorId(window);
    }
}

void Window_hide(Window *window) {
    if (!window)
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
    if (!window)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskResizable);
}

void Window_setResizable(Window *window, bool resizable) {
    if (!window)
        return;
    updateStyleMask(window, resizable ? NSWindowStyleMaskResizable : 0, resizable ? 0 : NSWindowStyleMaskResizable);
}

bool Window_isClosable(Window *window) {
    if (!window)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskClosable);
}

void Window_setClosable(Window *window, bool closable) {
    if (!window)
        return;
    updateStyleMask(window, closable ? NSWindowStyleMaskClosable : 0, closable ? 0 : NSWindowStyleMaskClosable);
}

bool Window_isMiniaturizable(Window *window) {
    if (!window)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskMiniaturizable);
}

void Window_setMiniaturizable(Window *window, bool miniaturizable) {
    if (!window)
        return;
    updateStyleMask(window, miniaturizable ? NSWindowStyleMaskMiniaturizable : 0, miniaturizable ? 0 : NSWindowStyleMaskMiniaturizable);
}

void Window_setFullscreenButton(Window *window, bool enabled) {
    if (!window)
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
    if (!window)
        return;
    @autoreleasepool {
        NSWindowStyleMask mask = styleMaskOf(window);
        if ((mask & NSWindowStyleMaskFullScreen) != 0)
            return; // AppKit owns the mask in fullscreen

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

        // setStyleMask: resets the collectionBehavior, which drops the
        // FullScreenPrimary entitlement set at creation -> green orb dies
        // (greyed / zoom-only). Re-assert it on every chrome switch so
        // NAKED keeps native fullscreen. Never touched by teardown.
        [(*window).nsWindow setCollectionBehavior:([(*window).nsWindow collectionBehavior] | NSWindowCollectionBehaviorFullScreenPrimary)];

        bool transparent = (mode == WINDOW_UNDECORATED_NAKED);
        [(*window).nsWindow setTitlebarAppearsTransparent:transparent];
        [(*window).nsWindow setTitleVisibility:(transparent ? NSWindowTitleHidden : NSWindowTitleVisible)];
    }
}

void Window_setFloatingTrafficLights(Window *window, bool floating) {
    if (!window) return;
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
    }
}

void Window_setOpacity(Window *window, float opacity) {
    if (!window) return;
    @autoreleasepool {
        [(*window).nsWindow setAlphaValue:(CGFloat)opacity];
    }
}

void Window_setTransparentBackground(Window *window, bool transparent) {
    if (!window) return;
    @autoreleasepool {
        [(*window).nsWindow setOpaque:!transparent];
        [(*window).nsWindow setBackgroundColor:(transparent ? [NSColor clearColor] : [NSColor windowBackgroundColor])];
        if ([(*window).nsWindow contentView].layer) {
            [(*window).nsWindow contentView].layer.opaque = !transparent;
        }
        Window_setTransparent(window, transparent);
    }
}

void Window_setBlur(Window *window, float blur) {
    if (!window) return;
    @autoreleasepool {
        NSWindow *nsw = (*window).nsWindow;
        NSView *contentView = [nsw contentView];
        
        // Find existing blur view
        NSVisualEffectView *blurView = nil;
        for (NSView *v in [contentView subviews]) {
            if ([v isKindOfClass:[NSVisualEffectView class]]) {
                blurView = (NSVisualEffectView*) v;
                break;
            }
        }
        
        if (blur > 0.01f) {
            if (!blurView) {
                blurView = [[NSVisualEffectView alloc] initWithFrame:[contentView bounds]];
                [blurView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
                [blurView setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
                [blurView setMaterial:NSVisualEffectMaterialHUDWindow];
                [blurView setState:NSVisualEffectStateActive];
                // Add it below everything else
                [contentView addSubview:blurView positioned:NSWindowBelow relativeTo:nil];
            }
            [blurView setAlphaValue:(CGFloat)blur];
            // Ensure the window and the root Vulkan layer are transparent so the blur shows through
            [nsw setBackgroundColor:[NSColor clearColor]];
            [nsw setOpaque:NO];
            if (contentView.layer) {
                contentView.layer.opaque = NO;
            }
            // CRITICAL: Signal Vulkan to rebuild the swapchain with alpha blending enabled!
            Window_setTransparent(window, true);
        } else if (blurView) {
            [blurView removeFromSuperview];
            Window_setTransparent(window, false);
        }
    }
}

void Window_setAlwaysOnTop(Window *window, bool onTop) {
    if (!window) return;
    @autoreleasepool {
        [(*window).nsWindow setLevel:(onTop ? NSFloatingWindowLevel : NSNormalWindowLevel)];
    }
}

void Window_setClickThrough(Window *window, bool clickThrough) {
    if (!window) return;
    @autoreleasepool {
        [(*window).nsWindow setIgnoresMouseEvents:clickThrough];
    }
}

void Window_setShadow(Window *window, bool shadow) {
    if (!window) return;
    @autoreleasepool {
        [(*window).nsWindow setHasShadow:shadow];
    }
}

void Window_setMovableByBackground(Window *window, bool movable) {
    if (!window) return;
    @autoreleasepool {
        [(*window).nsWindow setMovableByWindowBackground:movable];
    }
}

void Window_minimize(Window *window) {
    if (!window)
        return;
    @autoreleasepool {
        [(*window).nsWindow miniaturize:nil];
    }
}

void Window_restore(Window *window) {
    if (!window)
        return;
    @autoreleasepool {
        [(*window).nsWindow deminiaturize:nil];
    }
}

bool Window_isMinimized(Window *window) {
    if (!window)
        return false;
    @autoreleasepool {
        return [(*window).nsWindow isMiniaturized];
    }
}

bool Window_isFullscreen(Window *window) {
    if (!window)
        return false;
    @autoreleasepool {
        return (styleMaskOf(window) & NSWindowStyleMaskFullScreen) != 0;
    }
}

void Window_setFullscreen(Window *window, bool fullscreen) {
    if (!window)
        return;
    @autoreleasepool {
        if (fullscreen != Window_isFullscreen(window))
            [(*window).nsWindow toggleFullScreen:nil];
    }
}

void Window_toggleFullscreen(Window *window) {
    if (!window)
        return;
    @autoreleasepool {
        [(*window).nsWindow toggleFullScreen:nil];
    }
}

void Window_setDRM(Window *window, bool enabled) {
    if (!window)
        return;
    @autoreleasepool {
        // NSWindowSharingNone = 0, NSWindowSharingReadOnly = 1
        [(*window).nsWindow setSharingType:(enabled ? NSWindowSharingNone : NSWindowSharingReadOnly)];
    }
}

void Window_setMinSize(Window *window, int width, int height) {
    if (!window)
        return;
    @autoreleasepool {
        [(*window).nsWindow setContentMinSize:NSMakeSize((CGFloat)width, (CGFloat)height)];
    }
}

void Window_setMaxSize(Window *window, int width, int height) {
    if (!window)
        return;
    @autoreleasepool {
        [(*window).nsWindow setContentMaxSize:NSMakeSize((CGFloat)width, (CGFloat)height)];
    }
}

void Window_setCursorLocked(Window *window, bool locked) {
    (void) window; // the lock warps in global screen space; any window works
    if (locked == s_cursorLocked)
        return;

    if (locked) {
        // Decouple cursor from motion so deltas arrive without drift between
        // warp passes; anchor is the current key window's centre in global
        // (bottom-left origin) screen coordinates.
        NSWindow *anchor = sLastWindow;
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
    if (!window) return;
    WindowCursorType prev = (*window).cursorType;
    (*window).cursorType = type;
    @autoreleasepool {
        if (prev == WINDOW_CURSOR_HIDDEN && type != WINDOW_CURSOR_HIDDEN) {
            [NSCursor unhide];
        }
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
    if (!window) return WINDOW_CURSOR_DEFAULT;
    return (*window).cursorType;
}

// ---------------------------------------------------------------------------
// Event wiring. Listeners attach to THIS window's id; dispatch routes events
// by the window tag they carried when the OS delivered them. Global device
// taps (Key_addListener etc.) still hear everything, engine-wide.
// ---------------------------------------------------------------------------

void Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (!window) return;
    Key_attachWindow((*window).id, adapter);
}

bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (!window) return false;
    return Key_detachWindow((*window).id, adapter);
}

void Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (!window) return;
    Mouse_attachWindow((*window).id, adapter);
}

bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (!window) return false;
    return Mouse_detachWindow((*window).id, adapter);
}

void Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (!window) return;
    Touch_attachWindow((*window).id, adapter);
}

bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (!window) return false;
    return Touch_detachWindow((*window).id, adapter);
}

// --- Window-lifecycle adapters (thread 0 registry on the handle) ---

void Window_addWindowAdapter(Window *window, const WindowEvent *adapter) {
    if (!window || !adapter || (*window).windowAdapterCount >= WINDOW_ADAPTER_MAX)
        return;
    (*window).windowAdapters[(*window).windowAdapterCount++] = adapter;
}

bool Window_removeWindowAdapter(Window *window, const WindowEvent *adapter) {
    if (!window || !adapter)
        return false;
    for (int i = 0; i < (*window).windowAdapterCount; i++) {
        if ((*window).windowAdapters[i] == adapter) {
            for (int j = i; j < (*window).windowAdapterCount - 1; j++)
                (*window).windowAdapters[j] = (*window).windowAdapters[j + 1];
            (*window).windowAdapterCount--;
            return true;
        }
    }
    return false;
}

void Window_dispatchEvents(Window *window) {
    (void) window; // rings are engine-global; any window handle may drain them
    Key_dispatchEvents();
    Mouse_dispatchEvents();
    Touch_dispatchEvents();
}

// --- Focus ---

uint32_t Window_id(Window *window) {
    return window ? (*window).id : FOCUS_BROADCAST;
}

// Ask the OS to make this the key window (the spotlight).
void Window_focus(Window *window) {
    if (!window) return;
    @autoreleasepool {
        claimKeyAfterActivation((*window).nsWindow);
        Focus_set((*window).id);
    }
}

// Pulls the window to the absolute front of the screen without forcing the 
// OS to steal keyboard focus from whatever app the user is typing in.
void Window_bringToFront(Window *window) {
    if (!window) return;
    @autoreleasepool {
        [(*window).nsWindow orderFrontRegardless];
    }
}

bool Window_isFocused(Window *window) {
    return window && Focus_isFocused((*window).id);
}

void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata) {
    if (!window)
        return;
    (*window).resizeRenderFn = fn;
    (*window).resizeRenderUserdata = userdata;
}

uint32_t Window_getMonitorId(const Window *window) {
    return window ? atomic_load_explicit(&(*window).monitorId, memory_order_acquire) : 0;
}

// --- Resize reflection ---

uint64_t Window_sizeGeneration(Window *window) {
    if (!window) return 0;
    return atomic_load_explicit(&(*window).sizeGeneration, memory_order_acquire);
}
// --- Software frame presentation ---------------------------------------------
// Packs the planar RGBA raster into an interleaved RGBX bitmap and stamps it
// into the content view's layer, aspect-fit. The scratch pack buffer is cached
// per size: steady-state presents are allocation-free.

bool Window_present(Window *window, const Buffer *frame) {
    if (!window || !frame)
        return false;
    size_t w = Buffer_width(frame);
    size_t h = Buffer_height(frame);
    if (w == 0 || h == 0)
        return false;

    static uint8_t *s_pixels = nullptr;
    static size_t s_cap = 0;
    size_t bytes = w * h * 4;
    if (bytes > s_cap) {
        uint8_t *grown = (uint8_t*) realloc(s_pixels, bytes);
        if (!grown)
            return false;
        s_pixels = grown;
        s_cap = bytes;
    }

    for (size_t y = 0; y < h; y++) {
        uint8_t *row = s_pixels + y * w * 4;
        for (size_t x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 0;
            ColorBuffer_getRGBA(frame, x, y, &r, &g, &b, &a);
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = 255; // opaque present; alpha channel reserved
        }
    }

    @autoreleasepool {
        NSWindow *nsw = (*window).nsWindow;
        NSView *view = [nsw contentView];
        [view setWantsLayer:YES];

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            s_pixels, w, h, 8, w * 4, cs,
            kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
        if (!ctx) {
            CGColorSpaceRelease(cs);
            return false;
        }
        CGImageRef img = CGBitmapContextCreateImage(ctx);

        CALayer *layer = view.layer;
        [CATransaction begin];
        [CATransaction setDisableActions:YES]; // no implicit fade between frames
        layer.contents = (__bridge id)img;
        layer.contentsGravity = kCAGravityResizeAspect;
        [CATransaction commit];

        CFRelease(img);
        CFRelease(ctx);
        CGColorSpaceRelease(cs);
        return true;
    }
}

void *Window_contentView(Window *window) {
    if (!window || !(*window).nsWindow)
        return nullptr;
    return (__bridge void*)[(*window).nsWindow contentView];
}

@implementation VulkanView
- (BOOL)isFlipped {
    return YES;
}

- (NSView*)hitTest:(NSPoint)point {
    (void) point;
    return nil; // Let events pass through to WindowContentView
}

// Zoom bridge accessors — thread 0 only, no atomic.
- (BOOL)isZooming {
    return _zooming;
}

- (void)setZooming:(BOOL)zooming {
    _zooming = zooming;
}

- (BOOL)isFullScreenTransitioning {
    return _fullScreenTransitioning;
}

- (void)setFullScreenTransitioning:(BOOL)transitioning {
    _fullScreenTransitioning = transitioning;
}

- (void)settleAfterResize {
    Window *w = windowHandleOf([self window]);
    // Idempotent settle: exactly one rebuild/re-render per drag. A repeat
    // call with no live state and no stashed size is a no-op — gravity is
    // already TopLeft, _pendingSize already consumed, caches already final.
    // (viewDidEndLiveResize and windowDidEnter/ExitFullScreen may both fire
    // for one gesture; the second call lands here and returns.)
    if (!_liveResizing && !_zooming && _pendingSize.width == 0.0
        && _pendingSize.height == 0.0
        && (w == nullptr || !Window_isLiveResizing(w)))
        return;
    _liveResizing = NO;
    _zooming = NO;
    static bool s_settleTraceInit = false;
    static bool s_settleTrace = false;
    if (!s_settleTraceInit) {
        s_settleTraceInit = true;
        s_settleTrace = getenv("ANTI_VK_TRACE") != nullptr;
    }
    if (s_settleTrace)
        NSLog(@"vk: live-resize settle %.0fx%.0f", [self frame].size.width, [self frame].size.height);
    if (w) {
        // Settle: release the live-resize gate so the NEXT present pass runs
        // one final caps-drift rebuild at
        // the true final size. The flag clears BEFORE resizeRenderFn so that
        // settle pass sees a non-live window. Board panes return to TopLeft
        // transaction-synced presents first, so the final frames pin exactly.
        extern void PanelCocoa_setLiveResizingAll(bool live);
        PanelCocoa_setLiveResizingAll(false);
        atomic_store_explicit(&(*w).liveResizing, false, memory_order_relaxed);

        NSSize currentFrameSize = [self frame].size;
        NSSize finalSize = (currentFrameSize.width > 0.0 && currentFrameSize.height > 0.0)
            ? currentFrameSize
            : ((_pendingSize.width > 0.0 && _pendingSize.height > 0.0) ? _pendingSize : NSZeroSize);
        _pendingSize = NSZeroSize;

        atomic_store_explicit(&(*w).cachedWidth, (int)finalSize.width, memory_order_relaxed);
        atomic_store_explicit(&(*w).cachedHeight, (int)finalSize.height, memory_order_relaxed);

        if ([[self layer] isKindOfClass:[CAMetalLayer class]]) {
            // Restore exact 1:1 pinning BEFORE the final drawableSize lands:
            // the frozen drag frame is replaced by the settle rebuild's
            // exact-size present. TopLeft pins the last frozen drawable for
            // the one-frame gap between settle and rebuild — never stretches.
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            [(CAMetalLayer*) [self layer] setContentsGravity:kCAGravityTopLeft];
            // Reassert the YES that persisted since makeBackingLayer (idempotent:
            // neither drag, zoom, nor fullscreen ever clears it now).
            [(CAMetalLayer*) [self layer] setPresentsWithTransaction:YES];
            [CATransaction commit];
        }

        if ([[self layer] isKindOfClass:[CAMetalLayer class]]) {
            CAMetalLayer *metal = (CAMetalLayer*) [self layer];
            CGFloat scale = [[self window] backingScaleFactor];
            if (scale <= 0.0)
                scale = 1.0;
            metal.drawableSize = CGSizeMake((CGFloat)(finalSize.width * scale + 0.5),
                                            (CGFloat)(finalSize.height * scale + 0.5));
        }

        // Apply exact settled layout rects for all panels synchronously on thread 0
        Panel *contentPanel = atomic_load_explicit(&(*w).contentPanel, memory_order_acquire);
        if (contentPanel) {
            extern void Darling_setPanelSize(Panel *p, float w, float h);
            Darling_setPanelSize(contentPanel, (float)finalSize.width, (float)finalSize.height);
            Window_compositePanes(w, contentPanel);
        }

        if ((*w).resizeRenderFn)
            (*w).resizeRenderFn((*w).resizeRenderUserdata);
    }
}

- (void)viewWillStartLiveResize {
    _liveResizing = YES;
    static bool s_dragTraceInit = false;
    static bool s_dragTrace = false;
    if (!s_dragTraceInit) {
        s_dragTraceInit = true;
        s_dragTrace = getenv("ANTI_VK_TRACE") != nullptr;
    }
    if (s_dragTrace)
        NSLog(@"vk: live-resize begin");
    Window *w = windowHandleOf([self window]);
    if (w)
        atomic_store_explicit(&(*w).liveResizing, true, memory_order_relaxed);

    // Board-contract twin: window-sized Metal boards keep their exact-size
    // drawable TopLeft-pinned through the drag (freeze-exact — never Resize)
    // while fixed child panes stay TopLeft-pinned. Reasserted at settle.
    extern void PanelCocoa_setLiveResizingAll(bool live);
    PanelCocoa_setLiveResizingAll(true);

    // FREEZE-EXACT DRAG: the board swapchain stays at its frozen extent and
    // its gravity stays TopLeft, so the last presented frame keeps its exact
    // pre-drag pixels pinned top-left every drag step — zero stretch. The
    // seam beyond the frozen extent is the board's transparent (opaque=NO)
    // remainder: blur / window background shows through as the window grows.
    // This is the "empty gorge" trade-off the old Resize gravity hid: the
    // frozen frame does NOT cover the growing bounds, but it also never
    // smears. The settle rebuild replaces the frozen frame with an exact
    // TopLeft one at the true final size. The panes (separate CALayer
    // sublayers) are not affected by this gravity; they keep animating in
    // place. Pane anchoring during the drag is WindowServer-accelerated:
    // their autoresizingMask + anchorPoint (PanelCocoa_setAnchors) make CA
    // move the sublayers inside the window-resize transaction, edge-locked,
    // no per-event CPU frames (Window_compositePanes early-returns while live).
    if ([[self layer] isKindOfClass:[CAMetalLayer class]]) {
        // YES persists from makeBackingLayer through the drag: the worker
        // commits its own explicit transaction per present walk, so YES
        // presents release on worker cadence with no thread-0 dependency.
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [(CAMetalLayer*) [self layer] setContentsGravity:kCAGravityTopLeft];
        [CATransaction commit];
    }

    [super viewWillStartLiveResize];
}

- (void)viewDidEndLiveResize {
    if (_fullScreenTransitioning) {
        // Defer settle: during native macOS fullscreen transitions, Cocoa fires
        // viewDidEndLiveResize ~11ms in on the initial frame layout before the Spaces
        // slide animation has even visually begun. Settling now would prematurely
        // clear liveResizing and lock Core Animation transactions, stalling presentation.
        // windowDidEnterFullScreen / windowDidExitFullScreen will own the true settle.
        [super viewDidEndLiveResize];
        return;
    }
    [self settleAfterResize];
    [super viewDidEndLiveResize];
}

- (CALayer*)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.contentsGravity = kCAGravityTopLeft;
    layer.geometryFlipped = YES;
    layer.opaque = NO;
    layer.presentsWithTransaction = YES;
    layer.device = MTLCreateSystemDefaultDevice();
    CGFloat scale = [self window] ? [[self window] backingScaleFactor] : 1.0;
    if (scale <= 0.0)
        scale = 1.0;
    layer.contentsScale = scale;
    return layer;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    NSWindow *w = [self window];
    if (w) {
        CGFloat scale = [w backingScaleFactor];
        if (scale > 0.0 && [[self layer] isKindOfClass:[CAMetalLayer class]]) {
            [self layer].contentsScale = scale;
        }
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    Window *w = windowHandleOf([self window]);
    if (!w)
        return;

    CGFloat scale = [self window] ? [[self window] backingScaleFactor] : 1.0;
    if (scale <= 0.0)
        scale = 1.0;
    uint32_t pxW = (uint32_t) (newSize.width * scale + 0.5);
    uint32_t pxH = (uint32_t) (newSize.height * scale + 0.5);
    if (pxW == 0 || pxH == 0)
        return;

    atomic_store_explicit(&(*w).cachedWidth, (int) newSize.width, memory_order_relaxed);
    atomic_store_explicit(&(*w).cachedHeight, (int) newSize.height, memory_order_relaxed);

    Panel *contentPanel = atomic_load_explicit(&(*w).contentPanel, memory_order_acquire);
    if (contentPanel) {
        extern void Darling_setPanelSize(Panel *p, float w, float h);
        Darling_setPanelSize(contentPanel, (float)newSize.width, (float)newSize.height);
        Window_compositeBoards(w);
    }

    // LIVE-RESIZE CONTRACT (Rule 11.6): mid-drag thread 0 moves CALayer
    // frames only (above). Swapchain rebuilds and synchronous render hooks
    // are settle-only — per-drag-pixel GPU work is the size-proportional-lag
    // defect. settleAfterResize owns the one rebuild + one re-render.
    BOOL live = [self window] ? [[self window] inLiveResize] : NO;
    if (!live)
        live = _liveResizing || _fullScreenTransitioning;
    if (live)
        return;

    if ([[self layer] isKindOfClass:[CAMetalLayer class]]) {
        CAMetalLayer *metal = (CAMetalLayer*) [self layer];
        metal.drawableSize = CGSizeMake((CGFloat) pxW, (CGFloat) pxH);
    }

    // Fire resize render hook synchronously inside this layout turn
    if ((*w).resizeRenderFn)
        (*w).resizeRenderFn((*w).resizeRenderUserdata);
}
@end

// Resolve the VulkanView from an NSWindow's contentView subviews (thread 0).
static VulkanView *findVulkanView(NSWindow *window) {
    if (!window) return nil;
    NSView *contentView = [window contentView];
    if (!contentView) return nil;
    for (NSView *v in [contentView subviews]) {
        if ([v isKindOfClass:[VulkanView class]])
            return (VulkanView*) v;
    }
    return nil;
}

void *Window_metalLayer(Window *window) {
    if (!window || !(*window).nsWindow)
        return nullptr;
    @autoreleasepool {
        NSView *contentView = [(*window).nsWindow contentView];
        
        // Find existing Vulkan view or create it
        VulkanView *vulkanView = nil;
        for (NSView *v in [contentView subviews]) {
            if ([v isKindOfClass:[VulkanView class]]) {
                vulkanView = (VulkanView*) v;
                break;
            }
        }
        
        if (!vulkanView) {
            vulkanView = [[VulkanView alloc] initWithFrame:contentView.bounds];
            [vulkanView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
            [vulkanView setWantsLayer:YES];
            [contentView addSubview:vulkanView]; // Goes on top of NSVisualEffectView!
        }
        
        return (__bridge void*) [vulkanView layer];
    }
}

// Assert the layer geometry contract synchronously on thread 0.
static void applyLayerGravity(Window *window) {
    if (!window)
        return;
    @autoreleasepool {
        NSView *contentView = [(*window).nsWindow contentView];
        if (!contentView) return;
        
        VulkanView *vulkanView = nil;
        for (NSView *v in [contentView subviews]) {
            if ([v isKindOfClass:[VulkanView class]]) {
                vulkanView = (VulkanView*) v;
                break;
            }
        }
        
        if (!vulkanView || !vulkanView.layer)
            return;
        
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        vulkanView.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
        vulkanView.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        vulkanView.layer.contentsGravity = kCAGravityTopLeft;
        vulkanView.layer.needsDisplayOnBoundsChange = YES;
        [CATransaction commit];
    }
}

void Window_setGravityTopLeft(Window *window) {
    // Rebuild-moment reassertion: the present worker calls this the instant
    // a fresh swapchain lands, and the block fires on thread 0 the moment
    // the runloop can service it — beating the next scheduled pump pass in
    // the common case. Safety: capture the NSWindow STRONGLY and resolve
    // the live C handle inside the block, so a window destroyed between
    // enqueue and execution cannot dangle. During live resize (Rule 11.6)
    // the drag already pins TopLeft (freeze-exact); the gate skips the
    // redundant CA reapplication mid-drag so the tracking loop stays clean.
    if (!window)
        return;
    @autoreleasepool {
        NSWindow *nsw = (*window).nsWindow;
        dispatch_async(dispatch_get_main_queue(), ^{
            Window *live = windowHandleOf(nsw);
            if (live && !Window_isLiveResizing(live))
                applyLayerGravity(live);
        });
    }
}

// Worker present transaction: explicit CoreAnimation commit per board+pane
// walk. The present worker owns no runloop, so its implicit transaction
// never commits at idle and every presentsWithTransaction=YES drawable
// would stall behind it (freeze-at-idle). Wrapping one walk
// (Vk_clearPresent + VkPane_presentAll) in Begin/End releases YES-presents
// on worker cadence — which is exactly why layers can stay YES from
// makeBackingLayer through drag, zoom, and fullscreen. Gravity untouched:
// TopLeft at all times (freeze-exact steady-state AND mid-drag), owned by
// thread 0.
void Window_workerPresentBegin(void) {
    if (s_workerPresentArmed)
        return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    s_workerPresentArmed = true;
}

void Window_workerPresentEnd(void) {
    if (!s_workerPresentArmed)
        return;
    [CATransaction commit];
    s_workerPresentArmed = false;
#ifndef NDEBUG
    NSLog(@"vk: worker present committed (board+pane walk)");
#endif
}
