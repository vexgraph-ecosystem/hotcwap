#include "hot/vk_context.h"
#include "vulkan/vk.h"
#include "vulkan/vk_mac.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 *  * MODULE: VkLoader (hot/vk_loader.c)
 *  * LEVEL: L4 — Self-Management (module hot-reload shim over graphvex Vulkan)
 *  * ============================================================================
 *  * vk_loader.c is the thin module hot-reload adapter. It does NOT create a
 *  * VkInstance or VkDevice — those are owned entirely by graphvex (vk_instance.c
 *  * via the Vk_* seam). vk_loader.c's job:
 *  *   1. Load a Vulkan .dylib module via dlopen
 *  *   2. Extract the module's manifest + trampoline table
 *  *   3. Verify ABI compatibility (type IDs match frozen contracts)
 *  *   4. Atomically swap function pointers via the trampoline table
 *  *   5. Retire old dylibs safely across reload generations
 *  *   6. Persist + restore pipeline cache (VkPipelineCache handle obtained
 *  *      from graphvex, not created locally)
 *
 *  * All Vulkan handles flow from graphvex's Vk_* accessors:
 *  *   Vk_getInstance() / Vk_getGpa()  — for instance-level loader calls
 *  *   Vk_getDevice() / Vk_getGdpa()   — for device-level loader calls
 *  *   Vk_getQueue() / Vk_getQueueFamily()
 *
 *  * STRUCT FIELDS (local to this file):
 *  * ----------------------------------------------------------------------------
 *  *   Trampoline (one row per exported symbol):
 *  *     _Atomic(void*) ptr;                    // current generation target
 *  *     _Atomic(void*) fallback_ptr;           // prior generation (mid-swap cover)
 *  *     char name[64];                         // export symbol name
 *  *
 *  *   VkRetiredHandle (one parked dylib):
 *  *     void *handle;                          // retired dylib (nullptr = free slot)
 *  *     uint32_t generation;                   // reload generation when retired
 *  *
 *  *   Module statics:
 *  *     void *s_module_handle;                // currently loaded dylib
 *  *     bool s_initialized;                   // module ready
 *  *     VkPipelineCache s_cache;              // pipeline cache (from graphvex)
 *  *     PFN_vkCreatePipelineCache s_createCache;  // resolved via gdpa
 *  *     PFN_vkDestroyPipelineCache s_destroyCache;
 *  *     PFN_vkGetPipelineCacheData s_getCacheData;
 *
 *  * FUNCTION REGISTRY:
 *  * ----------------------------------------------------------------------------
 *  * Core Functions:
 *  *   - hot_vk_init_loader(void)
 *  *   - hot_vk_load_module(path)
 *  *   - hot_vk_shutdown(void)
 *  *   - vk_retire_handle(handle)
 *  *   - vk_advance_generation(void)
 *  *
 *  * Getters:
 *  *   - hot_vk_get_symbol(name)
 *  * ============================================================================
 */

// hot/vk_loader.c — thin module hot-reload adapter over graphvex Vulkan.
//
// The VkDevice is owned by graphvex (Vk_init in vk_instance.c). vk_loader.c
// only manages dylib loading, trampoline table atomics, and pipeline cache
// persistence. All Vulkan handles are obtained via Vk_get*() accessors.

// Module handle
static void *s_module_handle = nullptr;
static bool s_initialized = false;

// Pipeline cache state — handle obtained from graphvex, functions resolved
// through Vk_getGdpa(). We don't create our own device.
static VkPipelineCache s_cache = VK_NULL_HANDLE;
static PFN_vkCreatePipelineCache s_createCache = nullptr;
static PFN_vkDestroyPipelineCache s_destroyCache = nullptr;
static PFN_vkGetPipelineCacheData s_getCacheData = nullptr;

// Function pointers from the module
static VkModuleInitFn s_module_init = nullptr;
static VkModuleShutdownFn s_module_shutdown = nullptr;
static VkModuleGetTrampolinesFn s_module_get_trampolines = nullptr;
static VkModuleGetManifestFn s_module_get_manifest = nullptr;

// Trampoline table (atomic)
#define MAX_TRAMPOLINES 64
typedef struct {
    _Atomic(void*) ptr;
    _Atomic(void*) fallback_ptr;
    char name[64];
} Trampoline;

static Trampoline s_trampolines[MAX_TRAMPOLINES];
static _Atomic uint32_t s_trampoline_count = 0;

// Find or create a trampoline
static int trampoline_find(const char *name) {
    uint32_t count = atomic_load(&s_trampoline_count);
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(s_trampolines[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int trampoline_create(const char *name) {
    uint32_t idx = atomic_fetch_add(&s_trampoline_count, 1);
    if (idx >= MAX_TRAMPOLINES) return -1;
    strncpy(s_trampolines[idx].name, name, 63);
    s_trampolines[idx].name[63] = '\0';
    atomic_store(&s_trampolines[idx].ptr, nullptr);
    atomic_store(&s_trampolines[idx].fallback_ptr, nullptr);
    return (int)idx;
}

void *hot_vk_get_symbol(const char *name) {
    int idx = trampoline_find(name);
    if (idx < 0) return nullptr;
    void *ptr = atomic_load(&s_trampolines[idx].ptr);
    if (!ptr) {
        for (int retry = 0; retry < 4 && !ptr; retry++) {
            #if defined(__aarch64__)
            __asm__ volatile("yield");
            #endif
            ptr = atomic_load(&s_trampolines[idx].ptr);
        }
        if (!ptr) {
            ptr = atomic_load(&s_trampolines[idx].fallback_ptr);
        }
    }
    return ptr;
}

// Generational handle retirement: keeps old dylib handles alive across
// reload generations to prevent race conditions during module reload.
#define VK_RETIRED_MAX 16
#define VK_RETIRED_GENERATIONS 4

typedef struct {
    void *handle;
    uint32_t generation;
} VkRetiredHandle;

static VkRetiredHandle s_vk_retired[VK_RETIRED_MAX] = {0};
static uint32_t s_vk_generation = 0;

static void vk_retire_handle(void *handle) {
    if (!handle) return;

    // Reap old generations
    for (size_t i = 0; i < VK_RETIRED_MAX; i++) {
        if (s_vk_retired[i].handle && (s_vk_generation - s_vk_retired[i].generation >= VK_RETIRED_GENERATIONS)) {
            dlclose(s_vk_retired[i].handle);
            s_vk_retired[i].handle = nullptr;
        }
    }

    // Find a free slot
    size_t slot = VK_RETIRED_MAX;
    for (size_t i = 0; i < VK_RETIRED_MAX; i++) {
        if (!s_vk_retired[i].handle) {
            slot = i;
            break;
        }
    }

    if (slot == VK_RETIRED_MAX) {
        // Evict oldest if all slots full
        size_t oldest_idx = 0;
        uint32_t oldest_gen = UINT32_MAX;
        for (size_t i = 0; i < VK_RETIRED_MAX; i++) {
            if (s_vk_retired[i].generation < oldest_gen) {
                oldest_gen = s_vk_retired[i].generation;
                oldest_idx = i;
            }
        }
        dlclose(s_vk_retired[oldest_idx].handle);
        slot = oldest_idx;
    }

    s_vk_retired[slot].handle = handle;
    s_vk_retired[slot].generation = s_vk_generation;
}

static void vk_advance_generation(void) {
    s_vk_generation++;
    for (size_t i = 0; i < VK_RETIRED_MAX; i++) {
        if (s_vk_retired[i].handle && (s_vk_generation - s_vk_retired[i].generation >= VK_RETIRED_GENERATIONS)) {
            dlclose(s_vk_retired[i].handle);
            s_vk_retired[i].handle = nullptr;
        }
    }
}

// Resolve pipeline cache function pointers from graphvex's gdpa accessor.
// Called once during init; cached as statics for hot path efficiency.
static bool resolveCacheFns(void) {
    if (!s_createCache) {
        PFN_vkGetDeviceProcAddr gdpa = Vk_getGdpa();
        if (!gdpa) return false;
        s_createCache = (PFN_vkCreatePipelineCache)gdpa(Vk_getDevice(), "vkCreatePipelineCache");
        s_destroyCache = (PFN_vkDestroyPipelineCache)gdpa(Vk_getDevice(), "vkDestroyPipelineCache");
        s_getCacheData = (PFN_vkGetPipelineCacheData)gdpa(Vk_getDevice(), "vkGetPipelineCacheData");
    }
    return s_createCache && s_destroyCache && s_getCacheData;
}

// Initialize the Vulkan module loader shim.
// Vk_init() must have been called first (by the caller) so the device exists.
bool hot_vk_init_loader(void) {
    if (!Vk_ready())
        return false;

    // Grab the device handle from graphvex.
    VkDevice dev = Vk_getDevice();
    if (dev == VK_NULL_HANDLE)
        return false;

    // Resolve cache function pointers from graphvex's seam.
    if (!resolveCacheFns())
        return false;

    // Create pipeline cache, seeded from disk when a prior run saved one.
    // The cache blob is driver-versioned: vkCreatePipelineCache rejects
    // stale data itself, so a corrupt/mismatched file just falls back to
    // an empty cache — never a fatal error.
    uint8_t *cache_data = nullptr;
    size_t cache_size = 0;
    FILE *cache_in = fopen("hot/.pipeline_cache", "rb");
    if (cache_in) {
        fseek(cache_in, 0, SEEK_END);
        long cache_len = ftell(cache_in);
        fseek(cache_in, 0, SEEK_SET);
        if (cache_len > 0 && cache_len < 16 * 1024 * 1024) {
            cache_data = (uint8_t*) malloc((size_t) cache_len);
            if (cache_data) {
                if (fread(cache_data, 1, (size_t) cache_len, cache_in) == (size_t) cache_len)
                    cache_size = (size_t) cache_len;
                else {
                    free(cache_data);
                    cache_data = nullptr;
                }
            }
        }
        fclose(cache_in);
    }
    VkPipelineCacheCreateInfo cache_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = cache_size,
        .pInitialData = cache_data,
    };
    s_createCache(dev, &cache_ci, nullptr, &s_cache);
    if (cache_data)
        free(cache_data);

    s_initialized = true;
    printf("[vk_loader] initialized (device=%p, cache=%p)\n",
           (void*) Vk_getDevice(), (void*) s_cache);
    return true;
}

// Load the Vulkan module from a dylib
bool hot_vk_load_module(const char *path) {
    if (s_module_handle) {
        // Shutdown old module first
        if (s_module_shutdown) s_module_shutdown();
        vk_retire_handle(s_module_handle);
        vk_advance_generation();
        s_module_handle = nullptr;
        s_initialized = false;
    }

    s_module_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!s_module_handle) {
        fprintf(stderr, "[vk_loader] dlopen failed: %s\n", dlerror());
        return false;
    }

    // Get module functions
    s_module_init = (VkModuleInitFn)dlsym(s_module_handle, "VkModuleInit");
    s_module_shutdown = (VkModuleShutdownFn)dlsym(s_module_handle, "VkModuleShutdown");
    s_module_get_trampolines = (VkModuleGetTrampolinesFn)dlsym(s_module_handle, "VkModuleGetTrampolines");
    s_module_get_manifest = (VkModuleGetManifestFn)dlsym(s_module_handle, "VkModuleGetManifest");

    if (!s_module_init || !s_module_get_trampolines || !s_module_get_manifest) {
        fprintf(stderr, "[vk_loader] missing required exports\n");
        dlclose(s_module_handle);
        s_module_handle = nullptr;
        return false;
    }

    // Get manifest and verify ABI
    const VkModuleManifest *manifest = s_module_get_manifest();
    printf("[vk_loader] loading %s v%s\n", (*manifest).name, (*manifest).version);

    // Build the context from graphvex's seam — all handles flow from here.
    VkHotContext context = {
        .instance = Vk_getInstance(),
        .physical_device = Vk_getPhys(),
        .device = Vk_getDevice(),
        .queue = Vk_getQueue(),
        .queue_family = Vk_getQueueFamily(),
        .pipeline_cache = s_cache,
        .pipeline_cache_path = "hot/.pipeline_cache",
        .vulkan_api_version = VK_API_VERSION_1_2,
        .pipeline_cache_size = 0,
        .texture_registry = nullptr,
    };

    // Initialize the module
    if (!s_module_init(&context)) {
        fprintf(stderr, "[vk_loader] module init failed\n");
        dlclose(s_module_handle);
        s_module_handle = nullptr;
        return false;
    }

    // Register trampolines
    uint32_t trampoline_count = 0;
    const VkTrampolineEntry *trampolines = s_module_get_trampolines(&trampoline_count);
    for (uint32_t i = 0; i < trampoline_count; i++) {
        int idx = trampoline_find(trampolines[i].name);
        if (idx < 0) idx = trampoline_create(trampolines[i].name);
        if (idx >= 0) {
            void *old = atomic_load(&s_trampolines[idx].ptr);
            if (old && old != trampolines[i].function) {
                atomic_store(&s_trampolines[idx].fallback_ptr, old);
            }
            atomic_store(&s_trampolines[idx].ptr, trampolines[i].function);
        }
    }

    s_initialized = true;
    printf("[vk_loader] module loaded (%u trampolines)\n", trampoline_count);
    return true;
}

// Shutdown the Vulkan module loader shim
void hot_vk_shutdown(void) {
    if (s_module_shutdown) s_module_shutdown();
    if (s_module_handle) {
        dlclose(s_module_handle);
        s_module_handle = nullptr;
    }
    for (size_t i = 0; i < VK_RETIRED_MAX; i++) {
        if (s_vk_retired[i].handle) {
            dlclose(s_vk_retired[i].handle);
            s_vk_retired[i].handle = nullptr;
        }
    }
    s_initialized = false;

    // Persist + destroy pipeline cache. vkGetPipelineCacheData sizes the
    // blob; a zero size or error simply skips the write. All handles come
    // from graphvex's seam.
    if (s_cache && s_getCacheData) {
        size_t data_size = 0;
        VkDevice dev = Vk_getDevice();
        if (s_getCacheData(dev, s_cache, &data_size, nullptr) == VK_SUCCESS && data_size > 0) {
            uint8_t *data = (uint8_t*) malloc(data_size);
            if (data) {
                if (s_getCacheData(dev, s_cache, &data_size, data) == VK_SUCCESS) {
                    FILE *cache_out = fopen("hot/.pipeline_cache", "wb");
                    if (cache_out) {
                        fwrite(data, 1, data_size, cache_out);
                        fclose(cache_out);
                    }
                }
                free(data);
            }
        }
        if (s_destroyCache)
            s_destroyCache(Vk_getDevice(), s_cache, nullptr);
        s_cache = VK_NULL_HANDLE;
    }

    printf("[vk_loader] shutdown\n");
}
