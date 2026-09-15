#include "hot/hot.h"
#include "hot/manifest.h"
#include "hot/hot_trampoline.h"
#include "hot/hot_retire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <stdatomic.h>
#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: HotModule (hot/hot.c)
 * LEVEL: L4 — Self-Management (watches, verifies, swaps, retires; fixes/upgrades itself)
 * ============================================================================
 * Hotloading system: watches hot_dir for .dylib/.so, swaps trampoline
 * pointers atomically, retires old handles after a grace period. Hot_poll()
 * loads clones, verifies exports are present, then swaps. The manifest
 * authority (MANIFEST_UPDATE/PROMOTE) gates what lands in the watch dir —
 * the loader trusts the ladder placement. Hot_poll() runs on main thread only.
 *
 * STRUCT FIELDS (Mirroring typedef struct HotModule — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char hot_dir[HOT_PATH_LEN];              // watched directory (512B path)
 *   HotModuleInternal modules[HOT_MAX_MODULES]; // per-module slots (max 32)
 *   uint32_t module_count;                   // used slots in modules[]
 *   char last_error[256];                    // last diagnostic string
 *   HotTrampolineTable trampolines;          // per-instance symbols (zeroed by Hot_init calloc)
 *   HotRetireRing retireRing;                // per-instance generational close
 *
 * PRIVATE HELPERS (kept file-local pure-data only, with full fields):
 * ----------------------------------------------------------------------------
 *   HotModuleInternal (per-module state — HotModule's slot type, no own API):
 *     char name[HOT_MANIFEST_MAX_NAME];      // module name (no extension)
 *     char path[HOT_PATH_LEN];               // source dylib path
 *     void *handle;                          // dlopen handle (NULL = unloaded)
 *     uint64_t last_modified;                // mtime ns + size (change stamp)
 *     bool loaded;                           // true once first load succeeds
 *
 * Segregated (own files, see their overviews):
 *   HotTrampoline → hot/hot_trampoline.h/c
 *   HotRetiredHandle → hot/hot_retire.h/c
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Hot_init(hot_dir)
 *
 * Core Functions:
 *   - HotShutdown(hot)
 *   - Hot_poll(hot, loaded_count)
 *   - Hot_shutdown_module(hot, module_name)
 *   - Hot_save_module(hot, module_name, buf, cap, outLen)
 *   - Hot_restore_module(hot, module_name, buf, len)
 *   - Hot_migrate_module(hot, module_name, oldVersion, oldBuf, oldLen, newBuf, newCap, outLen)
 *
 * Getters:
 *   - Hot_get_api(hot, module_name)
 *   - Hot_get_symbol(hot, name)
 *   - Hot_last_error(hot)
 * ============================================================================
 */


// hot/hot.c — Hotloading system implementation.
//
// Architecture:
//   - Each module is a .dylib/.so loaded via dlopen()
//   - Function pointers are accessed through a trampoline table
//   - On reload, the new dylib is loaded, verified, then the trampoline
//     table is atomically swapped
//   - The old dylib is unloaded after a grace period (next frame)
//
// Thread safety:
//   - Hot_poll() must be called from the main thread only
//   - Function pointer tables are swapped atomically (C23 atomics)
//   - No locks needed for the trampoline table itself

#define HOT_MAX_MODULES 32
#define HOT_PATH_LEN 512

// Internal module state
typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];
    char path[HOT_PATH_LEN];
    void *handle;                    // dlopen handle
    uint64_t last_modified;          // last file modification timestamp (ns) + size
    bool loaded;                     // is currently loaded
} HotModuleInternal;
typedef struct HotModule {
    char hot_dir[HOT_PATH_LEN];
    HotModuleInternal modules[HOT_MAX_MODULES];
    uint32_t module_count;
    char last_error[256];
    HotTrampolineTable trampolines;  // per-instance symbols (no cross-app collision)
    HotRetireRing retireRing;        // per-instance generational close
} HotModule;

// CONSTRUCTORS — HotModule owns the module slots below; row mechanics live
// in hot_trampoline.c, generational close in hot_retire.c.

// Get file modification time with nanosecond precision + file size
static uint64_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#if defined(__APPLE__)
    uint64_t sec = (uint64_t) st.st_mtimespec.tv_sec;
    uint64_t nsec = (uint64_t) st.st_mtimespec.tv_nsec;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
    uint64_t sec = (uint64_t) st.st_mtim.tv_sec;
    uint64_t nsec = (uint64_t) st.st_mtim.tv_nsec;
#else
    uint64_t sec = (uint64_t) st.st_mtime;
    uint64_t nsec = 0;
#endif
    return (sec * 1000000000ULL) + nsec + (uint64_t) st.st_size;
}

// Check if a file exists
// static bool file_exists(const char *path) {
//     return access(path, F_OK) == 0;
// }

// Copy a file (for cloning working dylibs)
static bool file_copy(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
}

// Find a module by name
static HotModuleInternal *find_module(HotModule *hot, const char *name) {
    for (uint32_t i = 0; i < (*hot).module_count; i++) {
        if (strcmp((*hot).modules[i].name, name) == 0) {
            return &(*hot).modules[i];
        }
    }
    return NULL;
}

HotModule *Hot_init(const char *hot_dir) {
    if (!hot_dir) return NULL;
    
    HotModule *hot = (HotModule*) calloc(1, sizeof(HotModule));
    if (!hot) return NULL;
    
    strncpy((*hot).hot_dir, hot_dir, HOT_PATH_LEN - 1);
    (*hot).hot_dir[HOT_PATH_LEN - 1] = '\0';
    
    // Ensure hot directory exists
    DIR *dir = opendir(hot_dir);
    if (!dir) {
        // Try to create it
        #ifdef __APPLE__
        mkdir(hot_dir, 0755);
        #else
        mkdir(hot_dir, 0755);
        #endif
        dir = opendir(hot_dir);
    }
    if (dir) closedir(dir);
    
    return hot;
}

void HotShutdown(HotModule *hot) {
    if (!hot) return;

    // Unload all modules
    for (uint32_t i = 0; i < (*hot).module_count; i++) {
        HotModuleInternal *mod = &(*hot).modules[i];
        if ((*mod).handle) {
            typedef void (*ShutdownFn)(void);
            ShutdownFn shutdown = (ShutdownFn) dlsym((*mod).handle, "Hot_shutdown_module");
            if (shutdown)
                shutdown();
            dlclose((*mod).handle);
            (*mod).handle = NULL;
            (*mod).loaded = false;
        }
    }
    
    // Drain retired handle ring
    HotRetireRing_drainAll(&(*hot).retireRing);

    free(hot);
}

// Load a module from a dylib path
static HotResult load_module(HotModule *hot, HotModuleInternal *mod, const char *dylib_path) {
    HotTrampolineTable *table = &(*hot).trampolines;
    HotRetireRing *ring = &(*hot).retireRing;
    // 1. Load the dylib
    void *handle = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf((*hot).last_error, sizeof((*hot).last_error), 
                 "dlopen(%s) failed: %s", dylib_path, dlerror());
        return HOT_ERROR_DLOPEN_FAILED;
    }
    
    // 2. Register trampolines from the module's struct contract.
    //    (The retired HotManifest JSON path is gone — modules expose
    //    VkModuleGetTrampolines, and the ladder placement is the gate.)
    typedef const void *(*TrampolinesFn)(uint32_t*);
    TrampolinesFn get_trampolines = (TrampolinesFn)dlsym(handle, "VkModuleGetTrampolines");
    if (!get_trampolines) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "dlsym(VkModuleGetTrampolines) failed: %s", dlerror());
        dlclose(handle);
        return HOT_ERROR_DLSYM_FAILED;
    }
    uint32_t trampoline_count = 0;
    const void *trampolines = get_trampolines(&trampoline_count);
    for (uint32_t i = 0; i < trampoline_count; i++) {
        // Trampolines are {const char *name, void *function}
        const struct { const char *name; void *fn; } *entries = trampolines;
        int tidx = HotTrampolineTable_find(table, entries[i].name);
        if (tidx < 0)
            tidx = HotTrampolineTable_register(table, entries[i].name);
        if (tidx >= 0)
            HotTrampolineTable_set(table, tidx, entries[i].fn);
    }

    // 3. Adopt the new handle; the old one retires to the grace ring.
    if ((*mod).handle) HotRetireRing_retire(ring, (*mod).handle);
    (*mod).handle = handle;
    (*mod).last_modified = file_mtime(dylib_path);
    (*mod).loaded = true;

    return HOT_OK;
}

HotResult Hot_poll(HotModule *hot, uint32_t *loaded_count) {
    if (!hot) return HOT_ERROR_FILE_NOT_FOUND;
    if (loaded_count) *loaded_count = 0;

    HotRetireRing *ring = &(*hot).retireRing;
    HotRetireRing_advance(ring);
    
    // Scan the hot directory for .dylib files
    DIR *dir = opendir((*hot).hot_dir);
    if (!dir) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "Cannot open hot directory: %s", (*hot).hot_dir);
        return HOT_ERROR_FILE_NOT_FOUND;
    }
    
    uint32_t reloaded = 0;
    struct dirent *ent;
    
    while ((ent = readdir(dir)) != NULL) {
        // Check if it's a .dylib or .so
        const char *name = (*ent).d_name;
        size_t nlen = strlen(name);
        bool is_dylib = (nlen > 6 && strcmp(name + nlen - 6, ".dylib") == 0) ||
                        (nlen > 3 && strcmp(name + nlen - 3, ".so") == 0);
        if (!is_dylib) continue;
        
        // Extract module name (strip extension)
        char mod_name[HOT_MANIFEST_MAX_NAME];
        strncpy(mod_name, name, HOT_MANIFEST_MAX_NAME - 1);
        mod_name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
        char *dot = strrchr(mod_name, '.');
        if (dot) *dot = '\0';
        
        // Build full path
        char path[HOT_PATH_LEN];
        int path_len = snprintf(path, sizeof(path), "%s/%s", (*hot).hot_dir, name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            fprintf(stderr, "[hot] ERROR: Module path '%s/%s' truncated (exceeds %d bytes)\n",
                    (*hot).hot_dir, name, HOT_PATH_LEN);
            continue;
        }
        
        // Check if this is a new or updated module
        uint64_t mtime = file_mtime(path);
        HotModuleInternal *mod = find_module(hot, mod_name);
        
        if (!mod) {
            // New module — add it
            if ((*hot).module_count >= HOT_MAX_MODULES) {
                snprintf((*hot).last_error, sizeof((*hot).last_error),
                         "Too many modules (limit %d reached)", HOT_MAX_MODULES);
                fprintf(stderr, "[hot] ERROR: HOT_MAX_MODULES (%d) exceeded, skipping module '%s'\n",
                        HOT_MAX_MODULES, mod_name);
                continue;
            }
            mod = &(*hot).modules[(*hot).module_count++];
            strncpy((*mod).name, mod_name, HOT_MANIFEST_MAX_NAME - 1);
            (*mod).name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
            strncpy((*mod).path, path, HOT_PATH_LEN - 1);
            (*mod).path[HOT_PATH_LEN - 1] = '\0';
            (*mod).handle = NULL;
            (*mod).loaded = false;
        }
        
        // Check if file has been modified
        if ((*mod).loaded && (*mod).last_modified >= mtime) {
            continue; // No change
        }
        
        // Clone the dylib first (so we can verify before committing)
        char clone_path[HOT_PATH_LEN];
        int clone_len = snprintf(clone_path, sizeof(clone_path), "%s/.%s.clone", (*hot).hot_dir, name);
        if (clone_len < 0 || (size_t)clone_len >= sizeof(clone_path)) {
            fprintf(stderr, "[hot] ERROR: Clone path '%s/.%s.clone' truncated (exceeds %d bytes)\n",
                    (*hot).hot_dir, name, HOT_PATH_LEN);
            continue;
        }
        
        if (!file_copy(path, clone_path)) {
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "Failed to clone %s", path);
            continue;
        }
        
        // Try to load the clone
        HotModuleInternal clone_mod;
        memcpy(&clone_mod, mod, sizeof(clone_mod));
        clone_mod.handle = NULL;
        
        HotResult result = load_module(hot, &clone_mod, clone_path);
        
        if (result == HOT_OK) {
            // Success — commit the swap
            if ((*mod).handle) {
                HotRetireRing_retire(ring, (*mod).handle);
            }
            memcpy(mod, &clone_mod, sizeof(HotModuleInternal));
            reloaded++;
            
            fprintf(stderr, "[hot] reloaded %s\n", (*mod).name);
        } else {
            // Failed — clean up
            if (clone_mod.handle) {
                dlclose(clone_mod.handle);
            }
            fprintf(stderr, "[hot] failed to reload %s: %s\n",
                    (*mod).name, (*hot).last_error);
        }
        
        // Remove clone
        unlink(clone_path);
    }
    
    closedir(dir);
    
    if (loaded_count) *loaded_count = reloaded;
    return HOT_OK;
}

const void *Hot_get_api(HotModule *hot, const char *module_name) {
    if (!hot || !module_name) return NULL;
    
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded) return NULL;
    
    // Return the module's primary entry point, looked up by its own name
    // (the exported trampoline set after the struct-based swap).
    HotTrampolineTable *table = &(*hot).trampolines;
    int tidx = HotTrampolineTable_find(table, (*mod).name);
    if (tidx < 0) return NULL;

    return HotTrampolineTable_get(table, tidx);
}

HotFn Hot_get_symbol(HotModule *hot, const char *name) {
    if (!hot || !name) return NULL;

    // Find the trampoline by name
    HotTrampolineTable *table = &(*hot).trampolines;
    int tidx = HotTrampolineTable_find(table, name);
    if (tidx < 0) return NULL;

    return HotTrampolineTable_get(table, tidx);
}

const char *Hot_last_error(HotModule *hot) {
    if (!hot) return "NULL hot module";
    return (*hot).last_error;
}

void Hot_shutdown_module(HotModule *hot, const char *module_name) {
    if (!hot || !module_name)
        return;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return;
    typedef void (*ShutdownFn)(void);
    ShutdownFn shutdown = (ShutdownFn) dlsym((*mod).handle, "Hot_shutdown_module");
    if (shutdown)
        shutdown();
}

bool Hot_save_module(HotModule *hot, const char *module_name, void *buf, size_t cap, size_t *outLen) {
    if (!hot || !module_name || !buf || !outLen)
        return false;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return false;
    typedef bool (*SaveFn)(void*, size_t, size_t*);
    SaveFn save = (SaveFn) dlsym((*mod).handle, "Hot_save");
    if (!save)
        return false;
    return save(buf, cap, outLen);
}

bool Hot_restore_module(HotModule *hot, const char *module_name, const void *buf, size_t len) {
    if (!hot || !module_name || !buf)
        return false;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return false;
    typedef bool (*RestoreFn)(const void*, size_t);
    RestoreFn restore = (RestoreFn) dlsym((*mod).handle, "Hot_restore");
    if (!restore)
        return false;
    return restore(buf, len);
}

bool Hot_migrate_module(HotModule *hot, const char *module_name, const char *oldVersion,
                        const void *oldBuf, size_t oldLen, void *newBuf, size_t newCap, size_t *outLen) {
    if (!hot || !module_name || !oldVersion || !oldBuf || !newBuf || !outLen)
        return false;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return false;
    typedef bool (*MigrateFn)(const char*, const void*, size_t, void*, size_t, size_t*);
    MigrateFn migrate = dlsym((*mod).handle, "Hot_migrate");
    if (!migrate)
        return false;
    return migrate(oldVersion, oldBuf, oldLen, newBuf, newCap, outLen);
}
