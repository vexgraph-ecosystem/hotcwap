#include "hot/hot.h"
#include "hot/manifest.h"
#include "hot/hot_trampoline.h"
#include "hot/hot_retire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "annotation/overview.h"
#include "annotation/intention.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: HotModule (hot/hot.c)
 * LEVEL: L4 — Self-Management (watches the manifest ladder, verifies, swaps, retires)
 * ============================================================================
 * The manifest-driven hotloader. A HotModule binds ONE library key of the
 * mounted manifest (bin/current/<library>). Hot_poll compares
 * MANIFEST_GENERATION(<library>) against its last-seen generation: on a move
 * it snapshots the CURRENT generation's module state on an off-thread save
 * worker (hot loops never pay the serialization), then on a later pass it
 * dlopens every section of the new current set, verifies the WHOLE library
 * fail-closed (dlopen + VkModuleGetTrampolines on every section before any
 * commit), rehydrates the saved blobs into the STAGED images BEFORE any
 * commit, then atomically swaps the trampoline table and retires the old
 * handles into the grace ring. A section whose Hot_restore rejects its blob
 * rolls the whole swap back (#8.5 Automated State Rollback): the old
 * generation stays live, the generation never advances, and the next poll
 * re-attempts once the payload is fixed.
 * The rename slide (MANIFEST_PROMOTE) IS the swap — there is no clone step
 * and no watch dir. Hot_poll() runs on main thread only.
 *
 * STRUCT FIELDS (defined here; hot/hot.h keeps the type opaque):
 * ----------------------------------------------------------------------------
 *   char library[HOT_MANIFEST_MAX_NAME]; // manifest library key this loader watches
 *   uint64_t generation;                 // last COMMITTED bin/current/<lib> generation
 *   HotModuleInternal modules[HOT_MAX_MODULES]; // per-section slots (max 32)
 *   uint32_t module_count;               // used slots in modules[]
 *   char last_error[256];                // last diagnostic string
 *   HotTrampolineTable trampolines;      // per-instance symbols
 *   HotRetireRing retireRing;            // per-instance generational close
 *   pthread_t saveThread;                // off-thread state-save worker
 *   pthread_mutex_t saveLock;            // guards saveRequested/saveDone/saveSlots
 *   pthread_cond_t saveCond;             // worker sleep/wake
 *   bool saveWorkerLive;                 // worker running
 *   bool saveCancel;                     // shutdown flag (under saveLock)
 *   bool saveRequested;                  // snapshot pending (under saveLock)
 *   bool saveDone;                       // snapshot published (under saveLock)
 *   bool swapPending;                    // snapshot kicked; swap on next poll
 *   HotSaveSlot saveSlots[HOT_MAX_MODULES]; // snapshot blobs (worker writes, poll reads)
 *   size_t saveCount;                    // populated slots
 *
 * PRIVATE HELPERS (kept file-local pure-data only, each with full fields):
 * ----------------------------------------------------------------------------
 *   HotModuleInternal (per-section slot — HotModule's slot type, no own API):
 *     char name[HOT_MANIFEST_MAX_NAME];  // section stem (no extension)
 *     char path[HOT_PATH_LEN];           // source dylib path
 *     void *handle;                      // dlopen handle (NULL = unloaded)
 *     bool loaded;                       // true once first load succeeds
 *
 *   HotSaveSlot (per-section state blob captured off-thread):
 *     char name[HOT_MANIFEST_MAX_NAME];  // section stem the blob belongs to
 *     uint8_t buf[HOT_SAVE_SLOT_CAP];    // module Hot_save payload (4 KiB)
 *     size_t len;                        // valid bytes in buf
 *
 *   HotStagedSection (verify-phase staging, transient inside perform_swap):
 *     HotModuleInternal *mod;   // resolved slot (created if new)
 *     char path[HOT_PATH_LEN];  // dylib path
 *     void *handle;             // dlopened, not yet adopted
 *     const void *trampolines;  // VkModuleGetTrampolines table
 *     uint32_t trampolineCount; // entries
 *
 *   Static wiring (no stored state): find_module, snapshot_begin,
 *   snapshot_modules (worker body), save_worker_main, perform_swap.
 *
 * Segregated (own files, see their overviews):
 *   HotTrampoline → hot/hot_trampoline.h/c
 *   HotRetiredHandle → hot/hot_retire.h/c
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Hot_init(library)
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
 *   - Hot_get_symbol(hot, name)
 *   - Hot_get_generation(hot)
 *   - Hot_last_error(hot)
 * ============================================================================
 */

;;INTENTION("State-save module exports are trusted bounded: Hot_save copies a "
            "fixed-size state blob (memcpy of statics) and is called on the save "
            "worker thread. The worker's join is therefore bounded to <25ms plus "
            "one module snapshot, satisfying the Bounded Wait Law. If a module "
            "ever exports a Hot_save that loops unboundedly, the violation is at "
            "the module boundary — modules are in-repo trusted code (hotcwap "
            "hot_behavior), never hostile payloads.")

;;INTENTION("The trampoline table is set per-section inside perform_swap's "
            "commit phase: a call landing mid-commit can observe a mixed "
            "generation (section A new, section B old) for the microseconds of "
            "the swap. Module call sites reach code only through trampolines at "
            "frame boundaries and old images stay mapped in the retire ring "
            "throughout, so this window is safe. Full-library atomicity is "
            "enforced on the VERIFY side (no section is adopted unless every "
            "section dlopens + exports VkModuleGetTrampolines); the commit side "
            "is per-row assignment per the Conflict Triage Law.")

;;INTENTION("A library's declared section set is stable across its life (the "
            "manifest catalog is the contract; MANIFEST_UPDATE fails-closed "
            "against undeclared stems). A section that vanishes from a promoted "
            "current set keeps its old slot + handle mapped (never retired) so "
"stale trampolines never dangle — the unshared image lingers until "
             "HotShutdown. Removing a section is a library-uninstall event "
             "handled by a full restart, per the Conflict Triage Law.")

;;INTENTION("Restore-before-commit (#8.5 Automated State Rollback): a "
            "Hot_restore rejection — or a missing Hot_restore on a slot the "
            "previous generation snapshotted — fails the WHOLE swap: staged "
            "images close, old trampolines + state stay live, and the "
            "generation never advances. The ladder's current set keeps the "
            "bad payload (rejected again on every poll) until the deployer "
            "promotes a fixed set, so rollback self-heals. A module that "
            "intentionally drops Hot_save/Hot_restore must re-scope its "
            "state handoff first.")

#define HOT_MAX_MODULES 32
#define HOT_PATH_LEN 512
#define HOT_SAVE_SLOT_CAP 4096
#define HOT_SAVE_WAIT_NS (25 * 1000 * 1000) // 25ms cond-wait slice (the Bounded Wait Law)

typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];
    char path[HOT_PATH_LEN];
    void *handle;                    // dlopen handle
    bool loaded;                     // is currently loaded
} HotModuleInternal;

typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];
    alignas(16) uint8_t buf[HOT_SAVE_SLOT_CAP];
    size_t len;
} HotSaveSlot;

typedef struct {
    HotModuleInternal *mod;          // resolved slot (created if new)
    char path[HOT_PATH_LEN];         // dylib path
    void *handle;                    // dlopened, not yet adopted
    const void *trampolines;         // VkModuleGetTrampolines table
    uint32_t trampolineCount;        // entries
} HotStagedSection;

typedef struct HotModule {
    char library[HOT_MANIFEST_MAX_NAME];
    uint64_t generation;
    HotModuleInternal modules[HOT_MAX_MODULES];
    uint32_t module_count;
    char last_error[256];
    HotTrampolineTable trampolines;  // per-instance symbols (no cross-app collision)
    HotRetireRing retireRing;        // per-instance generational close
    // Off-thread state-save worker
    pthread_t saveThread;
    pthread_mutex_t saveLock;
    pthread_cond_t saveCond;
    bool saveWorkerLive;
    bool saveCancel;                 // under saveLock
    bool saveRequested;              // under saveLock
    bool saveDone;                   // under saveLock
    bool swapPending;                // main-thread only
    HotSaveSlot saveSlots[HOT_MAX_MODULES];
    size_t saveCount;
} HotModule;

// CONSTRUCTORS — HotModule owns the module slots below; row mechanics live
// in hot_trampoline.c, generational close in hot_retire.c.

// Find a module slot by section stem.
static HotModuleInternal *find_module(HotModule *hot, const char *name) {
    for (uint32_t i = 0; i < (*hot).module_count; i++) {
        if (strcmp((*hot).modules[i].name, name) == 0)
            return &(*hot).modules[i];
    }
    return NULL;
}

// Off-thread state-save worker (HOT_SAVE_WAIT_NS cond-wait cycle, cancel-aware).
static void *save_worker_main(void *arg);

HotModule *Hot_init(const char *library) {
    if (!library || *library == '\0')
        return NULL;
    if (strlen(library) >= HOT_MANIFEST_MAX_NAME)
        return NULL;
    if (!MANIFEST_ROOT())
        return NULL; // manifest must be mounted before the loader binds

    HotModule *hot = (HotModule*) calloc(1, sizeof(HotModule));
    if (!hot)
        return NULL;

    strncpy((*hot).library, library, HOT_MANIFEST_MAX_NAME - 1);
    (*hot).library[HOT_MANIFEST_MAX_NAME - 1] = '\0';

    pthread_mutex_init(&(*hot).saveLock, NULL);
    pthread_cond_init(&(*hot).saveCond, NULL);
    (*hot).saveWorkerLive = (pthread_create(&(*hot).saveThread, NULL,
                                            save_worker_main, hot) == 0);
    if (!(*hot).saveWorkerLive) {
        pthread_mutex_destroy(&(*hot).saveLock);
        pthread_cond_destroy(&(*hot).saveCond);
        free(hot);
        return NULL;
    }
    return hot;
}

// CORE FUNCTIONS

// Stop the save worker FIRST (bounded join — its cond wait is capped at
// HOT_SAVE_WAIT_NS), so no thread touches module handles after shutdown
// begins. Then close modules + drain the retire ring, per the Teardown
// Order Law (worker resources die before the handles they read).
static void save_worker_shutdown(HotModule *hot) {
    if (!(*hot).saveWorkerLive)
        return;
    pthread_mutex_lock(&(*hot).saveLock);
    (*hot).saveCancel = true;
    (*hot).saveRequested = false;
    pthread_cond_signal(&(*hot).saveCond);
    pthread_mutex_unlock(&(*hot).saveLock);
    pthread_join((*hot).saveThread, NULL);
    (*hot).saveWorkerLive = false;
}

void HotShutdown(HotModule *hot) {
    if (!hot)
        return;

    save_worker_shutdown(hot);
    pthread_mutex_destroy(&(*hot).saveLock);
    pthread_cond_destroy(&(*hot).saveCond);

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

// Runs ON THE SAVE WORKER thread. Copies each loaded module's state blob via
// its Hot_save export into saveSlots, then publishes saveDone. The module
// images being snapshotted are the CURRENT (old-generation) handles — they
// stay mapped in the retire ring until the swap commits, so the worker always
// reads live state.
static void snapshot_modules(HotModule *hot) {
    (*hot).saveCount = 0;
    for (uint32_t i = 0; i < (*hot).module_count && (*hot).saveCount < HOT_MAX_MODULES; i++) {
        HotModuleInternal *mod = &(*hot).modules[i];
        if (!(*mod).loaded || !(*mod).handle)
            continue;
        typedef bool (*SaveFn)(void *, size_t, size_t *);
        SaveFn save = (SaveFn) dlsym((*mod).handle, "Hot_save");
        if (!save)
            continue;
        HotSaveSlot *slot = &(*hot).saveSlots[(*hot).saveCount];
        size_t n = 0;
        if (save((*slot).buf, sizeof((*slot).buf), &n) && n > 0 && n <= sizeof((*slot).buf)) {
            strncpy((*slot).name, (*mod).name, HOT_MANIFEST_MAX_NAME - 1);
            (*slot).name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
            (*slot).len = n;
            (*hot).saveCount++;
        }
    }
}

// The worker loop: sleeps on saveCond (capped at HOT_SAVE_WAIT_NS so every
// path reaches the exit check bounded — the Bounded Wait Law), snapshots on
// request, exits on cancel.
static void *save_worker_main(void *arg) {
    HotModule *hot = (HotModule*) arg;
    pthread_mutex_lock(&(*hot).saveLock);
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += HOT_SAVE_WAIT_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&(*hot).saveCond, &(*hot).saveLock, &deadline);
        if ((*hot).saveCancel)
            break;
        if (rc == 0 || rc == ETIMEDOUT) {
            if ((*hot).saveRequested) {
                (*hot).saveRequested = false;
                snapshot_modules(hot);
                (*hot).saveDone = true;
            }
        }
    }
    pthread_mutex_unlock(&(*hot).saveLock);
    return NULL;
}

// Ask the worker to snapshot the CURRENT generation's state. Returns true
// when any loaded module exports Hot_save (a snapshot was requested); false
// when none do (the caller swaps directly — nothing to preserve).
static bool snapshot_begin(HotModule *hot) {
    bool any = false;
    for (uint32_t i = 0; i < (*hot).module_count && !any; i++) {
        HotModuleInternal *mod = &(*hot).modules[i];
        if ((*mod).loaded && (*mod).handle && dlsym((*mod).handle, "Hot_save") != NULL)
            any = true;
    }
    if (!any)
        return false;
    pthread_mutex_lock(&(*hot).saveLock);
    (*hot).saveRequested = true;
    (*hot).saveDone = false;
    (*hot).saveCount = 0;
    pthread_cond_signal(&(*hot).saveCond);
    pthread_mutex_unlock(&(*hot).saveLock);
    return true;
}

// Load a whole manifest library generation and commit it. Phase A verifies
// EVERY section (dlopen + VkModuleGetTrampolines present) before anything is
// touched — on any failure every staged handle is closed and the previous
// generation stays live, untouched. Phase B rehydrates the saved blobs into
// the STAGED images BEFORE any commit; a rejecting Hot_restore rolls the
// whole swap back (#8.5 Automated State Rollback). Phase C commits: retire
// old handles, adopt new, swap trampoline rows. Generation only advances on
// full success, so a failed swap re-enters the handshake on the next poll
// and self-heals once the payload is fixed.
static HotResult perform_swap(HotModule *hot, const char *libDir, uint32_t *outLoaded) {
    HotTrampolineTable *table = &(*hot).trampolines;
    HotRetireRing *ring = &(*hot).retireRing;
    uint32_t loaded = 0;

    DIR *dir = opendir(libDir);
    if (!dir) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "Cannot open manifest library dir: %s", libDir);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    // Phase A — verify (fail-closed: all-or-nothing).
    HotStagedSection staged[HOT_MAX_MODULES];
    uint32_t stagedCount = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = (*ent).d_name;
        size_t nlen = strlen(name);
        bool is_dylib = (nlen > 6 && strcmp(name + nlen - 6, ".dylib") == 0) ||
                        (nlen > 3 && strcmp(name + nlen - 3, ".so") == 0);
        if (!is_dylib)
            continue;
        if (stagedCount >= HOT_MAX_MODULES) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "Too many sections (limit %d) in %s", HOT_MAX_MODULES, libDir);
            return HOT_ERROR_OUT_OF_MEMORY;
        }

        char path[HOT_PATH_LEN];
        int plen = snprintf(path, sizeof(path), "%s/%s", libDir, name);
        if (plen < 0 || (size_t) plen >= sizeof(path)) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "Section path '%s/%s' truncated", libDir, name);
            return HOT_ERROR_FILE_NOT_FOUND;
        }

        void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "dlopen(%s) failed: %s", path, dlerror());
            return HOT_ERROR_DLOPEN_FAILED;
        }

        typedef const void *(*TrampolinesFn)(uint32_t *);
        TrampolinesFn get_trampolines = (TrampolinesFn) dlsym(handle, "VkModuleGetTrampolines");
        if (!get_trampolines) {
            closedir(dir);
            dlclose(handle);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "dlsym(VkModuleGetTrampolines) on %s failed: %s", path, dlerror());
            return HOT_ERROR_DLSYM_FAILED;
        }

        // Section stem = file name without extension, plus the manifest
        // catalog's stem rule ("libbar.so" → "bar") so loader slots and
        // declared sections agree on the same identity.
        char mod_name[HOT_MANIFEST_MAX_NAME];
        strncpy(mod_name, name, HOT_MANIFEST_MAX_NAME - 1);
        mod_name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
        char *dot = strrchr(mod_name, '.');
        if (dot)
            (*dot) = '\0';
        if (strcmp(name + nlen - 3, ".so") == 0 && nlen > 6 &&
            strncmp(mod_name, "lib", 3) == 0)
            memmove(mod_name, mod_name + 3, strlen(mod_name + 3) + 1);

        HotModuleInternal *mod = find_module(hot, mod_name);
        if (!mod) {
            if ((*hot).module_count >= HOT_MAX_MODULES) {
                closedir(dir);
                dlclose(handle);
                for (uint32_t i = 0; i < stagedCount; i++)
                    dlclose(staged[i].handle);
                snprintf((*hot).last_error, sizeof((*hot).last_error),
                         "Too many modules (limit %d reached)", HOT_MAX_MODULES);
                return HOT_ERROR_OUT_OF_MEMORY;
            }
            mod = &(*hot).modules[(*hot).module_count++];
            strncpy((*mod).name, mod_name, HOT_MANIFEST_MAX_NAME - 1);
            (*mod).name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
            (*mod).handle = NULL;
            (*mod).loaded = false;
        }

        HotStagedSection *s = &staged[stagedCount++];
        (*s).mod = mod;
        strncpy((*s).path, path, HOT_PATH_LEN - 1);
        (*s).path[HOT_PATH_LEN - 1] = '\0';
        (*s).handle = handle;
        (*s).trampolines = get_trampolines(&(*s).trampolineCount);
    }
    closedir(dir);

    if (stagedCount == 0) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "No loadable sections in %s", libDir);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    uint64_t newGen = MANIFEST_GENERATION((*hot).library);

    // Phase B — rehydrate the saved state into the STAGED (new) images BEFORE
    // any commit (#8.5 Automated State Rollback). A module that rejects the
    // previous generation's blob fails the whole swap: every staged handle
    // closes, the old generation (code + trampolines + state) stays live, and
    // the generation does NOT advance — the next poll re-enters the handshake
    // and re-attempts once the payload is fixed.
    for (size_t i = 0; i < (*hot).saveCount; i++) {
        HotSaveSlot *slot = &(*hot).saveSlots[i];
        HotStagedSection *match = NULL;
        for (uint32_t k = 0; k < stagedCount; k++) {
            if (strcmp((*staged[k].mod).name, (*slot).name) == 0)
                match = &staged[k];
        }
        if (!match)
            continue; // section absent from the new set; its old slot stays mapped
        typedef bool (*RestoreFn)(const void *, size_t);
        RestoreFn restore = (RestoreFn) dlsym((*match).handle, "Hot_restore");
        bool restored = (restore && restore((*slot).buf, (*slot).len));
        if (!restored && restore) {
            typedef bool (*MigrateFn)(const char *, const void *, size_t, void *, size_t, size_t *);
            MigrateFn migrate = (MigrateFn) dlsym((*match).handle, "Hot_migrate");
            if (migrate) {
                alignas(16) uint8_t migratedBuf[HOT_SAVE_SLOT_CAP];
                size_t migratedLen = 0;
                char oldGenStr[32];
                snprintf(oldGenStr, sizeof(oldGenStr), "%llu.0.0", (unsigned long long) (*hot).generation);
                if ((migrate(oldGenStr, (*slot).buf, (*slot).len, migratedBuf, sizeof(migratedBuf), &migratedLen) ||
                     migrate("1.0.0", (*slot).buf, (*slot).len, migratedBuf, sizeof(migratedBuf), &migratedLen)) &&
                    migratedLen > 0) {
                    restored = restore(migratedBuf, migratedLen);
                }
            }
        }
        if (!restored) {
            for (uint32_t k = 0; k < stagedCount; k++)
                dlclose(staged[k].handle); // never adopted — safe to close directly
            (*hot).saveCount = 0;
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "Restore rejected by %s (%zu bytes) — generation %llu stays live",
                     (*slot).name, (*slot).len, (unsigned long long) (*hot).generation);
            fprintf(stderr, "[hot] ROLLBACK: %s rejected restored state (%zu bytes) — "
                            "keeping generation %llu live, generation NOT advanced\n",
                    (*slot).name, (*slot).len, (unsigned long long) (*hot).generation);
            return HOT_ERROR_RESTORE_FAILED;
        }
    }

    // Phase C — commit: retire old handles, adopt new, swap trampoline rows.
    // State already lives inside the fresh images (Phase B restored in place).
    for (uint32_t i = 0; i < stagedCount; i++) {
        HotStagedSection *s = &staged[i];
        HotModuleInternal *mod = (*s).mod;
        if ((*mod).handle)
            HotRetireRing_retire(ring, (*mod).handle);
        (*mod).handle = (*s).handle;
        (*mod).loaded = true;
        strncpy((*mod).path, (*s).path, HOT_PATH_LEN - 1);
        (*mod).path[HOT_PATH_LEN - 1] = '\0';

        const struct { const char *name; void *fn; } *entries = (*s).trampolines;
        for (uint32_t k = 0; k < (*s).trampolineCount; k++) {
            int tidx = HotTrampolineTable_find(table, entries[k].name);
            if (tidx < 0)
                tidx = HotTrampolineTable_register(table, entries[k].name);
            if (tidx >= 0)
                HotTrampolineTable_set(table, tidx, entries[k].fn);
        }
        loaded++;
        fprintf(stderr, "[hot] reloaded %s (generation %llu)\n",
                (*mod).name, (unsigned long long) newGen);
    }
    (*hot).saveCount = 0;

    (*hot).generation = newGen;

    if (outLoaded)
        (*outLoaded) = loaded;
    return HOT_OK;
}

HotResult Hot_poll(HotModule *hot, uint32_t *loaded_count) {
    if (!hot)
        return HOT_ERROR_FILE_NOT_FOUND;
    if (loaded_count)
        (*loaded_count) = 0;

    HotRetireRing_advance(&(*hot).retireRing);

    char libDir[HOT_PATH_LEN];
    if (!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, (*hot).library,
                                 libDir, sizeof(libDir), false)) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "manifest not mounted or unknown library '%s'", (*hot).library);
        return HOT_ERROR_FILE_NOT_FOUND;
    }
    uint64_t stamp = MANIFEST_GENERATION((*hot).library);

    // Complete an in-flight live swap once the off-thread snapshot landed.
    if ((*hot).swapPending) {
        pthread_mutex_lock(&(*hot).saveLock);
        bool done = (*hot).saveDone;
        pthread_mutex_unlock(&(*hot).saveLock);
        if (done) {
            (*hot).swapPending = false;
            return perform_swap(hot, libDir, loaded_count);
        }
        return HOT_OK; // worker still snapshooting the old generation
    }

    if (stamp == (*hot).generation)
        return HOT_OK;

    if ((*hot).module_count == 0) {
        // First load — nothing to preserve, swap straight through.
        return perform_swap(hot, libDir, loaded_count);
    }

    // Live swap: preserve the CURRENT (old) generation's state off-thread,
    // swap on a later pass. Old handles stay mapped (retire ring) so the
    // worker always reads live state.
    if (snapshot_begin(hot)) {
        (*hot).swapPending = true;
        return HOT_OK;
    }
    return perform_swap(hot, libDir, loaded_count); // nothing save-capable
}

HotFn Hot_get_symbol(HotModule *hot, const char *name) {
    if (!hot || !name)
        return NULL;

    HotTrampolineTable *table = &(*hot).trampolines;
    int tidx = HotTrampolineTable_find(table, name);
    if (tidx < 0)
        return NULL;

    return (HotFn) HotTrampolineTable_get(table, tidx);
}

uint64_t Hot_get_generation(const HotModule *hot) {
    return hot ? (*hot).generation : 0;
}

const char *Hot_last_error(HotModule *hot) {
    if (!hot)
        return "NULL hot module";
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
    typedef bool (*SaveFn)(void *, size_t, size_t *);
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
    typedef bool (*RestoreFn)(const void *, size_t);
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
    typedef bool (*MigrateFn)(const char *, const void *, size_t, void *, size_t, size_t *);
    MigrateFn migrate = (MigrateFn) dlsym((*mod).handle, "Hot_migrate");
    if (!migrate)
        return false;
    return migrate(oldVersion, oldBuf, oldLen, newBuf, newCap, outLen);
}