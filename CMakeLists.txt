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
# NOTE: Vulkan core (instance/device/swapchain/pane/surface) has moved to
# graphvex. hotcwap keeps only the Window seam glue (vk_loader.c bridges
# kernel.c <-> graphvex's Vk_* API).
add_library(hotcwap STATIC
    process/application.c
    process/process.c
    process/console.c
    kernel/kernel.c
    hot/hot.c
    hot/hot_trampoline.c
    hot/hot_retire.c
    hot/manifest.c
    hot/vk_loader.c
    hot/spv_watch.c
    hot/stage.c
    window/window_event.c
)
target_include_directories(hotcwap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(hotcwap PRIVATE -Wall -Wextra -Werror)

# Vulkan headers: prefer find_package, fall back to brew --prefix
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND AND APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(BREW_PREFIX AND EXISTS "${BREW_PREFIX}/include/vulkan/vulkan.h")
        set(Vulkan_INCLUDE_DIRS "${BREW_PREFIX}/include")
        set(Vulkan_FOUND TRUE)
    endif()
endif()
if(Vulkan_FOUND)
    target_include_directories(hotcwap PUBLIC ${Vulkan_INCLUDE_DIRS})
endif()

# Dynamic module sample: compiled into runtime dylib
add_library(vk_module MODULE hot/vk_module.c)
target_include_directories(vk_module PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(vk_module PRIVATE -Wall -Wextra -Werror)
if(Vulkan_FOUND)
    target_include_directories(vk_module PUBLIC ${Vulkan_INCLUDE_DIRS})
endif()

# Phase-1 L2 behavior subject: pure pulse/bar math + texture path.
# Host handlers delegate here; module holds no GPU/Panel state so swap is
# safe by construction. Output lands in ${CMAKE_BINARY_DIR}/hot next to
# vk_test's watch dir; copy to ./hot/ when running from the source root.
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
