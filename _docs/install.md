# The MANIFEST(...) Install Layout — hotcwap's "Manifest Binary Way"

`hot/manifest_path.h/.c` is the per-app **install-layout authority**. The
manifest is the on-disk install tree itself — there is no separate policy
file to keep in sync. `MANIFEST.mf` (the old JSON seed) is retired.

This document describes the process: how the path resolves, what the ladder
holds, how first-run reflection works, and how the two update experiences
(hot swap while running, cold swap on next launch) share the same tree.

---

## 1. The three roots

Every install resolves from one OS root via `ManifestPath_begin`:

| Root | macOS | Windows | Linux |
| :--- | :--- | :--- | :--- |
| `MANIFEST_MAIN_DISK` | `/` (Macintosh HD) | `C:\` | `/` |
| `MANIFEST_USER_HOME` | `/Users/<you>` | `C:\Users\<you>` | `~` |
| `MANIFEST_APP_DATA` | `~/Library/Application Support` | `%LOCALAPPDATA%` | `~/.local/share` (or `$XDG_DATA_HOME`) |

The platform-agnostic base is `#define APPLICATION_PATH`, one `_WIN32` /
`__APPLE__` / else branch in `hot/manifest_path.h`. Building with a custom
deployment? Override the macro at build time. Testing? Any tool that wants
an isolated tree sets **`$VEX_MANIFEST`** to a scratch base — the resolver
honors it first, so a test may mount a ladder in `/tmp` with zero install
risk (this is the seam `manifest_path` smoke tests use).

## 2. `MANIFEST(...)` — one-time init

```c
bool MANIFEST(const char *first, ...);   // varargs, NULL-terminated
```

```c
MANIFEST("semicolon", (const char*) 0);
// → macOS:   ~/Library/Application Support/vexgraph/semicolon/
// → Windows: %LOCALAPPDATA%\vexgraph\semicolon\
```

- The org segment (`MANIFEST_ORG` = `vexgraph`) is **always** appended if
  missing — the ecosystem nests every install under one org folder.
- Each vararg segment is appended **and its directory created** as it goes
  ("will make a folder during that time").
- **`MANIFEST(...)` is one-shot.** A second call returns `false`. The root
  locks for the whole process — two launchers can never mount the same
  ladder. On failure the mount clears and the app may retry; after success
  the root is frozen.
- `MANIFEST_ROOT()` returns the locked buffer (`nullptr` before the init).

## 3. The ladder

`MANIFEST_ENSURE()` creates the full tree:

```
<root>/vexgraph/<app>/
├── bin/
│   ├── backward/    ← oldest retained set (rollback keeper)
│   ├── previous/    ← prior generation (rollback)
│   ├── current/     ← the runnable set — what the launcher dlopens
│   └── new/         ← staged future set (downloads land here)
├── hot/             ← MODE-1 watch dir: Hot_poll watches here for dylibs
└── cache/           ← cache system
```

Slot paths via `ManifestPath_ladderDir(slot, dest, cap, create)` where
`slot` is `MANIFEST_LADDER_BACKWARD|PREVIOUS|CURRENT|NEW`; `ManifestPath_hotDir`
and `ManifestPath_cacheDir` resolve the other two.

## 4. First-run reflection (`MANIFEST_IS_FIRST_RUN` / `MANIFEST_REFLECT`)

First time the process opens (no `<current>/.install-mark`), the launcher:

1. `MANIFEST_REFLECT(sourceDir)` — copy `sourceDir` payloads into
   `bin/current` via per-file temp + atomic `rename` (`.name.reflect` →
   `name`), then drop the fingerprint `MANIFEST_MARK`.
2. `MANIFEST_IS_FIRST_RUN()` flips from `true` to `false`.

The `.app` or `.exe` shell stays **frozen** — it is the loadable stub. The
behavior — all dylibs, `.spv` blobs, fonts, config — lives in the manifest
tree under `current/`. The OS binary never mutates on disk; the behavior
set does. (`MANIFEST_REFLECT` is idempotent via the mark; on copy failure
it returns `false` and the launcher retries next launch.)

## 5. Two update experiences (the "hot c wap")

Both modes share the ladder and the same verification primitive
(`HotStage_verify`: `hot.manifest` schema + `files.sha` FNV-1a integrity).

### MODE 1 — HOT SWAP (app running)

Updates land in `bin/new`, `MANIFEST_UPDATE()` verifies the staged set,
then the existing `hot/hot.c` pipeline hot-loads it **in-place** —
`clone → dlopen → ABI gate → save state → trampoline swap → init new →
restore state → shutdown old → retire ring (4-poll grace)`. Zero restart,
`current/` stays pinned during the swap.

### MODE 2 — COLD SWAP (app closed)

The staged set sits verified in `bin/new`. On exit (or guarded wipe of the
live set) the launcher calls:

```c
bool ok = MANIFEST_UPDATE();   // verify staged set — never promotes alone
bool ok = MANIFEST_PROMOTE();  // slide the ladder:
                               //   previous → backward (oldest dropped)
                               //   current  → previous
                               //   new      → current
```

- `MANIFEST_PROMOTE` is **renames only** — atomic on the same filesystem,
  never a copy, never a partial bind. Each step either completes or returns
  `false` with the prior generation still intact.
- With nothing staged it is an idempotent no-op (returns `true`).
- If everything compiles and verifies, the *next launch loads the new set*.
  The `.app` shell you run again is the same stub; the `current/` set it
  reflect-loads is the new binary — "after we exit the application, there
  will now be .exe or .app file that is at the latest version" is exactly
  this: not by rewriting the OS binary (impossible — mmap pins it), but by
  sliding the layout the stub resolves.

## 6. The old policy file

`MANIFEST.mf` (repo root) and `spoke/MANIFEST.md` are retired. The
`consumers[]` allow-list that governed which projects may bind vexspoke now
reads as a **documented baseline** in `spoke/vexspoke.h` — the authority for
what may board is the on-disk install ladder plus the `HotManifest` ABI gate,
never a JSON seed sitting next to the source.

## 7. Cold boundary

This is a cold boundary per the Cold-Strict, Hot-Minimal Validation Law:
`MANIFEST` / `REFLECT` / `UPDATE` / `PROMOTE` validate exhaustively
(null, bounds, mount state, mkdir/rename failures) and return `false` with
zero partial state; the hot `Hot_poll` path stays minimal. No function in
this unit blocks unboundedly or allocates outside stack buffers.