#include "hot/hot_trampoline.h"

#include <string.h>

#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: HotTrampolineTable
 * ============================================================================
 * Atomic indirect call-dispatch table for hot-swappable module exports. Provides
 * transparent trampoline redirection so callers invoke stable function addresses
 * while the underlying implementation dylib addresses swap dynamically in memory.
 *
 * Holds an array of up to 1024 symbol rows, each tracking an atomic current pointer
 * and a fallback pointer from the prior generation. During active module swaps, the
 * fallback pointer guarantees callers landing mid-swap never encounter a NULL pointer.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: HotTrampolineTable (hot/hot_trampoline.c)
 * LEVEL: L4 — Self-Management (per-instance swap machinery the loader stands on)
 * ============================================================================
 * SUMMARY:
 *   One atomic function-pointer table per HotModule instance. Two loaders
 *   share this code but resolve through their own tables — same symbol,
 *   different targets, zero collision. Reload swaps a row's ptr while
 *   fallback_ptr covers mid-swap readers.
 *
 * STRUCT FIELDS (Mirroring hot/hot_trampoline.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   HotTrampoline rows[HOT_MAX_TRAMPOLINES]; // one row per export (max 1024)
 *   _Atomic uint32_t count;                  // used rows (cross-thread readers)
 *
 * SLOT RECORD (public, behaviorless, owned by this table):
 * ----------------------------------------------------------------------------
 *   HotTrampoline (one row per exported symbol):
 *     _Atomic(void*) ptr;                    // current generation target
 *     _Atomic(void*) fallback_ptr;           // prior generation (mid-swap cover)
 *     char name[HOT_MANIFEST_MAX_NAME];      // export symbol name
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - (none)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - HotTrampolineTable_register(self, name) : Allocate a row, returns index
 *   - HotTrampolineTable_set(self, idx, ptr)  : Atomic swap, stash old as fallback
 *   - HotTrampolineTable_find(self, name)     : Index by symbol name, -1 if absent
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - HotTrampolineTable_get(self, idx)       : Current ptr with fallback cover
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// CORE FUNCTIONS (PUBLIC & PRIVATE)
// Register a row for a function. Returns the row index.
int HotTrampolineTable_register(HotTrampolineTable *self, const char *name) {
    if (!self || !name) return -1;
    uint32_t idx = atomic_fetch_add(&(*self).count, 1);
    if (idx >= HOT_MAX_TRAMPOLINES) return -1;
    HotTrampoline *row = &(*self).rows[idx];
    strncpy((*row).name, name, HOT_MANIFEST_MAX_NAME - 1);
    (*row).name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
    atomic_store(&(*row).ptr, NULL);
    atomic_store(&(*row).fallback_ptr, NULL);
    return (int)idx;
}

// Set a row's function pointer atomically.
void HotTrampolineTable_set(HotTrampolineTable *self, int idx, void *ptr) {
    if (!self) return;
    if (idx < 0 || idx >= (int)atomic_load(&(*self).count)) return;
    HotTrampoline *row = &(*self).rows[idx];
    void *old = atomic_load(&(*row).ptr);
    if (old && old != ptr) {
        atomic_store(&(*row).fallback_ptr, old);
    }
    atomic_store(&(*row).ptr, ptr);
}

// Find a row by name. Returns -1 if not found.
int HotTrampolineTable_find(HotTrampolineTable *self, const char *name) {
    if (!self || !name) return -1;
    uint32_t count = atomic_load(&(*self).count);
    for (uint32_t i = 0; i < count; i++) {
        HotTrampoline *row = &(*self).rows[i];
        if (strcmp((*row).name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
// Get a row's current function pointer, falling back to prior generation on mid-swap NULL.
void *HotTrampolineTable_get(HotTrampolineTable *self, int idx) {
    if (!self) return NULL;
    if (idx < 0 || idx >= (int)atomic_load(&(*self).count)) return NULL;
    HotTrampoline *row = &(*self).rows[idx];
    void *ptr = atomic_load(&(*row).ptr);
    if (!ptr) {
        // Poll briefly in case atomic swap is in-flight
        for (int retry = 0; retry < 4 && !ptr; retry++) {
            #if defined(__aarch64__)
            __asm__ volatile("yield");
            #endif
            ptr = atomic_load(&(*row).ptr);
        }
        // Fall back to prior generation rather than returning NULL and crashing caller
        if (!ptr) {
            ptr = atomic_load(&(*row).fallback_ptr);
        }
    }
    return ptr;
}
