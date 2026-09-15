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
    process/application.c
    process/process.c
    process/console.c
    kernel/kernel.c
    spoke/vexspoke.c
    hot/hot.c
    hot/hot_trampoline.c
    hot/hot_retire.c
    hot/manifest.c
    hot/manifest_path.c
    hot/stage.c
    window/window_event.c
)
target_include_directories(hotcwap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(hotcwap PRIVATE -Wall -Wextra -Werror)

# Phase-1 L2 behavior subject: pure pulse/bar math + texture path.
# Host handlers delegate here; module holds no GPU/Panel state so swap is
# safe by construction. Output lands in ${CMAKE_BINARY_DIR}/hot next to the
# umbrella test_suite's watch dir; copy to ./hot/ when running from source.
add_library(hot_behavior MODULE hot/hot_behavior.c)
target_include_directories(hot_behavior PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../vexspoke/src)
target_compile_options(hot_behavior PRIVATE -Wall -Wextra -Werror)
set_target_properties(hot_behavior PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hot"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hot")

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
# zero Vulkan/Metal — a window with bridges per the Window Decoupling Law),
# platform stubs elsewhere.
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
    target_sources(hotcwap PRIVATE window/window.c)
elseif(UNIX)
    target_sources(hotcwap PRIVATE window/window_linux.c)
endif()
endif() # NOT TARGET hotcwap

set(HOTCWAP_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../_tests/hotcwap")
if(NOT EXISTS "${HOTCWAP_TEST_DIR}")
    set(HOTCWAP_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
endif()

# Manifest artifact: headless suite for the module manifest contract
# (order-tolerant parse, fail-closed swap compatibility, digest).
# Self-contained (hot/manifest.c only) so it builds standalone and in-tree.
if(NOT TARGET manifest_test AND EXISTS "${HOTCWAP_TEST_DIR}/manifest_test.c")
    add_executable(manifest_test
        hot/manifest.c
        ${HOTCWAP_TEST_DIR}/manifest_test.c
    )
    target_include_directories(manifest_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(manifest_test PRIVATE -Wall -Wextra -Werror)
endif()

# Headless spoke bridge test: static fill wires every slot, arena lifecycle
# through the opaque table with type attestation. Needs the real vexspoke
# symbols, so it builds only where the vexspoke target exists.
if(NOT TARGET spoke_test AND EXISTS "${HOTCWAP_TEST_DIR}/spoke_test.c" AND TARGET vexspoke)
    add_executable(spoke_test
        spoke/vexspoke.c
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

# Inert pixel-seam contract test (_docs/bridging.md §3): pins the safe
# defaults (false/nullptr/no-op) of every function hotcwap keeps inert by
# design. In-repo (tests/) so it is tracked — unlike the umbrella _tests/
# scratch dir. Links hotcwap for the per-OS backend; vexspoke input symbols
# resolve transitively in-tree exactly like window_test.
if(NOT TARGET bridge_seam_test AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge_seam_test.c")
    add_executable(bridge_seam_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge_seam_test.c
    )
    target_include_directories(bridge_seam_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(bridge_seam_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(bridge_seam_test PRIVATE hotcwap)
endif()
