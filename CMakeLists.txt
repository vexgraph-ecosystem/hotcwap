cmake_minimum_required(VERSION 3.28)
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
    window/window_event.c
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
    )
    set_source_files_properties(window/window_cocoa.m PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(hotcwap PUBLIC
        "-framework Cocoa"
        "-framework AppKit"
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
if(NOT TARGET spoke_test AND EXISTS "${HOTCWAP_TEST_DIR}/spoke_test.c" AND TARGET vexspoke)
    add_executable(spoke_test
        spoke/lifetime.c
        ${HOTCWAP_TEST_DIR}/spoke_test.c
    )
    target_include_directories(spoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(spoke_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(spoke_test PRIVATE vexspoke)
endif()

if(NOT TARGET window_test AND EXISTS "${HOTCWAP_TEST_DIR}/window_test.c")
    add_executable(window_test
        ${HOTCWAP_TEST_DIR}/window_test.c
    )
    target_include_directories(window_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(window_test PRIVATE hotcwap)
    target_compile_options(window_test PRIVATE -Wall -Wextra -Werror)
endif()

# Headless WindowEvent registry test: symmetric get/set + fire dispatch + the
# single vetoable quit slot. Never touches the OS window backend, so it runs
# on every platform with zero surfaces.
if(NOT TARGET window_event_test AND EXISTS "${HOTCWAP_TEST_DIR}/window_event_test.c")
    add_executable(window_event_test
        ${HOTCWAP_TEST_DIR}/window_event_test.c
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
if(NOT TARGET manifest_hot_test AND EXISTS "${HOTCWAP_TEST_DIR}/manifest_hot_test.c")
    add_executable(manifest_hot_test
        ${HOTCWAP_TEST_DIR}/manifest_hot_test.c
    )
    target_include_directories(manifest_hot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(manifest_hot_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(manifest_hot_test PRIVATE
        HOT_BEHAVIOR_MODULE="$<TARGET_FILE:hot_behavior>")
    target_link_libraries(manifest_hot_test PRIVATE hotcwap)
    add_dependencies(manifest_hot_test hot_behavior)
endif()

# #8.5 automated-state-rollback end-to-end test: generation 2 is a
# foreign-magic hot_behavior build whose Hot_restore REJECTS generation 1's
# state blob. The loader must rehydrate BEFORE commit, roll the whole swap
# back (generation 1 + its set phaseBias stay live), surface
# HOT_ERROR_RESTORE_FAILED without advancing the stamp, reject the bad set
# again on the next poll, and heal on a subsequent good promote (gen 3).
if(NOT TARGET manifest_rollback_test AND EXISTS "${HOTCWAP_TEST_DIR}/manifest_rollback_test.c")
    add_executable(manifest_rollback_test
        ${HOTCWAP_TEST_DIR}/manifest_rollback_test.c
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
if(NOT TARGET bridge_seam_test AND EXISTS "${HOTCWAP_TEST_DIR}/bridge_seam_test.c")
    add_executable(bridge_seam_test
        ${HOTCWAP_TEST_DIR}/bridge_seam_test.c
    )
    target_include_directories(bridge_seam_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(bridge_seam_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(bridge_seam_test PRIVATE hotcwap)
endif()

if(NOT TARGET kernel_function_test AND EXISTS "${HOTCWAP_TEST_DIR}/kernel_function_test.c")
    add_executable(kernel_function_test
        ${HOTCWAP_TEST_DIR}/kernel_function_test.c
    )
    target_include_directories(kernel_function_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(kernel_function_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(kernel_function_test PRIVATE hotcwap)
endif()

