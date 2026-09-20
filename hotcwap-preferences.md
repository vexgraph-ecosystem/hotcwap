# hotcwap — Repo-Local Living Preferences
> Exclusive repository-level preferences (the Living Preferences Law).
> Universal Supreme Constitution: preferences.md (vexspoke).

;;SYNC("mirrors ecosystem/vexspoke/preferences.md @ 2026.09-universal")

## 0. Constitution Link (supreme)
- [preferences.md](https://github.com/vexgraph-dev/vexspoke/blob/main/preferences.md) (canonical, vexspoke) — accessible locally at ../../preferences.md
- All universal laws in `preferences.md` are mandatory and binding across the ecosystem.
- This document codifies **exclusive** preferences that apply uniquely to `hotcwap` (R1 Kernel Host).

## 1. Exclusive Preferences Binding Matrix

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **Two-Layer Split Window Architecture Law** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Present-On-Demand Law (composite ≠ render)** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Dynamic Module ABI Verification Law** | R1 Kernel Host | Mandatory for `hotcwap` |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### Two-Layer Split Window Architecture Law

#### Definition:
The host window composite is strictly partitioned into two layers: the bottom layer is the direct Vulkan swapchain surface (or transparent `NSVisualEffectView` blur substrate), and the top layer is composed of hardware `CALayer`s backed by zero-copy `IOSurface` allocations.

#### The Why:
Mixing UI presentation directly into the Vulkan render loop creates swapchain contention and forces full-frame redraws during lightweight UI interactions. Splitting swapchain graphics from native composited layers allows independent frame cadences and zero-latency window management.

#### The Rule:
1. **Bottom Layer:** Swapchain or window blur only; no UI widgets render directly to the background swapchain.
2. **Top Layer:** `CALayer` hierarchy hosted via Objective-C AppKit bridge with `geometryFlipped = YES`.

---

### Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")

Freezing swapchain extents, dropping `VK_ERROR_OUT_OF_DATE_KHR` frames, and
early-returning from layout during mouse drags (`Window_isLiveResizing`) is
**strictly abolished**. Deferring work to "settle" is an artificial cop-out
that produces frozen windows, dead animations, and visual tearing. During an
active window drag or live resize, the rendering pipeline operates
continuously:

1. **Dynamic Extent & Swapchain:** `CAMetalLayer.drawableSize` tracks live
   window bounds on every resize event. Swapchain out-of-date events
   immediately rebuild the swapchain cleanly without dropping frames.
2. **Live Layout Recalculation = Real-Time Anchor Feel:** Container layout and
   anchor resolution run on live bounds every frame of the drag — pinned
   elements (e.g., right-anchored, bottom-anchored) recalculate their offsets
   dynamically and re-present, so they stay glued to their edges in real time.
   The resolution granularity is one vsync (60/120Hz), which IS the native
   contract: CoreAnimation itself commits per frame, so per-frame latency is
   indistinguishable from a native view. Nothing is ever stale beyond the
   latency of a single frame. The sub-frame gap between a resize event and
   the next frame is covered by the instantly-moved layer frame (the
   Single-Transaction Live Coordination Law) plus the gravity-pinned last
   bitmap — never black, never torn, never stretched-out-of-anchor.
3. **Unbroken Animation & Presentation:** Animation tickers,
   dirty-propagation, and command buffer presentation
   (`presentsWithTransaction = YES`) continue rendering and presenting at the
   display's native refresh rate (60/120Hz) throughout mouse drags and moves.

---

### Present-On-Demand Law (composite ≠ render)

A surface presents only on demand. Demand is change: motion, layout, text,
hover, or a scene publishing a new frame. Anything static presents once and
then rests on its last composite — the compositor never re-presents clean
content and never re-invokes a scene's render handler.

#### The Why (subsumes the former No Double-Render Law)
The old law forbade stamping one panel into two chains. Present-on-demand
makes that impossible by construction: a scene RENDER (its world into its
retained offscreen target — `VkLayer`, own thread, own FPS) is distinct from
a COMPOSITE (sample published targets + paint the UI tree into the canvas at
vsync). The canvas painter only samples published targets — it cannot
re-render a scene. Double-render is structurally unreachable, not merely
forbidden.

#### Present modes (per scene):
- `COMPOSITED` (default): the scene owns retained flight render targets
  (offscreen images + acquire/render semaphores + fences) on its own
  timeline; the canvas samples the latest published frame at the anchor rect,
  in tree z-order interleaved with UI. One canvas total — no per-scene
  surfaces.
- `DIRECT` (managed exception, the Conflict Triage Law): a scene may own its
  own `CAMetalLayer` + swapchain (`VkPane`) and present at its own pace —
  full-window or latency-locked scenes that must not pay the composite copy.

#### Composite rules:
- The presenter wakes only on demand: a dirty tree, a published layer frame,
  or a live-resize drag (the moving edge is itself a ticket). An idle tree
  sleeps — zero presents, zero GPU work; power is the free win.
- UI paints the FULL tree on any dirty tick (immediate-on-demand; damage
  rects are a later optimization, never a first move). Static content rests:
  the canvas is neither re-acquired nor presented.
- Scene anchors are plain resolved C rects into the canvas; WindowServer-
  native anchoring (`presentsWithTransaction`, `autoresizingMask`) lives on
  the single canvas layer only.

---

### Dynamic Module ABI Verification Law

#### Definition:
Dynamic dylibs loaded during hot-reload passes must verify their ABI compatibility header before binding function trampolines. The loader inspects ABI version, class registry hashes, and exported symbol alignments. Retiring modules cycle through ABA-safe retirement rings.

#### The Why:
Live reloading without ABI validation causes memory misalignment and crashes when struct fields change between compilations. Validating before swapping guarantees zero runtime crashes during live iteration.

#### The Rule:
1. **Pre-Flight Verification:** ABI headers are checked before swapping function pointers.
2. **Graceful Rejection:** Incompatible dynamic modules fail cleanly, leaving the previous working version active.

---

## 3. Repo-Local Extensions (managed, per the Conflict Triage Law)

;;INTENTION("R1 Kernel Host: native AppKit/Metal bridge; two-layer split (swapchain bottom, CALayer IOSurface top); dynamic dylib hotloading.")

---

## 4. Readiness Cross-Reference (the Living Feature Readiness Law)

- Feature readiness matrix tracked in [`../../_repositories/.ecosystem/hotcwap.md`](../../_repositories/.ecosystem/hotcwap.md) (rendered as `[[hotcwap]]` wiki page).
