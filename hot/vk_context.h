#ifndef HOT_VK_CONTEXT_H
#define HOT_VK_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

// hot/vk_context.h — Vulkan context transfer protocol.
//
// The VkDevice is owned by the application loader, NOT by any module.
// Each module (vulkan.dylib) receives a context with the device handle,
// queue, physical device, and pipeline cache. The module creates its own
// pipelines, descriptor sets, and framebuffers — but these are recreated
// on hotreload, not transferred.
//
// Transferable across reloads (just copy the handle):
//   - VkDevice, VkQueue, VkPhysicalDevice
//   - VkPipelineCache (serialized to disk, deserialized by new module)
//   - VkImage from pane swapchains (OS-owned via CAMetalLayer)
//   - VkDeviceMemory (GPU memory is GPU memory)
//
// Non-transferable (must recreate):
//   - VkPipeline (baked to specific shader code)
//   - VkDescriptorSetLayout (binds to specific module code)
//   - VkRenderPass (format compatibility)
//   - VkFramebuffer (tied to specific image views)

typedef struct {
    // Device handles — owned by loader, stable across reloads
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    
    // Pipeline cache — serialized across reloads
    // On first load: load from disk (if exists), create cache
    // On reload: save old cache to disk, new module loads from disk
    VkPipelineCache pipeline_cache;
    char pipeline_cache_path[512];
    
    // Version info (for cache compatibility)
    uint32_t vulkan_api_version;
    uint32_t pipeline_cache_size;
    
    // Bindless texture registry — module-specific IDs
    // The registry is owned by the loader, not the module
    // On reload, the new module registers its own textures
    void *texture_registry;  // Opaque pointer to loader-owned registry
} VkHotContext;

// Module interface — each vulkan.dylib exports these

// Initialize the Vulkan subsystem with a context.
// The context is provided by the loader and persists across reloads.
// On first load, the module SHOULD create the device and return it
// via VkModuleGetDevice(). On subsequent loads, the context->device
// will be non-NULL and the module should skip device creation.
// Returns true on success.
typedef bool (*VkModuleInitFn)(const VkHotContext *context);

// Shutdown the Vulkan subsystem.
// Does NOT destroy the device — only module-owned resources.
typedef void (*VkModuleShutdownFn)(void);

// Get the VkDevice handle (for context transfer).
// Returns NULL if the module hasn't created a device yet.
typedef VkDevice (*VkModuleGetDeviceFn)(void);

// Get the VkInstance handle.
typedef VkInstance (*VkModuleGetInstanceFn)(void);

// Get the VkPhysicalDevice handle.
typedef VkPhysicalDevice (*VkModuleGetPhysicalDeviceFn)(void);

// Get the VkQueue handle.
typedef VkQueue (*VkModuleGetQueueFn)(void);

// Get the queue family index.
typedef uint32_t (*VkModuleGetQueueFamilyFn)(void);

// Begin a frame. Returns the next image index.
typedef uint32_t (*VkModuleBeginFrameFn)(void);

// End a frame and present.
typedef void (*VkModuleEndFrameFn)(void);

// Get the module's trampoline table.
// Each entry is a function pointer that can be atomically swapped.
typedef struct {
    const char *name;
    void *function;
} VkTrampolineEntry;

typedef const VkTrampolineEntry *(*VkModuleGetTrampolinesFn)(uint32_t *count);

// The module's manifest (for ABI verification)
typedef struct {
    const char *name;
    const char *version;
    uint32_t type_id_count;
    struct { const char *name; uint64_t value; } type_ids[256];
    uint32_t export_count;
    const char *exports[128];
    uint32_t dependency_count;
    const char *dependencies[16];
} VkModuleManifest;

typedef const VkModuleManifest *(*VkModuleGetManifestFn)(void);

#endif
