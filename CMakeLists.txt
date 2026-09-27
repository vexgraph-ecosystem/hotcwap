cmake_minimum_required(VERSION 3.28)

# Platform Support Floor Law: Apple Silicon only (arm64), macOS 14.0+.
# Cache entries set before project() so a standalone build enforces the same
# floor as the umbrella. A user -D override is preserved.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Platform Support Floor Law: Apple Silicon only (arm64)")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Platform Support Floor Law: macOS 14.0+")
endif()

project(hotcwap C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(APPLE)
    enable_language(OBJC)
    set(CMAKE_OBJC_STANDARD 23)
    set(CMAKE_OBJC_STANDARD_REQUIRED ON)
endif()

# Release: dead-code stripping so unused symbols cost zero bytes in artifacts.
# (Duplicated per repo for standalone builds; harmless when built in-tree.)
if(APPLE)
    add_compile_options($<$<CONFIG:Release>:-ffunction-sections> $<$<CONFIG:Release>:-fdata-sections>)
    add_link_options($<$<CONFIG:Release>:-Wl,-dead_strip>)
elseif(UNIX)
    add_compile_options($<$<CONFIG:Release>:-ffunction-sections> $<$<CONFIG:Release>:-fdata-sections>)
    add_link_options($<$<CONFIG:Release>:-Wl,--gc-sections>)
endif()

if(NOT TARGET hotcwap)
# The OS-stable layer: hot-reload machinery + the OS-agnostic window.
# A window never hot-updates — it is owned by the OS — so it lives here,
# next to the loader that persists across module swaps.
#
# NOTE: Vulkan core (instance/device/swapchain/pane/surface, including the
# vk_loader bridge) has moved to graphvex. hotcwap holds no Vulkan code and
# owns only the OS window + event registry seam that graphvex presents into.
add_library(hotcwap STATIC
        kernel/application.c
        kernel/process.c
        kernel/console.c
    kernel/kernel.c
    spoke/lifetime.c
    hot/hot.c
    hot/hot_trampoline.c
    hot/hot_retire.c
    hot/manifest.c
    hot/ledger.c
    hot/throwable.c
    window/window_event.c
    permission/permission.c
    capability/capability.c
)
target_include_directories(hotcwap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(hotcwap PRIVATE -Wall -Wextra -Werror)

# Phase-1 L2 behavior subject: pure pulse/bar math + texture path.
# Host handlers delegate here; module holds no GPU/Panel state so swap is
# safe by construction. Output lands in ${CMAKE_BINARY_DIR}/modules; the
# manifest ladder (MANIFEST_REFLECT/UPDATE/PROMOTE) and manifest_hot_test
# REPLACE the retired ${CMAKE_BINARY_DIR}/hot watch dir.
add_library(hot_behavior MODULE hot/hot_behavior.c)
target_include_directories(hot_behavior PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../vexspoke/src)
target_compile_options(hot_behavior PRIVATE -Wall -Wextra -Werror)
set_target_properties(hot_behavior PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules")

# #8.5 rollback seam: the SAME source reconfigured with a foreign schema
# magic — a module whose Hot_restore rejects the canonical state blob. The
# manifest_rollback_test promotes this build as generation 2 and asserts the
# loader rolls the swap back (old generation stays live, no stamp advance)
# instead of adopting it. OUTPUT_NAME collides with hot_behavior, so it lands
# in its own modules_bad directory.
add_library(hot_behavior_bad MODULE hot/hot_behavior.c)
target_include_directories(hot_behavior_bad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../vexspoke/src)
target_compile_options(hot_behavior_bad PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(hot_behavior_bad PRIVATE
    HOT_BEHAVIOR_SCHEMA_MAGIC=0xBADC0DE5u)
set_target_properties(hot_behavior_bad PROPERTIES
    OUTPUT_NAME hot_behavior
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules_bad"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules_bad")

# Check if vexspoke is already available as a target (parent build)
if(NOT TARGET vexspoke)
    option(HOTCWAP_USE_VEXSPOKE "Depend on vexspoke core via FetchContent" OFF)
    if(HOTCWAP_USE_VEXSPOKE)
        include(FetchContent)
        FetchContent_Declare(
            vexspoke
            GIT_REPOSITORY https://github.com/vexgraph-dev/vexspoke.git
            GIT_TAG main
        )
        FetchContent_MakeAvailable(vexspoke)
        target_include_directories(hotcwap PUBLIC ${vexspoke_SOURCE_DIR}/src)
    endif()
endif()

# Upstream graphics foundation: the Buffer family lives in graphvex since the
# move down from vexspoke, so standalone hotcwap needs the same seam here.
# In-tree the TARGET guard below suffices; the PUBLIC link re-exports
# graphvex/src so "buffer/..." keeps resolving downstream (Rule 18).
if(NOT TARGET graphvex)
    option(HOTCWAP_USE_GRAPHVEX "Depend on graphvex via FetchContent" OFF)
    if(HOTCWAP_USE_GRAPHVEX)
        include(FetchContent)
        if(NOT TARGET vexspoke)
            FetchContent_Declare(
                vexspoke
                GIT_REPOSITORY https://github.com/vexgraph-dev/vexspoke.git
                GIT_TAG main
            )
        endif()
        FetchContent_Declare(
            graphvex
            GIT_REPOSITORY https://github.com/vexgraph-dev/graphvex.git
            GIT_TAG main
        )
        FetchContent_MakeAvailable(vexspoke graphvex)
    endif()
endif()

if(TARGET vexspoke)
    target_link_libraries(hotcwap PUBLIC vexspoke)
endif()
if(TARGET graphvex)
    target_link_libraries(hotcwap PUBLIC graphvex)
endif()

# OS window backend: the fresh AppKit window backend on macOS (pure AppKit,
# zero Vulkan/Metal — a window with bridges per the Window Decoupling Law).
# On Windows the lean Win32 draft (window/window_win32.c) is the production
# candidate; the legacy safe-default stub (window.c) stays one flag away.
# On Linux the lean Wayland draft (window/window_wayland.c) is opt-in and
# requires the wayland-client + xkbcommon dev libraries; without them (or the
# flag) the X11 fallback (window_linux.c) builds.
# NOTE: both drafts are compiled only on their own host — the macOS build
# never sees them, keeping the umbrella AppKit trail green.
if(APPLE)
    target_sources(hotcwap PRIVATE
            window/window_cocoa.m
            window/traffic_light_cocoa.m
            permission/objc/permission_cocoa.m
    )
    set_source_files_properties(window/window_cocoa.m window/traffic_light_cocoa.m permission/objc/permission_cocoa.m PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(hotcwap PUBLIC
        "-framework Cocoa"
        "-framework AppKit"
        "-framework AVFoundation"
        "-framework Contacts"
        "-framework EventKit"
        "-framework Photos"
        "-framework CoreLocation"
        "-framework UserNotifications"
        "-framework CoreServices"
    )
elseif(WIN32)
    option(HOTCWAP_WIN32_LEGACY "Use the legacy Win32 safe-default stub (window.c) instead of the lean draft" OFF)
    if(HOTCWAP_WIN32_LEGACY)
        target_sources(hotcwap PRIVATE window/window.c)
    else()
        target_sources(hotcwap PRIVATE window/window_win32.c)
        target_link_libraries(hotcwap PUBLIC user32 gdi32 dwmapi)
    endif()
elseif(UNIX)
    option(HOTCWAP_USE_WAYLAND "Build the lean Wayland draft (window_wayland.c); auto-falls back to window_linux.c when the libraries are absent" OFF)
    if(HOTCWAP_USE_WAYLAND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(WAYLAND QUIET IMPORTED_TARGET wayland-client xkbcommon)
        endif()
        if(WAYLAND_FOUND)
            target_sources(hotcwap PRIVATE window/window_wayland.c)
            target_link_libraries(hotcwap PUBLIC PkgConfig::WAYLAND)
        else()
            message(WARNING "HOTCWAP_USE_WAYLAND=ON but wayland-client/xkbcommon not found — falling back to window_linux.c")
            target_sources(hotcwap PRIVATE window/window_linux.c)
        endif()
    else()
        target_sources(hotcwap PRIVATE window/window_linux.c)
    endif()
endif()
endif() # NOT TARGET hotcwap

set(HOTCWAP_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/hotcwap")
if(NOT EXISTS "${HOTCWAP_TEST_DIR}")
    set(HOTCWAP_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
endif()


# Headless spoke bridge test: static fill wires every slot, arena lifecycle
# through the opaque table with type attestation. Needs the real vexspoke
# symbols, so it builds only where the vexspoke target exists.
if(NOT TARGET spoke_test AND EXISTS "${HOTCWAP_TEST_DIR}/spoke/spoke_test.c" AND TARGET vexspoke)
    add_executable(spoke_test
        spoke/lifetime.c
        ${HOTCWAP_TEST_DIR}/spoke/spoke_test.c
    )
    target_include_directories(spoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(spoke_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(spoke_test PRIVATE vexspoke)
endif()

if(NOT TARGET window_test AND EXISTS "${HOTCWAP_TEST_DIR}/window/window_test.c")
    add_executable(window_test
        ${HOTCWAP_TEST_DIR}/window/window_test.c
    )
    target_include_directories(window_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(window_test PRIVATE hotcwap)
    target_compile_options(window_test PRIVATE -Wall -Wextra -Werror)
endif()

# Headless WindowEvent registry test: symmetric get/set + fire dispatch + the
# single vetoable quit slot. Never touches the OS window backend, so it runs
# on every platform with zero surfaces.
if(NOT TARGET window_event_test AND EXISTS "${HOTCWAP_TEST_DIR}/window/window_event_test.c")
    add_executable(window_event_test
        ${HOTCWAP_TEST_DIR}/window/window_event_test.c
    )
    target_include_directories(window_event_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(window_event_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(window_event_test PRIVATE hotcwap)
endif()

# End-to-end manifest-ladder hot reload: REFLECT (gen 1) → load → UPDATE +
# PROMOTE (gen 2) → two-poll generation-driven swap. Builds its own scratch
# $VEX_MANIFEST and copies hot_behavior.so into a payload dir, so it exercises
# the real loader (hot.c) against the real ladder, not a unit stub. Passed the
# built module path as HOT_BEHAVIOR_MODULE.
if(NOT TARGET manifest_hot_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/manifest_hot_test.c")
    add_executable(manifest_hot_test
        ${HOTCWAP_TEST_DIR}/hot/manifest_hot_test.c
    )
    target_include_directories(manifest_hot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(manifest_hot_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(manifest_hot_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>")
    target_link_libraries(manifest_hot_test PRIVATE hotcwap)
    add_dependencies(manifest_hot_test hot_behavior)
endif()

# Manifest adversarial battery: path/name gate, corrupt-manifest no-clobber,
# create-if-empty, and the library key gate. Headless, no dylibs.
if(NOT TARGET manifest_adversarial_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/manifest_adversarial_test.c")
    add_executable(manifest_adversarial_test
        ${HOTCWAP_TEST_DIR}/hot/manifest_adversarial_test.c
    )
    target_include_directories(manifest_adversarial_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(manifest_adversarial_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(manifest_adversarial_test PRIVATE hotcwap)
endif()

# Destructive-target guard: MANIFEST()/UNINSTALL must refuse protected or
# non-owned roots (the uninstall safety net). Headless, POSIX-only.
if(NOT TARGET uninstall_guard_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/uninstall_guard_test.c")
    add_executable(uninstall_guard_test
        ${HOTCWAP_TEST_DIR}/hot/uninstall_guard_test.c
    )
    target_include_directories(uninstall_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(uninstall_guard_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(uninstall_guard_test PRIVATE hotcwap)
endif()

# Install ledger: the machine-scoped, out-of-tree record (macOS Keychain).
# Skips cleanly where the secure store is unavailable.
if(NOT TARGET ledger_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/ledger_test.c")
    add_executable(ledger_test
        ${HOTCWAP_TEST_DIR}/hot/ledger_test.c
    )
    target_include_directories(ledger_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(ledger_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(ledger_test PRIVATE hotcwap)
endif()

# Two real dylibs in one library: load both sections, swap both together.
if(NOT TARGET two_dylib_swap_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/two_dylib_swap_test.c")
    add_executable(two_dylib_swap_test
        ${HOTCWAP_TEST_DIR}/hot/two_dylib_swap_test.c
    )
    target_include_directories(two_dylib_swap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(two_dylib_swap_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(two_dylib_swap_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>")
    target_link_libraries(two_dylib_swap_test PRIVATE hotcwap)
    add_dependencies(two_dylib_swap_test hot_behavior)
endif()

# A wrong (non-loadable) promoted section is refused and rolled back.
if(NOT TARGET wrong_binary_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/wrong_binary_test.c")
    add_executable(wrong_binary_test
        ${HOTCWAP_TEST_DIR}/hot/wrong_binary_test.c
    )
    target_include_directories(wrong_binary_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(wrong_binary_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(wrong_binary_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>")
    target_link_libraries(wrong_binary_test PRIVATE hotcwap)
    add_dependencies(wrong_binary_test hot_behavior)
endif()

# Retire-ring overflow: more swaps than the 16-slot ring holds.
if(NOT TARGET retire_ring_overflow_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/retire_ring_overflow_test.c")
    add_executable(retire_ring_overflow_test
        ${HOTCWAP_TEST_DIR}/hot/retire_ring_overflow_test.c
    )
    target_include_directories(retire_ring_overflow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(retire_ring_overflow_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(retire_ring_overflow_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>")
    target_link_libraries(retire_ring_overflow_test PRIVATE hotcwap)
    add_dependencies(retire_ring_overflow_test hot_behavior)
endif()

# Shutdown ordering: bounded-join the save worker before closing handles.
if(NOT TARGET shutdown_order_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/shutdown_order_test.c")
    add_executable(shutdown_order_test
        ${HOTCWAP_TEST_DIR}/hot/shutdown_order_test.c
    )
    target_include_directories(shutdown_order_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(shutdown_order_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(shutdown_order_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>")
    target_link_libraries(shutdown_order_test PRIVATE hotcwap)
    add_dependencies(shutdown_order_test hot_behavior)
endif()

# Loader trust boundary: path/name gate + confined scan.
if(NOT TARGET loader_trust_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/loader_trust_test.c")
    add_executable(loader_trust_test
        ${HOTCWAP_TEST_DIR}/hot/loader_trust_test.c
    )
    target_include_directories(loader_trust_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(loader_trust_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(loader_trust_test PRIVATE hotcwap)
endif()

# #8.5 automated-state-rollback end-to-end test: generation 2 is a
# foreign-magic hot_behavior build whose Hot_restore REJECTS generation 1's
# state blob. The loader must rehydrate BEFORE commit, roll the whole swap
# back (generation 1 + its set phaseBias stay live), surface
# HOT_ERROR_RESTORE_FAILED without advancing the stamp, reject the bad set
# again on the next poll, and heal on a subsequent good promote (gen 3).
if(NOT TARGET manifest_rollback_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/manifest_rollback_test.c")
    add_executable(manifest_rollback_test
        ${HOTCWAP_TEST_DIR}/hot/manifest_rollback_test.c
    )
    target_include_directories(manifest_rollback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(manifest_rollback_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(manifest_rollback_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>"
        HOT_BEHAVIOR_BAD_MODULE="$<TARGET_FILE:hot_behavior_bad>")
    target_link_libraries(manifest_rollback_test PRIVATE hotcwap)
    add_dependencies(manifest_rollback_test hot_behavior hot_behavior_bad)
endif()

# Inert pixel-seam contract test (docs/bridging.md #3): pins the safe
# defaults (false/nullptr/no-op) of every function hotcwap keeps inert by
# design. Links hotcwap for the per-OS backend; vexspoke input symbols
# resolve transitively in-tree exactly like window_test.
if(NOT TARGET bridge_seam_test AND EXISTS "${HOTCWAP_TEST_DIR}/window/bridge_seam_test.c")
    add_executable(bridge_seam_test
        ${HOTCWAP_TEST_DIR}/window/bridge_seam_test.c
    )
    target_include_directories(bridge_seam_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(bridge_seam_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(bridge_seam_test PRIVATE hotcwap)
endif()

if(NOT TARGET process_test AND EXISTS "${HOTCWAP_TEST_DIR}/kernel/process_test.c")
    add_executable(process_test ${HOTCWAP_TEST_DIR}/kernel/process_test.c)
    target_compile_options(process_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(process_test PRIVATE hotcwap)
endif()

if(NOT TARGET kernel_lifecycle_test AND EXISTS "${HOTCWAP_TEST_DIR}/kernel/kernel_lifecycle_test.c")
    add_executable(kernel_lifecycle_test ${HOTCWAP_TEST_DIR}/kernel/kernel_lifecycle_test.c)
    target_compile_options(kernel_lifecycle_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(kernel_lifecycle_test PRIVATE hotcwap)
endif()

if(NOT TARGET kernel_function_test AND EXISTS "${HOTCWAP_TEST_DIR}/kernel/kernel_function_test.c")
    add_executable(kernel_function_test
        ${HOTCWAP_TEST_DIR}/kernel/kernel_function_test.c
    )
    target_include_directories(kernel_function_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(kernel_function_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(kernel_function_test PRIVATE hotcwap)
endif()

if(NOT TARGET manifest_uninstall_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/manifest_uninstall_test.c")
    add_executable(manifest_uninstall_test
        ${HOTCWAP_TEST_DIR}/hot/manifest_uninstall_test.c
    )
    target_include_directories(manifest_uninstall_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(manifest_uninstall_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(manifest_uninstall_test PRIVATE hotcwap)
endif()

if(NOT TARGET throwable_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/throwable_test.c")
    add_executable(throwable_test
        ${HOTCWAP_TEST_DIR}/hot/throwable_test.c
    )
    target_include_directories(throwable_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(throwable_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(throwable_test PRIVATE hotcwap)
endif()

# ============================================================================
# Per-file owner tests (the Test Tree Mirror Law): remaining production units
# not yet covered by a ledger seam above.
# ============================================================================

# hot/hot_trampoline.c — the per-instance atomic dispatch table.
if(NOT TARGET hot_trampoline_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/hot_trampoline_test.c")
    add_executable(hot_trampoline_test
        ${HOTCWAP_TEST_DIR}/hot/hot_trampoline_test.c
    )
    target_include_directories(hot_trampoline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(hot_trampoline_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(hot_trampoline_test PRIVATE hotcwap)
endif()

# hot/hot_retire.c — the generational dlclose grace ring.
if(NOT TARGET hot_retire_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/hot_retire_test.c")
    add_executable(hot_retire_test
        ${HOTCWAP_TEST_DIR}/hot/hot_retire_test.c
    )
    target_include_directories(hot_retire_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(hot_retire_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(hot_retire_test PRIVATE hotcwap)
endif()

# hot/hot.c — the loader's cold-seam degradation contract.
if(NOT TARGET hot_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/hot_test.c")
    add_executable(hot_test
        ${HOTCWAP_TEST_DIR}/hot/hot_test.c
    )
    target_include_directories(hot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(hot_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(hot_test PRIVATE hotcwap)
endif()

# hot/hot_behavior.c — the reloadable behavior module, via dlopen.
if(NOT TARGET hot_behavior_test AND EXISTS "${HOTCWAP_TEST_DIR}/hot/hot_behavior_test.c")
    add_executable(hot_behavior_test
        ${HOTCWAP_TEST_DIR}/hot/hot_behavior_test.c
    )
    target_include_directories(hot_behavior_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(hot_behavior_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(hot_behavior_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>")
    target_link_libraries(hot_behavior_test PRIVATE hotcwap)
    add_dependencies(hot_behavior_test hot_behavior)
endif()

# kernel/console.c — the session state machine over an injected IO seam.
if(NOT TARGET console_test AND EXISTS "${HOTCWAP_TEST_DIR}/kernel/console_test.c")
    add_executable(console_test
        ${HOTCWAP_TEST_DIR}/kernel/console_test.c
    )
    target_include_directories(console_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(console_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(console_test PRIVATE hotcwap)
endif()

# kernel/application.c — the executable manifest + window registry.
if(NOT TARGET application_test AND EXISTS "${HOTCWAP_TEST_DIR}/kernel/application_test.c")
    add_executable(application_test
        ${HOTCWAP_TEST_DIR}/kernel/application_test.c
    )
    target_include_directories(application_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(application_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(application_test PRIVATE hotcwap)
endif()

# window/traffic_light_cocoa.m — the macOS traffic-light chrome controller.
if(APPLE AND NOT TARGET traffic_light_test AND EXISTS "${HOTCWAP_TEST_DIR}/window/traffic_light_test.c")
    add_executable(traffic_light_test
        ${HOTCWAP_TEST_DIR}/window/traffic_light_test.c
    )
    target_include_directories(traffic_light_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(traffic_light_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(traffic_light_test PRIVATE hotcwap)
endif()

# permission/permission.c — the R1 OS-consent surface (screen/key/accessibility).
if(NOT TARGET permission_test AND EXISTS "${HOTCWAP_TEST_DIR}/permission/permission_test.c")
    add_executable(permission_test
        ${HOTCWAP_TEST_DIR}/permission/permission_test.c
    )
    target_include_directories(permission_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(permission_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(permission_test PRIVATE hotcwap)
endif()

# capability/capability.c — the R1 host capability probe (the Capability Gating Law).
if(NOT TARGET capability_test AND EXISTS "${HOTCWAP_TEST_DIR}/capability/capability_test.c")
    add_executable(capability_test
        ${HOTCWAP_TEST_DIR}/capability/capability_test.c
    )
    target_include_directories(capability_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(capability_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(capability_test PRIVATE hotcwap)
endif()

