#ifndef HOT_MANIFEST_H
#define HOT_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// hot/manifest.h — the MANIFEST(...) install-layout authority.
//
// The "manifest binary way": the manifest IS the on-disk install tree, not
// a JSON policy seed. MANIFEST(...) resolves the per-app install root from
// the platform application-data base, creating directories as it goes, and
// the MANIFEST_* verbs walk that ladder. MANIFEST.mf is retired — there is
// no separate policy file to keep in sync.
//
// SCREAMING_CASE surface, hotcwap-style:
//   MANIFEST("semicolon", (const char*) 0)   // ONE-TIME init (see below)
//   MANIFEST_UPDATE()                        // verify staged set (bin/new)
//   MANIFEST_PROMOTE()                       // new → current finale
//   MANIFEST_ENSURE()                        // create the full ladder
//
// Per-app layout under APPLICATION_DATA + org + <app>:
//   bin/backward    oldest retained set (rollback)
//   bin/previous    prior generation (rollback)
//   bin/current     the runnable set — what the launcher dlopens
//   bin/new         staged future set (downloads land here)
//   hot/            live hot-swap dir — the Mode-1 Hot_poll watch dir
//   cache/          cache system
//
// Two experiences share this tree: MODE 1 (app RUNNING) swaps dylibs live
// via Hot_poll while current/ stays pinned; MODE 2 (app CLOSED) promotes
// new/ → current/ with renames so the next launch is the new binary.
// See docs/install.md for the full process.

// --- Hotloading constants (consumed by hot/ trampolines and module slots) ----

#define HOT_MANIFEST_MAX_NAME 64
#define HOT_MANIFEST_MAX_EXPORTS 128

// --- Path roots & base defines ----------------------------------------------

// Per-platform application-install base, overridable at build time
// (custom deployments) or by $VEX_MANIFEST at runtime (test seam).
#if defined(_WIN32)
#  define APPLICATION_PATH "%LOCALAPPDATA%"
#elif defined(__APPLE__)
#  define APPLICATION_PATH "Library/Application Support"
#else
#  define APPLICATION_PATH ".local/share"
#endif

// The ecosystem org folder under the application-data base.
#define MANIFEST_ORG "vexgraph"

// Install fingerprint file dropped in bin/current after first-run reflection.
#define MANIFEST_MARK ".install-mark"

// OS roots (MANIFEST_MAIN_DISK / MANIFEST_USER_HOME available for custom
// layouts; the default MANIFEST() init mounts on MANIFEST_APP_DATA).
typedef enum ManifestRoot {
    MANIFEST_MAIN_DISK = 0,
    MANIFEST_USER_HOME,
    MANIFEST_APP_DATA,
} ManifestRoot;

// Ladder slots, oldest → newest staging. VERBS promote NEW → CURRENT;
// CURRENT → PREVIOUS → BACKWARD keeps the rollback sets.
typedef enum ManifestLadder {
    MANIFEST_LADDER_BACKWARD = 0,
    MANIFEST_LADDER_PREVIOUS,
    MANIFEST_LADDER_CURRENT,
    MANIFEST_LADDER_NEW,
} ManifestLadder;

// The class: a path being built under a manifest root. A thin builder that
// owns a caller buffer pointer + current length, never the memory.
typedef struct ManifestPath {
    char *buf;    // caller-owned destination buffer
    size_t cap;   // capacity of buf
    size_t len;   // current path length, excluding the trailing NUL
} ManifestPath;

// Constructors:
//   ManifestPath(dest, cap)           — bind a builder to a caller buffer.
//   MANIFEST(app, ...)                — one-shot init + create dirs (below).

ManifestPath ManifestPath_0(char *dest, size_t cap);

// --- CORE FUNCTIONS ----------------------------------------------------------

// ONE-TIME initializer. MANIFEST(app, ...) — varargs, must end with
// (const char*) 0 — resolves <application-base>/<org>/<app>/..., creating
// each directory as it goes, and locks the root for every MANIFEST_* verb.
//
//   MANIFEST("semicolon", (const char*) 0)
//   → macOS:  ~/Library/Application Support/vexgraph/semicolon
//   → Windows: %LOCALAPPDATA%\vexgraph\semicolon
//
// THE SECOND CALL FAILS (returns false). The manifest initializes once —
// two launchers must never mount the same ladder. Returns false on a
// second call or any resolution/mkdir failure.
bool MANIFEST(const char *first, ...);

// Return the locked root (<application-base>/<org>/<app>). Buffer valid until
// the next ManifestPath call. Returns nullptr when MANIFEST() never ran.
const char *MANIFEST_ROOT(void);

// Create the whole per-app ladder (bin/{backward,previous,current,new},
// hot, cache). Idempotent. Returns true when every dir exists.
bool MANIFEST_ENSURE(void);

// Reflect binaries: copy `sourceDir` payloads into bin/current (the first-run
// install step). Idempotent via the fingerprint in MANIFEST_MARK. Returns
// false on any copy failure or when the fingerprint cannot be written — the
// launcher retries next launch.
bool MANIFEST_REFLECT(const char *sourceDir);

// Update staging: verify the staged set in bin/new has content (at least one
// regular file). Vexspoke validates content before placing — this verb
// checks the ladder state. Verify only — never promotes. Returns true when
// a promotion is safe.
bool MANIFEST_UPDATE(void);

// MODE-2 finale: promote the ladder (renames, same filesystem, atomic).
//   current → previous → backward   (rollback sets slide, oldest dropped)
//   new     → current
// Returns false if any rename fails (nothing is half-applied).
bool MANIFEST_PROMOTE(void);

// First-run detection: true when the tree was never installed (no
// bin/current/<MANIFEST_MARK>). The launcher must MANIFEST_REFLECT before run.
bool MANIFEST_IS_FIRST_RUN(void);

// --- PATH BUILDERS (dest-last) ----------------------------------------------

// Push a segment onto a bound builder. When `create` is true the segment's
// directory is created as it is appended ("will make a folder during that
// time"). Returns false on overflow / mkdir failure.
bool ManifestPath_push(ManifestPath *self, const char *segment, bool create);

// Resolve an OS root into a builder (no mkdir — the root always exists).
bool ManifestPath_begin(ManifestPath *self, ManifestRoot kind);

// Ladder dir <locked root>/bin/<slot>. create=true also mkdirs it.
bool ManifestPath_ladderDir(ManifestLadder slot, char *dest, size_t cap, bool create);

// hot/ and cache/ dirs under the locked root.
bool ManifestPath_hotDir(char *dest, size_t cap);
bool ManifestPath_cacheDir(char *dest, size_t cap);

// --- GETTERS -----------------------------------------------------------------

// Null-safe accessors (the Symmetric Getter/Setter Completeness Law).
const char *ManifestPath_get(const ManifestPath *self);
size_t ManifestPath_len(const ManifestPath *self);

#endif