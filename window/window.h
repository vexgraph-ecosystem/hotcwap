#ifndef WINDOW_WINDOW_H
#define WINDOW_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event/keyhandler.h"
#include "event/mousehandler.h"
#include "event/touchhandler.h"

// window/window.h — platform-agnostic window API.
//
// The implementation is window_cocoa.m (the one ObjC file). C callers
// never see AppKit: they get an opaque handle, create/destroy it, and poll
// the OS event queue once per frame. This is the seam where a Win32 or X11
// backend could later drop in with zero changes above this header.
//
// The method surface mirrors the legacy macOSWindow (the FFM backend that
// lived in _legacy-java): title/size/position, chrome capability toggles
// (resizable/closable/miniaturizable/traffic lights), fullscreen, minimize,
// undecorated (naked) chrome, DRM (sharing) mode, and size constraints.
//
// Three state families live on the handle:
//   POLICY    — present pacing + clear color + transparency. Written by
//               thread 0 any time; the GPU consumer polls them through
//               atomic words and watches renderGeneration for swapchain
//               rebuilds. Zero allocation, zero locks.
//   CONTENT   — exactly ONE container slot. nullptr root => the renderer has
//               nothing to draw and degrades to a clear-only pass. All
//               nesting happens INSIDE that root via Panel_addContainer;
//               the window influences whatever hangs under it.
//   ADAPTERS  — key/mouse/touch/window listener vtables attached by
//               pointer identity, dispatched on thread 0 during
//               Window_dispatchEvents. A disabled window mutes its input.

// Opaque handle; contents live in the backend file. The tag stays INCOMPLETE
// here on purpose (exception to preferences rule 3): the backend translation
// unit completes struct Window with its real fields.
typedef struct Window Window;

typedef struct Panel Panel;

// Undecorated chrome modes for Window_setUndecorated.
#define WINDOW_DECORATED  0 // standard opaque title bar, title visible
#define WINDOW_UNDECORATED_BORDERLESS 1 // no title bar and no traffic lights
#define WINDOW_UNDECORATED_NAKED      2 // transparent title bar, hidden title, traffic lights kept

// Present pacing. What MoltenVK actually exposes — FIFO paces frames to the
// display (vsync), IMMEDIATE submits unthrottled. Naming follows the Vulkan
// present modes they map onto, not marketing words.
#define WINDOW_PRESENT_FIFO      0 // display-synced, capped (default)
#define WINDOW_PRESENT_IMMEDIATE 1 // uncapped, no sync

// Constructor parameters. Every field has a default; a call site names only
// what it wants to change. Zero it for pure defaults.
typedef struct WindowDesc {
    const char *title;   // default "vex"
    int width;           // default 800
    int height;          // default 600
    int x;               // top-left, default 0
    int y;               // default 0
    bool centered;       // default false
    bool shown;          // default false — construct hidden, show() when ready
} WindowDesc;

// --- Overloaded constructors (the Vec4 chooser idiom) ---
//
//   Window()                     -> defaults, hidden
//   Window("title")              -> titled, hidden
//   Window("title", 800, 600)    -> legacy create, hidden
//   Window_new(&(WindowDesc){…}) -> every other field (x/y/centered/shown)
//
// All variants construct HIDDEN: construct -> mutate -> Window_show().
// The macro is function-like, so it never fires when `Window` is used as the
// type name — only at call sites with parentheses.

Window *Window_0(void);
Window *Window_1(const char *title);
Window *Window_3(const char *title, int width, int height);

#define WINDOW_CHOOSER(_0, _1, _2, _3, NAME, ...) NAME

#define Window(...) WINDOW_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    Window_3, Window_2, Window_1, Window_0 \
)(__VA_ARGS__)

// Parameterized constructor: Desc fields applied on top of defaults.
// Pass &(WindowDesc){ .title = "...", .centered = true } — unset fields keep
// their defaults. Returns nullptr on failure.
Window *Window_new(const WindowDesc *desc);

// Legacy-style convenience constructor: titled, sized, created hidden.
Window *Window_create(const char *title, int width, int height);

// Close the window and free the handle. Safe if already closed.
void Window_destroy(Window *window);

// True once the user has asked to close (red button / Cmd+W).
bool Window_shouldClose(Window *window);

// Drain the OS event queue. Call once per frame from the engine loop.
void Window_pollEvents(void);

// --- Title / size / position ---
void Window_setTitle(Window *window, const char *title);
int  Window_width(Window *window);
int  Window_height(Window *window);
void Window_setSize(Window *window, int width, int height);
void Window_setLocation(Window *window, int x, int y);
// Top-left corner in global desktop points (the space setLocation speaks).
void Window_getLocation(const Window *window, int *outX, int *outY);
// Top-left of the CONTENT area (below the title bar) in desktop points —
// the space darling layouts and GPU caches speak. This is what renderers
// should join against, not getLocation (the frame includes chrome).
void Window_getContentOrigin(const Window *window, int *outX, int *outY);
void Window_center(Window *window);
void Window_show(Window *window);
void Window_hide(Window *window);
void Window_setVisible(Window *window, bool visible);

// --- Content: the ONE container slot ------------------------------------------
//
// The root is a Panel (the basket); the game panel, UI panels, everything
// nests UNDER it via Panel_addContainer — the scene3d is just a child like
// any other. The basket MIRRORS the window: its width/height are rewritten
// to the window's content size whenever the window resizes (size-generation
// reflection), so percentage layouts and edge anchors inside it track the
// real window without anyone forwarding sizes by hand.
//
// Setting nullptr detaches content and the renderer falls back to a clear-only
// pass — nothing is displayed. The renderer re-reads this pointer every frame
// (relaxed atomic), so swaps land on the next presented frame.
// Two-layer split architecture:
//   - contentPanel: the UI tree (board-backed, child panes composited by AppKit)
//   - scenePanel: the scene tree (Vulkan swapchain-backed)

void   Window_setContainer(Window *window, Panel *root);
Panel *Window_getContainer(const Window *window);

// Set the content panel (the UI tree, composited natively via CALayers).
void   Window_setContentPanel(Window *window, Panel *panel);
Panel *Window_getContentPanel(const Window *window);

// Set the scene panel (the Vulkan-rendered scene tree stamped on the swapchain).
void   Window_setScenePanel(Window *window, Panel *panel);
Panel *Window_getScenePanel(const Window *window);

// --- Metal pane bridge (C callable from renderer) ------------------------
//
// Scene children get Metal pane backing and AppKit composites them
// via CALayers. These functions let the renderer attach, resize, and
// position the CALayers for Metal-backed panels. Thread 0 only.

bool Window_attachPanes(Window *window, Panel *panel, int width, int height);
bool Window_resizePanes(Window *window, Panel *panel, int width, int height);
void Window_compositePanes(Window *window, Panel *contentPanel);

// Board composite: the scene + content panels when backed as full-window
// CAMetalLayer boards (PanelCocoa_newBoard). Parents the scene board below
// the content board under the window's root layer at full-window frames —
// stack: NSWindow -> board Metal -> scene Metal -> content Metal -> child
// panes, recursively. No-op for panels without board backing (child-pane
// scenes still composite through Window_compositePanes).
// Thread 0 only (like all layer-tree mutation).
void Window_compositeBoards(Window *window);

// --- Present policy -----------------------------------------------------------
//
// Written by thread 0 whenever; consumed by the GPU thread through atomic
// loads. presentMode and transparent participate in the swapchain, so a
// change bumps Window_renderGeneration(); the consumer compares generations
// and rebuilds targets on drift (same reflection contract as resize).
//
// NOTE: there is deliberately NO background color here. Color is content —
// it lives on the container panel hung in Window_setContainer, and the
// renderer clears its monitor cache to that panel's color. Unset panel
// color (PANEL_COLOR_CLEAR) means transparent across the board.

void     Window_setPresentMode(Window *window, int mode);
int      Window_getPresentMode(const Window *window);

// Composite transparency for the swapchain surface. Selecting true asks the
// rebuild path to pick a non-opaque compositeAlpha from what the driver
// actually supports.
void Window_setTransparent(Window *window, bool transparent);
bool Window_isTransparent(const Window *window);

// Monotonic counter bumped by thread 0 whenever presentMode or transparent
// changed. Renderers compare their last-applied generation against this and
// rebuild the swapchain when it moved.
uint64_t Window_renderGeneration(const Window *window);

// --- Runtime state ---

// Input-only kill switch: a disabled window delivers NO OS input — events
// never enter the device rings, adapters stay silent, polling freezes.
// Presentation and rendering are untouched.
void Window_setEnabled(Window *window, bool enabled);
bool Window_isEnabled(const Window *window);

// Live-resize flag: set by thread 0 while AppKit is inside an active window
// drag (NSViewLiveResize). The renderer reads it to keep presenting the
// current chain WITHOUT rebuilding: live resize moves CALayer frames (panes
// track at full rate), it must NOT resize pane chains or rebuild
// swapchains per drag frame. On settle the flag clears and exactly one
// resize + one rebuild converge to the final size.
bool Window_isLiveResizing(const Window *window);

// --- Chrome capability toggles (style-mask API) ---
bool Window_isResizable(Window *window);
void Window_setResizable(Window *window, bool resizable);
bool Window_isClosable(Window *window);
void Window_setClosable(Window *window, bool closable);
bool Window_isMiniaturizable(Window *window);
void Window_setMiniaturizable(Window *window, bool miniaturizable);

// Green traffic light (fullscreen entry). Gated by NSWindowCollectionBehavior
// FullScreenPrimary, set at creation. Call before the window shows to remove it.
void Window_setFullscreenButton(Window *window, bool enabled);

// Switch window chrome at runtime: one of WINDOW_UNDECORATED_*.
void Window_setUndecorated(Window *window, int type);
void Window_setFloatingTrafficLights(Window *window, bool floating); // Transparent titlebar, leaves just traffic lights over content

void Window_setOpacity(Window *window, float opacity); // 0.0 to 1.0
void Window_setTransparentBackground(Window *window, bool transparent); // Makes the window backdrop fully clear so Vulkan can draw holes
void Window_setBlur(Window *window, float blur);       // 0.0 to 1.0 (adds frosted glass behind content)
void Window_setAlwaysOnTop(Window *window, bool onTop);
void Window_setClickThrough(Window *window, bool clickThrough);
void Window_setShadow(Window *window, bool shadow);
void Window_setMovableByBackground(Window *window, bool movable);

// --- Z-Order & Presentation ---
void Window_bringToFront(Window *window); // Pulls window to the top of its level without stealing keyboard focus


// --- Minimize ---
void Window_minimize(Window *window);
void Window_restore(Window *window);
bool Window_isMinimized(Window *window);

// --- Fullscreen ---
bool Window_isFullscreen(Window *window);
void Window_setFullscreen(Window *window, bool fullscreen);
void Window_toggleFullscreen(Window *window);

// --- DRM / sharing ---
void Window_setDRM(Window *window, bool enabled);

// --- Size constraints ---
void Window_setMinSize(Window *window, int width, int height);
void Window_setMaxSize(Window *window, int width, int height);

// --- Cursor control ---
typedef enum WindowCursorType {
    WINDOW_CURSOR_DEFAULT       = 0,
    WINDOW_CURSOR_IBEAM         = 1,
    WINDOW_CURSOR_POINTING_HAND = 2,
    WINDOW_CURSOR_CROSSHAIR     = 3,
    WINDOW_CURSOR_RESIZE_EW     = 4,
    WINDOW_CURSOR_RESIZE_NS     = 5,
    WINDOW_CURSOR_NOT_ALLOWED   = 6,
    WINDOW_CURSOR_HIDDEN        = 7,
} WindowCursorType;

void Window_setCursorType(Window *window, WindowCursorType type);
WindowCursorType Window_getCursorType(const Window *window);

// FPS-style relative cursor: hides the pointer, decouples it from movement,
// and re-warps to the window centre each pump pass while deltas flow into the
// input/mouse stream as move-delta events. (Legacy: macOSWindow.setCursorLock.)
void Window_setCursorLocked(Window *window, bool locked);

// The AppKit content view behind this window — the surface anchor GPU backends
// need (MoltenVK wraps it in a CAMetalLayer). THREAD CONTRACT: thread 0 only.
void *Window_contentView(Window *window);

// Creates (or reuses) a CAMetalLayer on the content view — the VK_EXT_metal_surface
// path. Returns nullptr off-Apple. THREAD CONTRACT: thread 0 only.
void *Window_metalLayer(Window *window);
void Window_setGravityTopLeft(Window *window);

// --- Software frame presentation ---
//
// Stamp an RGBA raster (ColorBuffer layout) into the window's content view,
// scaled to fit. THREAD CONTRACT: call from thread 0 only — this touches
// AppKit, and AppKit owns its main thread like a landlord.
typedef struct Buffer Buffer;
bool Window_present(Window *window, const Buffer *frame);

// --- Event wiring (adapters) ---
//
// The window is the registration surface for the event contracts: implement
// a KeyHandler/MouseHandler/TouchHandler vtable (with .self = your object) and
// attach it here. Every queued event carries the id of the window the OS
// delivered it to, so an attached adapter only hears events for ITS window
// (broadcast-tagged synthetic events reach every window). Removal is by
// pointer identity. Destroying the window detaches its listeners.
void Window_addKeyAdapter(Window *window, const KeyHandler *adapter);
bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter);
void Window_addMouseAdapter(Window *window, const MouseHandler *adapter);
bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter);
void Window_addTouchAdapter(Window *window, const TouchHandler *adapter);
bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter);

// Window-lifecycle adapter: close requests, focus flips, resize/move moves,
// monitor hand-offs. Fired from the pump pass on thread 0, AFTER the OS
// event is processed.
typedef struct WindowEvent {
    void *self;
    void (*onCloseRequested)(void *self, Window *window);
    void (*onFocusChanged)(void *self, Window *window, bool focused);
    void (*onResized)(void *self, Window *window, int width, int height);
    void (*onMoved)(void *self, Window *window, int x, int y);
    // oldId or newId is 0 when the window leaves/joins the mapped set
    // (headless boot, screen unplugged with no successor).
    void (*onMonitorChanged)(void *self, Window *window,
                             uint32_t oldMonitorId, uint32_t newMonitorId);
} WindowEvent;

void Window_addWindowAdapter(Window *window, const WindowEvent *adapter);
bool Window_removeWindowAdapter(Window *window, const WindowEvent *adapter);

// The running: drain all three device rings into the registered adapters.
// Call ONCE per frame from the game loop, after Window_pollEvents(). If you
// never call it, polling (Key_isDown/Mouse_x) still works but queued events
// pile up and drop once the rings fill.
void Window_dispatchEvents(Window *window);

// --- Focus (the spotlight: one focused window per machine) ---
//
// The OS owns focus; we mirror it. After every pump pass the key window's id
// lands in one atomic word any thread can read.
uint32_t Window_id(Window *window);      // 0 when window is nullptr
void Window_focus(Window *window);       // ask the OS to make this key
bool Window_isFocused(Window *window);   // is THIS the spotlight right now?

// --- Monitor identity ---
//
// The OS owns display topology; we mirror it. Every pump pass resolves which
// screen carries the window (greatest intersection) and lands its
// CGDirectDisplayID in one atomic word any thread can read. Primed eagerly
// by Window_show; 0 means unknown (never shown, or headless). When the value
// flips — drag across displays, screen unplugged — WindowEvent's
// onMonitorChanged fires on thread 0.
uint32_t Window_getMonitorId(const Window *window);

// --- Resize reflection ---
//
// Monotonic counter bumped by Thread 0 whenever Window_pollEvents observes
// the content size actually changed. Renderers, layouts, and scenes compare
// their last-seen generation against this and rebuild when it moved — one
// frame of lag at worst, zero AppKit calls off Thread 0, no locks.
uint64_t Window_sizeGeneration(Window *window);

// --- Resize-cadence rendering (the c -> objc -> c bridge) --------------------
//
// AppKit resizes windows inside its own nested tracking loop; a decoupled
// present thread always trails that cadence by up to a frame. One hook, two
// bridges, both ON THREAD 0:
//   - windowWillResize/windowDidResize (tracking ticks) route through the
//     SYNC path: they wait out any in-flight frame, then render+present
//     inline, so every border step carries an exactly-sized frame.
//   - the pump pass keeps opportunistic catch-up for move-drags and
//     programmatic changes.
// Hook implementations may still drop internally under contention; the
// regular loop catches up one tick later either way.
typedef void (*WindowResizeRenderFn)(void *userdata);
void Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, void *userdata);

#endif
