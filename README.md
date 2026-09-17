# hotcwap. hot-c-wap. R1 Host Supervisor — thin nano-VM.

Zero-downtime dynamic module hot-reloading, persistent OS windowing, and process supervision.

A play on the term **hot swap** — `hotcwap` is an infrastructure runtime designed to reload compiled C23 dynamic libraries in real-time without restarting the process, losing application state, or destroying the native operating system window.

In conventional game architectures, window management and simulation loops are tightly tangled. If a module crashes, reloads, or reconfigures, the window flickers, the graphics device is destroyed, and the event pump restarts. 

`hotcwap` enforces a strict architectural division: **a window belongs to the operating system and Thread 0, while simulation and graphics logic belong to reloadable modules.** By holding the window handle stable in `hotcwap`, modules and shaders can be swapped seamlessly on the fly.

---

## Key Architecture & Strengths

* **OS Window Decoupling**: Thread 0 hosts the native platform window (`AppKit` / Cocoa on macOS; X11/Wayland on Linux). The display link, event pump, and surface layer persist indefinitely across module reloads.
* **Decoupled Window Backend**: A window is a dumb surface + callback bridge (`window/window_cocoa.m`) — pure AppKit, zero Vulkan/Metal. It answers `Window_*` calls from graphvex and R5 apps through the per-window `WindowEvent` lifecycle registry; the GPU-era composite/attach surface is retained as `;;INTENTION` stubs until the darling compositor migrates onto the bridge (the Window Decoupling Law).
* **Microsecond Dynamic Reloader**: Swaps loaded `.dylib` function pointer dispatch tables with zero frame interruption. `Hot_poll` (`hot/hot.c`) compares the manifest's per-library generation stamp (`bin/current/<library>.generation`) against its last-seen generation; on a move it snapshots the running module state on an off-thread save worker (the Bounded Wait Law), then on the next poll dlopens + fail-closed verifies the whole new set (`VkModuleGetTrampolines` on every section), rehydrates the saved state into the STAGED images BEFORE any commit, then atomically swaps trampolines. A new image whose `Hot_restore` rejects its saved state rolls the whole swap back (#8.5 Automated State Rollback) — the old generation stays live and the stamp never advances, so the next poll self-heals once a fixed set is promoted. The install-ladder authority + `manifest.json` catalog (`hot/manifest.h/.c`) gates what lands in `bin/current/<library>` via the `MANIFEST_UPDATE`/`MANIFEST_PROMOTE` verbs — the loader trusts the ladder placement. Vulkan module loading lives in graphvex (`src/vulkan/vk_loader.c`) — hotcwap holds no Vulkan code.
* **Low-Latency Event Pump**: Decoupled polling for keyboard, mouse, and touch in bounded 25ms slices (the Bounded Wait Law), mirrored into the vexspoke input rings per-window.

---

## Workspace Integration & How to Use It

`hotcwap` sits at R1 Host in the `vexgraph` supervisor order (the Vertical Integration Law: `R1 hotcwap > R2 vexspoke > R3 graphvex/api-haven/language/darkbase > R4 darling-framework/sesh > R5 engines`). It boots first as Kernel Host, owns the master + transient arenas and the Application registry, and tears down last — depending only on `vexspoke` shapes + `graphvex` GPU types, never on `darling`/`api-haven`/engines:

```
workspace/
├── cmake-build-debug/           # Out-of-tree CMake build artifacts & staged SPVs
├── projects/                    # Vertically integrated subsystem repositories
│   ├── hotcwap/                 # R1 Host: nano-VM (this library)
│   │   ├── kernel/              # Kernel {arena, transientArena, applications[]} supervisor
│   │   ├── process/             # Process taxonomy: process, application, console
│   │   └── window/              # OS window, AppKit Cocoa bridge, event registry
│   ├── vexspoke/                # R2 Behavior: relational C23 runtime (shapes Kernel borrows)
│   ├── graphvex/                # R3 Driver: GPU compute, SPIR-V, fonts
│   ├── darling-framework/       # R4 Interface: UI tree (registers via Application)
│   ├── api-haven/               # R3 Driver: telemetry/connectors (registers via callbacks)
│   └── [R5 engines register as Applications: vex-engine, mini-ide, daw, ...]
├── CMakeLists.txt               # Umbrella workspace orchestrator
└── preferences.md               # Engine architectural style preferences (Rules 1–n, supreme)
```

### Kernel lifecycle (the 7 steps — test_suite order)

```c
Kernel *k = Kernel();              // 1. kernel: master + transient arenas
Application *app = Application("vex vk probe"); // 2. application
Kernel_addApplication(k, app);     // 3. adding an application in the kernel
Window *w = Window();              // 4. window (OS-owned, never hot-updated)
Application_addWindow(app, w);     // 5. adding a window to the application
Application_run(app);              // 6. run the application (Phase 1: blocking, single-app)
Application_free(app);             // 7a. end the application (detach-only)
Kernel_destroy(k);                 // 7b. end the kernel (stops apps, arenas LAST)
```

### 1. In-Tree Integration (Subdirectory)
When integrated inside an umbrella workspace:

```cmake
# In your top-level CMakeLists.txt
add_subdirectory(projects/hotcwap)

add_executable(my_app spoke.c)
target_link_libraries(my_app PRIVATE hotcwap vexspoke)
```

### 2. Standalone Integration (FetchContent Seam)
When building standalone or in downstream projects:

```cmake
if (NOT TARGET hotcwap)
    include(FetchContent)
    FetchContent_Declare(
            hotcwap
            GIT_REPOSITORY https://github.com/vexgraph-dev/hotcwap.git
            GIT_TAG spoke
    )
    FetchContent_MakeAvailable(hotcwap)
endif ()

target_link_libraries(my_app PRIVATE hotcwap)
```

---

## What's in this repo

* **`kernel/kernel.h/.c`** — R1 Host Supervisor (thin nano-VM): `Kernel {arena, transientArena, applications[KERNEL_MAX_APPS]}`. Boots first, tears down last. Holds opaque Application handles + callbacks, never engine headers.
* **`process/`** — Process taxonomy: `process` (one-shot invocable), `application` (executable identity + window registry + hot-module slot), `console` (tty/session pump).
* **`spoke/lifetime.h/.c`** — R1→R2 memory substrate contract for vexspoke (and vexspoke only): the `Lifetime` struct (master + transient arenas, opaque handles, type-attested). The single seam including vexspoke memory headers — `kernel/` names no vexspoke internal type.
* **`window/window.h`** — Platform-agnostic window abstraction: creation, sizing, fullscreen toggles, input event dispatch, and title management.
* **`window/window_cocoa.m`** — Native macOS AppKit backend (pure AppKit, zero Vulkan/Metal): window lifecycle, event pump, chrome, traffic-light API, per-window `WindowEvent` registry.
* **`window/window_linux.c`** — Linux X11 fallback backend (kept for hosts without Wayland dev libraries).
* **`window/window_wayland.c`** — Lean Linux Wayland draft backend (1:1 cocoa mirror; opt-in via `-DHOTCWAP_USE_WAYLAND=ON`, auto-falls back to X11 when `wayland-client`/`xkbcommon` are absent — `;;DRAFT`, needs a Linux host to compile).
* **`window/window_win32.c`** — Lean Windows Win32 draft backend (1:1 cocoa mirror; the production candidate, with `window.c` kept as the legacy safe-default stub via `-DHOTCWAP_WIN32_LEGACY=ON` — `;;DRAFT`, needs a Windows host to compile).
* **`hot/hot.h/.c`** — Dynamic module reloader: `dlopen`/`dlsym` lifecycle wrappers and runtime state preservation.
* **`hot/manifest.h/.c`** — The `MANIFEST(...)` install-layout authority (the "manifest binary way"): the install tree plus the `manifest.json` library catalog the downloader edits. `MANIFEST(kind, org, app)` resolves `<application-data>/<org>/<app>` once; `MANIFEST_LIBRARY(...)` registers library KEYS; `MANIFEST_UPDATE(library, payloadDir)` fail-closes undeclared payload sections and stages `bin/new/<library>`; `MANIFEST_PROMOTE()` slides each library's generations — the MODE-2 cold-swap ladder beneath the MODE-1 `Hot_poll` hot swap. See `docs/install.md`.

---

## Requirements

* C23 compiler (Clang with `-std=gnu23`).
* macOS (AppKit, Cocoa) or Linux (X11).
* CMake $\ge$ 4.3.
