#include "hot/manifest_path.h"
#include "hot/stage.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#  define RENAME(a, b) (MoveFileExA((a), (b), MOVEFILE_REPLACE_EXISTING) != 0)
#  define UNLINK(a)    _unlink(a)
#else
#  include <unistd.h>
#  define RENAME(a, b) (rename((a), (b)) == 0)
#  define UNLINK(a)    unlink(a)
#endif

#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: ManifestPath (hot/manifest_path.c)
 * LEVEL: L4 — Self-Management (owns the per-app install layout on disk)
 * ============================================================================
 * The MANIFEST(...) install-layout authority — the "manifest binary way".
 * The manifest IS the on-disk install tree: MANIFEST(...) resolves
 * <application-base>/<org>/<app> from the platform application-data base,
 * creating directories as it goes, and the MANIFEST_* verbs walk the ladder.
 * MANIFEST.mf (the JSON policy seed) is retired — no separate policy file.
 *
 * STRUCT FIELDS (Mirroring hot/manifest_path.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char  *buf;    // caller-owned destination buffer
 *   size_t cap;    // capacity of buf
 *   size_t len;    // current path length, excluding the trailing NUL
 *
 * PRIVATE HELPERS (file-local pure-data only):
 * ----------------------------------------------------------------------------
 *   static char  g_root[MANIFEST_BUF_CAP];   // the locked install root
 *   static bool  g_mounted;                  // MANIFEST() one-time guard
 *   static bool  dir_exists(const char *path);
 *   static bool  dir_mkdir(const char *path);        // mkdir one level
 *   static bool  mkdir_p(const char *path);          // mkdir whole tree
 *   static bool  resolve_base(char *out, size_t cap); // platform base path
 *   static bool  copy_file(const char *src, const char *dst);
 *   static bool  file_exists(const char *path);
 *   static void  remove_ladder_dir(const char *dir); // recursive rm via rename+globe
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - ManifestPath(dest, cap)            : ManifestPath_0(dest, cap)
 *   - MANIFEST(app, ...)                 : one-shot init + mkdir ladder root
 *
 * Core Functions:
 *   - MANIFEST_ROOT()
 *   - MANIFEST_ENSURE()
 *   - MANIFEST_REFLECT(sourceDir)
 *   - MANIFEST_UPDATE()
 *   - MANIFEST_PROMOTE()
 *   - MANIFEST_IS_FIRST_RUN()
 *
 * Path Builders:
 *   - ManifestPath_push(self, segment, create)
 *   - ManifestPath_begin(self, kind)
 *   - ManifestPath_ladderDir(slot, dest, cap, create)
 *   - ManifestPath_hotDir(dest, cap)
 *   - ManifestPath_cacheDir(dest, cap)
 *
 * Getters:
 *   - ManifestPath_get(const self)
 *   - ManifestPath_len(const self)
 * ============================================================================
 */

#define MANIFEST_BUF_CAP 1024
#define MANIFEST_BIN "bin"
#define MANIFEST_HOT "hot"
#define MANIFEST_CACHE "cache"
#define SLOT_NAMES { "backward", "previous", "current", "new" }
#define ROTATION_DIR  ".promote_rot"

static char g_root[MANIFEST_BUF_CAP];
static bool g_mounted = false;

// --- private helpers ---------------------------------------------------------

static bool dir_exists(const char *path) {
    if (path == nullptr || *path == '\0')
        return false;
    struct stat st;
    memset(&st, 0, sizeof(st));
    return stat(path, &st) == 0 && S_ISDIR((unsigned int) st.st_mode);
}

static bool file_exists(const char *path) {
    if (path == nullptr || *path == '\0')
        return false;
    struct stat st;
    memset(&st, 0, sizeof(st));
    return stat(path, &st) == 0 && S_ISREG((unsigned int) st.st_mode);
}

static bool dir_mkdir(const char *path) {
    if (dir_exists(path))
        return true;
#if defined(_WIN32)
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0755);
#endif
    if (rc != 0 && errno != EEXIST)
        return false;
    return dir_exists(path);
}

// mkdir -p: walk each '/' prefix and create it in order.
static bool mkdir_p(const char *path) {
    if (path == nullptr || *path == '\0')
        return false;
    char tmp[MANIFEST_BUF_CAP];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    while (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[--len] = '\0';
    size_t i = 1;
    for (; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            if (!dir_mkdir(tmp))
                return false;
            tmp[i] = '/';
        }
    }
    return dir_mkdir(tmp);
}

// Recursive rm — rename-based: rename dir to a hidden sibling then sweep
// everything under it. No system(), bounded per-entry unlinks.
static void remove_ladder_dir(const char *dir) {
    if (!dir_exists(dir))
        return;
    char tomb[MANIFEST_BUF_CAP];
    snprintf(tomb, sizeof(tomb), "%s" ROTATION_DIR, dir);
    RENAME(dir, tomb);
    if (!dir_exists(tomb))
        return;
    DIR *d = opendir(tomb);
    if (!d)
        return;
    struct dirent *ent;
    char entry[MANIFEST_BUF_CAP];
    while ((ent = readdir(d)) != nullptr) {
        const char *name = (*ent).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        snprintf(entry, sizeof(entry), "%s/%s", tomb, name);
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (lstat(entry, &st) == 0 && S_ISDIR((unsigned int) st.st_mode))
            remove_ladder_dir(entry);
        else
            UNLINK(entry);
    }
    closedir(d);
    rmdir(tomb);
}

static bool resolve_base(char *out, size_t cap) {
    // $VEX_MANIFEST overrides the whole base (test seam).
    const char *override = getenv("VEX_MANIFEST");
    if (override && *override != '\0') {
        snprintf(out, cap, "%s", override);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || *home == '\0')
        home = ".";
#if defined(_WIN32)
    const char *local = getenv("LOCALAPPDATA");
    if (local && *local != '\0')
        snprintf(out, cap, "%s", local);
    else {
        const char *profile = getenv("USERPROFILE");
        snprintf(out, cap, "%s", (profile && *profile != '\0') ? profile : home);
    }
#elif defined(__APPLE__)
    snprintf(out, cap, "%s/%s", home, APPLICATION_PATH);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg != '\0')
        snprintf(out, cap, "%s", xdg);
    else
        snprintf(out, cap, "%s/%s", home, APPLICATION_PATH);
#endif
    return true;
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    uint8_t buf[4096];
    size_t n = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

// --- constructors ------------------------------------------------------------

ManifestPath ManifestPath_0(char *dest, size_t cap) {
    ManifestPath self;
    self.buf = dest;
    self.cap = cap;
    self.len = 0;
    if (dest && cap > 0)
        (*dest) = '\0';
    return self;
}

bool MANIFEST(const char *first, ...) {
    if (first == nullptr)
        return false;
    if (g_mounted)
        return false;

    char base[MANIFEST_BUF_CAP];
    if (!resolve_base(base, sizeof(base)))
        return false;

    ManifestPath path = ManifestPath_0(g_root, sizeof(g_root));
    if (!ManifestPath_begin(&path, MANIFEST_APP_DATA))
        return false;
    if (!ManifestPath_push(&path, MANIFEST_ORG, true))
        return false;

    const char *segment = first;
    va_list ap;
    va_start(ap, first);
    while (segment != nullptr) {
        if (*segment != '\0' && !ManifestPath_push(&path, segment, true))
            break;
        segment = va_arg(ap, const char*);
    }
    va_end(ap);
    if (segment != nullptr) {
        g_root[0] = '\0';
        g_mounted = false;
        return false;
    }

    g_mounted = true;
    return true;
}

// --- core functions ----------------------------------------------------------

const char *MANIFEST_ROOT(void) {
    return g_mounted ? g_root : nullptr;
}

bool MANIFEST_ENSURE(void) {
    if (!g_mounted)
        return false;
    char path[MANIFEST_BUF_CAP];
    static const char *slots[] = SLOT_NAMES;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        if (!ManifestPath_ladderDir((ManifestLadder) i, path, sizeof(path), true))
            return false;
    }
    if (!ManifestPath_hotDir(path, sizeof(path)))
        return false;
    if (!dir_mkdir(path))
        return false;
    if (!ManifestPath_cacheDir(path, sizeof(path)))
        return false;
    return dir_mkdir(path);
}

bool MANIFEST_REFLECT(const char *sourceDir) {
    if (!g_mounted || sourceDir == nullptr)
        return false;

    char dest_dir[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(MANIFEST_LADDER_CURRENT, dest_dir, sizeof(dest_dir), true))
        return false;

    DIR *dir = opendir(sourceDir);
    if (!dir)
        return false;
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        const char *name = (*ent).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char src[MANIFEST_BUF_CAP];
        char tmp[MANIFEST_BUF_CAP];
        char dst[MANIFEST_BUF_CAP];
        snprintf(src, sizeof(src), "%s/%s", sourceDir, name);
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (stat(src, &st) != 0 || !S_ISREG((unsigned int) st.st_mode))
            continue;
        snprintf(tmp, sizeof(tmp), "%s/.%s.reflect", dest_dir, name);
        snprintf(dst, sizeof(dst), "%s/%s", dest_dir, name);
        if (!copy_file(src, tmp)) {
            ok = false;
            break;
        }
        if (!RENAME(tmp, dst)) {
            UNLINK(tmp);
            ok = false;
            break;
        }
    }
    closedir(dir);
    if (!ok)
        return false;

    char mark[MANIFEST_BUF_CAP];
    snprintf(mark, sizeof(mark), "%s/%s", dest_dir, MANIFEST_MARK);
    FILE *f = fopen(mark, "wb");
    if (!f)
        return false;
    fputs("vexgraph install reflection\n", f);
    fclose(f);
    return true;
}

bool MANIFEST_UPDATE(void) {
    if (!g_mounted)
        return false;
    char new_dir[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(MANIFEST_LADDER_NEW, new_dir, sizeof(new_dir), false))
        return false;
    // The staged set is verified as a HotStage package (hot.manifest +
    // files.sha + payloads). Verify only — never promote on this verb.
    return HotStage_verify(new_dir, nullptr, 0);
}

bool MANIFEST_PROMOTE(void) {
    if (!g_mounted)
        return false;

    char backward[MANIFEST_BUF_CAP];
    char previous[MANIFEST_BUF_CAP];
    char current[MANIFEST_BUF_CAP];
    char new_dir[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(MANIFEST_LADDER_BACKWARD, backward, sizeof(backward), false) ||
        !ManifestPath_ladderDir(MANIFEST_LADDER_PREVIOUS, previous, sizeof(previous), false) ||
        !ManifestPath_ladderDir(MANIFEST_LADDER_CURRENT, current, sizeof(current), false) ||
        !ManifestPath_ladderDir(MANIFEST_LADDER_NEW, new_dir, sizeof(new_dir), false))
        return false;

    if (!dir_exists(new_dir))
        return true; // nothing staged → idempotent no-op

    // 1. drop the oldest rollback set.
    remove_ladder_dir(backward);

    // 2. slide: previous → backward, current → previous.
    if (dir_exists(previous) && !RENAME(previous, backward))
        return false;
    if (dir_exists(current) && !RENAME(current, previous))
        return false;

    // 3. promote new → current.
    if (!RENAME(new_dir, current))
        return false;

    return true;
}

bool MANIFEST_IS_FIRST_RUN(void) {
    if (!g_mounted)
        return true; // fail-closed: no mounted manifest means never installed
    char current[MANIFEST_BUF_CAP];
    char mark[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(MANIFEST_LADDER_CURRENT, current, sizeof(current), false))
        return true;
    snprintf(mark, sizeof(mark), "%s/%s", current, MANIFEST_MARK);
    return !file_exists(mark);
}

// --- path builders -----------------------------------------------------------

bool ManifestPath_begin(ManifestPath *self, ManifestRoot kind) {
    if (self == nullptr || (*self).buf == nullptr || (*self).cap == 0)
        return false;
    char base[MANIFEST_BUF_CAP];
    if (!resolve_base(base, sizeof(base)))
        return false;
    (void) kind; // all three roots resolve from the platform base today
    snprintf((*self).buf, (*self).cap, "%s", base);
    (*self).len = strlen((*self).buf);
    return true;
}

bool ManifestPath_push(ManifestPath *self, const char *segment, bool create) {
    if (self == nullptr || segment == nullptr || (*self).buf == nullptr)
        return false;
    size_t seg_len = strlen(segment);
    size_t need = (*self).len + 1 + seg_len + 1;
    if (need > (*self).cap)
        return false;
    (*self).buf[(*self).len] = '/';
    memcpy((*self).buf + (*self).len + 1, segment, seg_len);
    (*self).len += 1 + seg_len;
    (*self).buf[(*self).len] = '\0';
    if (create && !mkdir_p((*self).buf))
        return false;
    return true;
}

bool ManifestPath_ladderDir(ManifestLadder slot, char *dest, size_t cap, bool create) {
    if (dest == nullptr || cap == 0)
        return false;
    static const char *slot_names[] = SLOT_NAMES;
    if (slot < MANIFEST_LADDER_BACKWARD || slot > MANIFEST_LADDER_NEW)
        return false;
    if (!g_mounted)
        return false;
    size_t root_len = strlen(g_root);
    if (root_len + 1 + 3 + 1 + strlen(slot_names[slot]) + 1 > cap)
        return false;
    memcpy(dest, g_root, root_len);
    dest[root_len] = '\0';
    ManifestPath p;
    p.buf = dest;
    p.cap = cap;
    p.len = root_len;
    if (!ManifestPath_push(&p, MANIFEST_BIN, create))
        return false;
    if (!ManifestPath_push(&p, slot_names[slot], create))
        return false;
    return true;
}

bool ManifestPath_hotDir(char *dest, size_t cap) {
    if (dest == nullptr || cap == 0)
        return false;
    if (!g_mounted)
        return false;
    size_t root_len = strlen(g_root);
    if (root_len + 1 + strlen(MANIFEST_HOT) + 1 > cap)
        return false;
    memcpy(dest, g_root, root_len);
    dest[root_len] = '\0';
    ManifestPath p;
    p.buf = dest;
    p.cap = cap;
    p.len = root_len;
    return ManifestPath_push(&p, MANIFEST_HOT, false);
}

bool ManifestPath_cacheDir(char *dest, size_t cap) {
    if (dest == nullptr || cap == 0)
        return false;
    if (!g_mounted)
        return false;
    size_t root_len = strlen(g_root);
    if (root_len + 1 + strlen(MANIFEST_CACHE) + 1 > cap)
        return false;
    memcpy(dest, g_root, root_len);
    dest[root_len] = '\0';
    ManifestPath p;
    p.buf = dest;
    p.cap = cap;
    p.len = root_len;
    return ManifestPath_push(&p, MANIFEST_CACHE, false);
}

// --- getters -----------------------------------------------------------------

const char *ManifestPath_get(const ManifestPath *self) {
    return self == nullptr || (*self).buf == nullptr ? nullptr : (*self).buf;
}

size_t ManifestPath_len(const ManifestPath *self) {
    return self == nullptr ? 0 : (*self).len;
}