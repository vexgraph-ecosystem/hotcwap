// hot/ledger.c — the machine-scoped install ledger (R1 host).
//
// Records, OUTSIDE the install tree, that an application (org/app) has been
// installed on this machine — and survives UNINSTALL — in a per-user state
// file:
//   macOS   ~/Library/Application Support/vexgraph/ledger/<org>_<app>.rec
//   Windows %LOCALAPPDATA%\vexgraph\ledger\<org>_<app>.rec
//   Linux   $XDG_STATE_HOME/vexgraph/ledger/<org>_<app>.rec
//
// No OS secure store, no registry: a plain file outside the tree. This is a
// CORRECTNESS record, not a security boundary — a local user can always delete
// it. Its promise is "the machine remembers this app was here", not "the
// machine cannot forget". The in-tree manifest.json stays the ownership marker
// for safe deletion; this ledger is the machine's out-of-tree memory.

#include "hot/ledger.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct Ledger {
    char org[64];
    char app[64];
    char path[512];
    char version[64];
    LedgerState state;
};

// --- the state directory (per user, outside any install tree) ----------------

static bool state_dir(char *dest, size_t cap) {
#if defined(_WIN32)
    const char *base = getenv("LOCALAPPDATA");
    if (base == nullptr || *base == '\0')
        base = getenv("USERPROFILE");
    if (base == nullptr || *base == '\0')
        return false;
    int n = snprintf(dest, cap, "%s/vexgraph/ledger", base);
    return n > 0 && (size_t) n < cap;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (home == nullptr || *home == '\0')
        return false;
    int n = snprintf(dest, cap, "%s/Library/Application Support/vexgraph/ledger", home);
    return n > 0 && (size_t) n < cap;
#else
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        int n = snprintf(dest, cap, "%s/vexgraph/ledger", xdg);
        return n > 0 && (size_t) n < cap;
    }
    const char *home = getenv("HOME");
    if (home == nullptr || *home == '\0')
        return false;
    int n = snprintf(dest, cap, "%s/.local/state/vexgraph/ledger", home);
    return n > 0 && (size_t) n < cap;
#endif
}

static bool mkdir_p(const char *path) {
    char tmp[512];
    size_t len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len == 0 || len >= sizeof(tmp))
        return false;
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return false;
        tmp[i] = '/';
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

// Sanitize org/app into one filename-safe key (non [alnum_] → '_').
static void sanitize(const char *src, char *dest, size_t cap) {
    size_t k = 0;
    for (const char *p = src; *p != '\0' && k + 1 < cap; p++)
        dest[k++] = (isalnum((unsigned char) *p) || *p == '_') ? *p : '_';
    dest[k] = '\0';
}

static bool record_path(char *dest, size_t cap, const char *org, const char *app) {
    char dir[400];
    if (!state_dir(dir, sizeof(dir)))
        return false;
    char o[64], a[64];
    sanitize(org, o, sizeof(o));
    sanitize(app, a, sizeof(a));
    int n = snprintf(dest, cap, "%s/%s_%s.rec", dir, o, a);
    return n > 0 && (size_t) n < cap;
}

// --- the class ---------------------------------------------------------------

static void apply_record(Ledger *ledger, const char *record) {
    const char *nl = strchr(record, '\n');
    if (nl == nullptr)
        return;
    size_t slen = (size_t) (nl - record);
    if (slen == 0 || slen >= 16)
        return;
    if (slen == 9 && strncmp(record, "installed", 9) == 0)
        (*ledger).state = LEDGER_INSTALLED;
    else if (slen == 11 && strncmp(record, "uninstalled", 11) == 0)
        (*ledger).state = LEDGER_UNINSTALLED;
    else
        return;
    const char *ver = nl + 1;
    const char *nl2 = strchr(ver, '\n');
    size_t vlen = nl2 ? (size_t) (nl2 - ver) : strlen(ver);
    if (vlen >= sizeof((*ledger).version))
        vlen = sizeof((*ledger).version) - 1;
    memcpy((*ledger).version, ver, vlen);
    (*ledger).version[vlen] = '\0';
}

Ledger *Ledger_open(const char *org, const char *app) {
    if (org == nullptr || app == nullptr || *org == '\0' || *app == '\0')
        return nullptr;
    Ledger *ledger = calloc(1, sizeof(*ledger));
    if (ledger == nullptr)
        return nullptr;
    snprintf((*ledger).org, sizeof((*ledger).org), "%s", org);
    snprintf((*ledger).app, sizeof((*ledger).app), "%s", app);
    (*ledger).state = LEDGER_ABSENT;
    (*ledger).version[0] = '\0';
    if (!record_path((*ledger).path, sizeof((*ledger).path), org, app)) {
        free(ledger);
        return nullptr;
    }

    FILE *f = fopen((*ledger).path, "rb");
    if (f != nullptr) {
        char buf[256];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        apply_record(ledger, buf);
    }
    return ledger;
}

void Ledger_free(Ledger *ledger) {
    free(ledger);
}

static bool write_state(Ledger *ledger, const char *state, const char *version) {
    char dir[400];
    if (!state_dir(dir, sizeof(dir)) || !mkdir_p(dir))
        return false;
    const char *ver = version ? version : "";
    char record[192];
    int n = snprintf(record, sizeof(record), "%s\n%s\n", state, ver);
    if (n < 0 || (size_t) n >= sizeof(record))
        return false;
    FILE *f = fopen((*ledger).path, "wb");
    if (f == nullptr)
        return false;
    bool ok = (fwrite(record, 1, (size_t) n, f) == (size_t) n);
    fclose(f);
    if (!ok)
        return false;
    (*ledger).state = (strcmp(state, "installed") == 0) ? LEDGER_INSTALLED : LEDGER_UNINSTALLED;
    snprintf((*ledger).version, sizeof((*ledger).version), "%s", ver);
    return true;
}

bool Ledger_recordInstall(Ledger *ledger, const char *version) {
    if (ledger == nullptr)
        return false;
    return write_state(ledger, "installed", version);
}

bool Ledger_markUninstalled(Ledger *ledger) {
    if (ledger == nullptr)
        return false;
    return write_state(ledger, "uninstalled", (*ledger).version);
}

bool Ledger_forget(Ledger *ledger) {
    if (ledger == nullptr)
        return false;
    if (remove((*ledger).path) != 0 && errno != ENOENT)
        return false;
    (*ledger).state = LEDGER_ABSENT;
    (*ledger).version[0] = '\0';
    return true;
}

LedgerState Ledger_state(const Ledger *ledger) {
    return ledger ? (*ledger).state : LEDGER_ABSENT;
}

bool Ledger_hasRecord(const Ledger *ledger) {
    return ledger != nullptr && (*ledger).state != LEDGER_ABSENT;
}

bool Ledger_lastVersion(const Ledger *ledger, char *dest, size_t cap) {
    if (ledger == nullptr || dest == nullptr || cap == 0)
        return false;
    snprintf(dest, cap, "%s", (*ledger).version);
    return true;
}

const char *Ledger_lastError(const Ledger *ledger) {
    (void) ledger;
    return "";
}
