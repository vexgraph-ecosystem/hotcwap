# hotcwap Bridging Doctrine — Loader + Event Bridge, Never a Gallery

> R1 Host. hotcwap exists for two jobs: the **loading process** (Kernel,
> process taxonomy, hot-swap) and the **event bridge** (window events open
> for graphics consumers). There is no gallery here, no scene, no pixels —
> by decision, not by omission. A gallery would drag UI, layout, and paint
> into the supervisor and rot it into spaghetti. The graphics live in
> graphvex (R3) and the UI in darling (R4); hotcwap hands them a stable
> window and a fired event, then gets out of the way (the Window Decoupling
> Law, the Vertical Integration Law).

---

## 1. The Two Jobs

| # | Job | Owner files | Never does |
|:---:|---|---|---|
| J1 | **Loading process** — boot arenas, supervise the process trio, hot-swap dylibs with state handoff | `kernel/`, `process/`, `hot/`, `spoke/` | Tick, present, layout, paint |
| J2 | **Event bridge** — macOS happens on thread 0, hotcwap stores it atomically and fires it outward | `window/window_cocoa.m`, `window/window_event.c` | Interpret pixels, own a swapchain, composite |

## 2. The Bridge Pattern (the only three steps)

Every seam follows the same pipeline — macOS thing, bridge, accurate state:

```
during resize {
    macOS thingy  →  NSView live-resize notification lands on thread 0;
                     hotcwap mirrors it into an atomic (liveResizing)
                     and stores the resize hook + generations.
    the bridge    →  consumer (graphvex or whatever) reads
                     Window_isLiveResizing / Window_sizeGeneration /
                     its resizeRenderFn hook — on its own timeline.
    accurate size →  consumer sizes its own surface and presents with
                     transaction = YES, so the frame lands edge-locked.
}
```

Rules of the pattern:

1. **hotcwap stores, never acts.** Setters are atomic stores, getters are
   atomic loads, events fire outward. No callback ever draws.
2. **Thread 0 owns the macOS side.** Anything touching AppKit runs on
   thread 0 (the Window Decoupling Law). The consumer reads the atomics
   from wherever it lives.
3. **Null is a contract, not a crash.** Every seam is null-safe and
   degrades to a safe default (the Cold-Strict, Hot-Minimal Validation
   Law): inert pixel functions return `false`/`nullptr`, events to no
   listener are dropped.
4. **Full API support, behavior-wise.** Key, mouse, touch, focus, plus
   vexspoke system events cross the same bridge shape (attach-by-window-id
   adapters, fire-outward dispatch) so behavior consumers get everything
   and pixel consumers get slots — never pointers into each other.

## 3. Seam Contract Table (what is live, what is inert-by-design)

| Seam | State | Consumer contract |
|---|---|---|
| `WindowEvent` registry (close/focus/resize/move/monitor/occlusion/quit) | LIVE — per-window slots, fire-outward | Attach a listener, get called on thread 0; veto only the quit slot |
| Key/Mouse/Touch adapters (attach by window id) | LIVE — window-scoped routing, zero global listeners | Push native events in, dispatch per window; `window_event_test` proves the registry, `window_test` the dispatch path |
| Container/ContentPanel/ScenePanel slots | LIVE — atomic store/load | Hang your root; hotcwap never walks it |
| Present mode / transparency / generations | LIVE — stored policy + monotonic counters | graphvex sizes swapchains off these, never asks twice |
| Live-resize mirror + resize hook | LIVE — atomic flag + stored fn slot | The §2 pipeline: mirror, read, present-with-transaction |
| `contentView` anchor | LIVE — AppKit view handle | Render repos create their own `CAMetalLayer` on it |
| `attach/resize/compositePanes`, `compositeBoards` | INERT (`false`/no-op, `;;INTENTION`) | Owned by the consumer's own pass; asserted by `bridge_seam_test` |
| `workerPresentBegin/End`, `setGravityTopLeft` | INERT (no-op) | No layer here, nothing to commit; asserted by `bridge_seam_test` |
| `metalLayer` | INERT (`nullptr`) | Degrades the `VK_EXT_metal_surface` path cleanly; asserted by `bridge_seam_test` |
| `Window_present` | INERT (`false`) | A pure AppKit window has no raster target; asserted by `bridge_seam_test` |

Inert is a **promise**, not a gap: the signatures stay so consumers compile
against one stable header, and the test pins the safe defaults so no
consumer ever leans on a pixel hotcwap does not own. Each seam retires with
the darling compositor migration, never one at a time.

## 4. What This Means for the Readiness Matrix

- A 🟩 on a bridge seam means **contract + proof**: the slot stores, the
  event fires, a headless test pins it. Never pixels.
- Platform backends outside macOS (`window_linux.c`, `window.c`, Wayland,
  headless) are tracked in a platform appendix — they do not gate the
  macOS host milestone.
- The remaining feature work is the loading process itself: state handoff
  (§8) and the TUI session proof (§2.7). Pixels are never on this list.
