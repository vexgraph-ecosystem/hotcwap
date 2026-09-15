# hotcwap. hot-c-wap. R0 Kernel Host — thin nano-VM.

Zero-downtime dynamic module hot-reloading, persistent OS windowing, and process supervision.

A play on the term **hot swap** — `hotcwap` is an infrastructure runtime designed to reload compiled C23 dynamic libraries in real-time without restarting the process, losing application state, or destroying the native operating system window.

In conventional game architectures, window management and simulation loops are tightly tangled. If a module crashes, reloads, or reconfigures, the window flickers, the graphics device is destroyed, and the event pump restarts. 

`hotcwap` enforces a strict architectural division: **a window belongs to the operating system and Thread 0, while simulation and graphics logic belong to reloadable modules.** By holding the window handle stable in `hotcwap`, modules and shaders can be swapped seamlessly on the fly.

---

## Key Architecture & Strengths

* **OS Window Decoupling**: Thread 0 hosts the native platform window (`AppKit` / Cocoa on macOS; X11/Wayland on Linux). The display link, event pump, and surface layer persist indefinitely across module reloads.
* **Decoupled Window Backend**: A window is a dumb surface + callback bridge (`window/window_cocoa.m`) — pure AppKit, zero Vulkan/Metal. It answers `Window_*` calls from graphvex and R5 apps through the per-window `WindowEvent` lifecycle registry; the GPU-era composite/attach surface is retained as `;;INTENTION` stubs until the darling compositor migrates onto the bridge (the Window Decoupling Law).
* **Microsecond Dynamic Reloader**: Monitors file manifests and filesystem timestamps (`hot/manifest.c`, `hot/hot.c`) to detect newly built dynamic libraries (`.dylib`), swap function pointer dispatch tables, and rebind entry points with zero frame interruption.
* **Vulkan GPA Loader**: Integrated `vkGetInstanceProcAddr` dynamic loader (`hot/vk_loader.c`) that extracts Vulkan symbols dynamically without requiring hard linkage to external loader stubs.
* **Low-Latency Event Pump**: Decoupled polling for keyboard, mouse, and touch in bounded 25ms slices (the Bounded Wait Law), mirrored into the vexspoke input rings per-window.

---

## Workspace Integration & How to Use It

`hotcwap` sits at R0 in the `vexgraph` supervisor order (Rule 17: `R0 hotcwap > R1 vexspoke > R1.5 graphvex > R2 features > R3 engines`). It boots first as Kernel Host, owns the master + transient arenas and the Application registry, and tears down last — depending only on `vexspoke` shapes + `graphvex` GPU types, never on `darling`/`api-haven`/engines:

```
workspace/
├── cmake-build-debug/           # Out-of-tree CMake build artifacts & staged SPVs
├── projects/                    # Vertically integrated subsystem repositories
│   ├── hotcwap/                 # R0 Kernel Host: nano-VM (this library)
│   │   ├── kernel/              # Kernel {arena, transientArena, applications[]} supervisor
│   │   ├── app/                 # Application {CLI/TUI/GUI} + windows[APP_MAX_WINDOWS]
│   │   └── window/              # OS window, AppKit Cocoa bridge, loader
│   ├── vexspoke/                # R1 Spoke: relational C23 runtime (shapes Kernel borrows)
│   ├── graphvex/                # R1.5 GPU compute, SPIR-V, fonts
│   ├── darling/                 # R2 feature: UI tree (registers via Application)
│   ├── api-haven/               # R2 feature: telemetry (registers via callbacks)
│   └── [R3 engines register as Applications: vex-engine, mini-ide, daw, ...]
├── CMakeLists.txt               # Umbrella workspace orchestrator
└── preferences.md               # Engine architectural style preferences (Rules 1–n, supreme)
```

### Kernel lifecycle (the 7 steps — vk_test order)

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

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE hotcwap vexspoke)
```

### 2. Standalone Integration (FetchContent Seam)
When building standalone or in downstream projects:

```cmake
if(NOT TARGET hotcwap)
    include(FetchContent)
    FetchContent_Declare(
        hotcwap
        GIT_REPOSITORY https://github.com/vexgraph-dev/hotcwap.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(hotcwap)
endif()

target_link_libraries(my_app PRIVATE hotcwap)
```

---

## What's in this repo

* **`kernel/kernel.h/.c`** — R0 Host Supervisor (thin nano-VM): `Kernel {arena, transientArena, applications[KERNEL_MAX_APPS]}`. Boots first, tears down last. Holds opaque Application handles + callbacks, never engine headers.
* **`process/`** — Process taxonomy: `process` (one-shot invocable), `application` (executable identity + window registry + hot-module slot), `console` (tty/session pump).
* **`window/window.h/.c`** — Platform-agnostic window abstraction: creation, sizing, fullscreen toggles, input event dispatch, and title management.
* **`window/window_cocoa.m`** — Native macOS AppKit backend (pure AppKit, zero Vulkan/Metal): window lifecycle, event pump, chrome, traffic-light API, per-window `WindowEvent` registry.
* **`window/window_linux.c`** — Linux X11/Wayland display backend.
* **`hot/hot.h/.c`** — Dynamic module reloader: `dlopen`/`dlsym` lifecycle wrappers and runtime state preservation.
* **`hot/manifest.h/.c`** — Dynamic file manifest tracker and change detector.
* **`hot/vk_loader.c`** — Dynamic MoltenVK/Vulkan symbol loader and GPA function table generator.
* **`hot/vk_module.c`** — Hot-reloadable Vulkan pipeline module bindings.
* **`main/vk_test.c`** & **`tests/window_test.c`** — Verification test harnesses for Cocoa window creation, event polling, and dynamic library swapping.

---

## Requirements

* C23 compiler (Clang with `-std=gnu23`).
* macOS (AppKit, Cocoa, QuartzCore, Metal) or Linux (X11).
* CMake $\ge$ 4.3.
* Vulkan SDK / MoltenVK headers.
