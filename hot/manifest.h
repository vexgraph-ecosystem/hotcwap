#ifndef HOT_MANIFEST_H
#define HOT_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// hot/manifest.h — Module manifest format.
//
// Each module (dylib) exports a JSON manifest that describes:
//   - name: module name (e.g., "primitive", "vulkan", "buffers")
//   - version: semantic version string (e.g., "1.2.3")
//   - type_ids: array of {name, value, parent?, size?} — the frozen ABI contract
//   - exports: array of function names exported by this module
//   - dependencies: array of module names this module depends on
//   - consumers: provider-owned allow-list of who may bind this module:
//     array of {name, runtime, sections[]} — e.g. {"name":"hotcwap",
//     "runtime":"R1","sections":["arena","rings"]}. The listing itself is the
//     authority: runtime names the supervision level the consumer executes
//     at (documentary — R1 boots first, R5 last); an unlisted consumer
//     is refused. Adding a consumer is policy growth (no code swap); adding
//     API the consumer needs is a code change (versioned, ABI-gated).
//
// A type row states the class identity (value = full 64-bit id), the parent
// class number (parent = index into the repo's own registry, 0 = root), and
// the struct byte size. parent/size are optional on the wire: absent means
// unstated (legacy manifests), never root/zero-sized. has_parent tells the
// two apart; size 0 is never a real struct, so 0 alone means unstated.
//
// On reload, the hotloader verifies (fail-closed, swap time only):
//   1. Every old type name exists in the new manifest with the same value
//   2. Parent chains match wherever both sides state them
//   3. Struct sizes match wherever both sides state them
//   4. One-sided parent/size (stated on exactly one side) refuses — a side
//      that withholds contract info cannot prove compatibility
//   5. All dependencies are loaded and compatible
//   6. The new dylib's init function succeeds
//
// If verification passes, the function pointer table is atomically swapped.
// HotManifest_digest (FNV-1a over name-sorted rows) is the cache key for
// "already validated this exact contract, skip the re-walk" plus the log
// line. The digest never accepts on its own: only a full row walk accepts,
// so a collision can at worst cause a redundant re-walk, never a bad swap.
//
// The consumer allow-list is POLICY, not ABI: it is excluded from both
// compatibility and the digest. The gate (HotManifest_allows) is enforced
// live per bind, before any digest check — and a swap must additionally
// refuse when a currently-bound consumer vanishes from the new manifest
// (that check belongs to the loader, which knows who is bound; the manifest
// layer cannot see it).

#define HOT_MANIFEST_MAX_TYPE_IDS 256
#define HOT_MANIFEST_MAX_EXPORTS 128
#define HOT_MANIFEST_MAX_DEPENDENCIES 16
#define HOT_MANIFEST_MAX_CONSUMERS 16
#define HOT_MANIFEST_MAX_CONSUMER_SECTIONS 8
#define HOT_MANIFEST_MAX_NAME 64
#define HOT_MANIFEST_MAX_VERSION 16
#define HOT_MANIFEST_MAX_RUNTIME 8

// A single type row in the manifest.
typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];   // e.g., "ID_LABEL"
    uint64_t value;                     // e.g., 0x1'00'0'0'0'00'00000005 (full 64-bit id)
    int32_t parent;                     // parent class number, 0 = root (valid iff has_parent)
    bool has_parent;                    // false = unstated on the wire (legacy)
    uint32_t size;                      // sizeof struct in bytes, 0 = unstated on the wire
} HotTypeId;

// A single function export entry.
typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];   // e.g., "Memory_alloc"
} HotExport;

// A single dependency entry.
typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];   // e.g., "primitive"
} HotDependency;

// A single consumer allow-list row: who may bind this module and which API
// sections they may touch. Grant at section granularity (e.g. "arena",
// "rings") — never per-function; per-function rows rot into unmaintained
// spreadsheets. An empty sections list grants nothing (default-deny).
typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];   // e.g., "hotcwap"
    char runtime[HOT_MANIFEST_MAX_RUNTIME]; // e.g., "R1" — documentary only
    uint32_t section_count;
    char sections[HOT_MANIFEST_MAX_CONSUMER_SECTIONS][HOT_MANIFEST_MAX_NAME];
} HotConsumer;

// The full manifest for a module.
typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];
    char version[HOT_MANIFEST_MAX_VERSION];
    
    uint32_t type_id_count;
    HotTypeId type_ids[HOT_MANIFEST_MAX_TYPE_IDS];
    
    uint32_t export_count;
    HotExport exports[HOT_MANIFEST_MAX_EXPORTS];
    
    uint32_t dependency_count;
    HotDependency dependencies[HOT_MANIFEST_MAX_DEPENDENCIES];

    uint32_t consumer_count;
    HotConsumer consumers[HOT_MANIFEST_MAX_CONSUMERS];
} HotManifest;

// Parse a manifest from JSON string.
// Returns true on success, false on parse error.
bool HotManifest_parse(const char *json, size_t len, HotManifest *out);

// Verify that two manifests are ABI-compatible.
// Every old type name must exist in the new manifest with the same value;
// parent chains and struct sizes must match wherever both sides state them,
// and one-sided parent/size refuses (fail-closed). New names in the new
// manifest are allowed (growth); removed or renumbered names refuse.
bool HotManifest_compatible(const HotManifest *old_manifest, const HotManifest *new_manifest);

// Contract digest: FNV-1a over name-sorted rows (name, value, parent, size).
// Cache key + log line only — never the acceptance gate (see above).
// Returns 0 on null input.
uint64_t HotManifest_digest(const HotManifest *manifest);

// Get a type ID by name. Returns 0 if not found.
uint64_t HotManifest_get_type_id(const HotManifest *manifest, const char *name);

// Provider-side allow-list gate: true iff the manifest names consumer with
// section granted. Default-deny: unknown consumer, unlisted section, or any
// null input refuses. A null section asks bind-level existence ("may this
// module bind at all"); a named section asks API-level grant. Enforced live
// per bind, before any digest check.
bool HotManifest_allows(const HotManifest *manifest, const char *consumer, const char *section);

#endif
