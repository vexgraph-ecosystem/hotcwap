#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "annotation/intention.h"
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Console
 * ============================================================================
 * Session state machine for shell scripts, CLI child execution, and REPL interactions.
 * Owns session lifecycle (shell command, working directory, running/cancel state,
 * exit status) while delegating process execution across the ConsoleIo function-table
 * seam to avoid direct thread or socket ownership in hotcwap.
 *
 * Memory consists of fixed-capacity string buffers for command and directory, an
 * injectable ConsoleIo function table, and an atomic cancel flag. Allocation is
 * cold heap via calloc; child process operations run non-blocking in slices bounded
 * to at most 100 milliseconds per the Bounded Wait Law.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Console (kernel/console.c)
 * LEVEL: L2 — Behavior (session state machine; execution borrowed from R2)
 * ============================================================================
 * SUMMARY:
 *   Session state machine for shell scripts and REPL-style interactions. Owns
 *   the session lifecycle (running/cancel/exitStatus) and borrows execution
 *   through a ConsoleIo fn-table seam (the Conflict Triage Law managed exception — wired to
 *   R2 ProcessSpawn when that class lands).
 *
 *   No threads, no sockets, no blocking waits are owned by Console. The
 *   supervising Kernel thread drives Console_poll in bounded <=100ms slices
 *   with a cancel flag (the Bounded Wait Law). Console never touches a window.
 *
 * STRUCT FIELDS (Mirroring kernel/console.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char shell[CONSOLE_MAX_SHELL];          // command to run (default "/bin/sh")
 *   char workDir[CONSOLE_MAX_WORKDIR];      // working directory (default "")
 *   ConsoleIo io;                           // injectable execution seam
 *   _Atomic bool cancel;                    // cancel flag (the Bounded Wait Law)
 *   bool running;                           // session active
 *   int exitStatus;                         // exit code of last session
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   console_clamp_copy(dst, cap, src) : Safe bounded string copy with null terminator
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Console()                               : Console_0()
 *   - Console(shell)                          : Console_1(shell)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Console_run(self)                       : Launch session via io seam
 *   - Console_writeInput(self, line, len)     : Feed stdin pipe
 *   - Console_poll(self, out, outCap, outLen) : Non-blocking <=100ms output drain
 *   - Console_cancel(self)                    : Request graceful SIGTERM
 *   - Console_isRunning(self)                 : Query session active flag
 *   - Console_getExitStatus(self)             : Query child process exit code
 *   - Console_free(self)                      : Cancel session and release memory
 *
 * Private Core Functions: (.c static)
 *   - console_clamp_copy(dst, cap, src)       : Bounded string clamp helper
 *
 * Public Setters: (.h)
 *   - Console_setShell(self, shell)
 *   - Console_setWorkDir(self, workDir)
 *   - Console_setIo(self, io, ctx)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Console_getShell(self)
 *   - Console_getWorkDir(self)
 *   - Console_getIo(self)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// ;;INTENTION("ConsoleIo seam wiring: when R2 ProcessSpawn lands,
// Console_run calls io.spawn(shell, workDir).  Console_poll drives
// io.reap in <=100ms non-blocking slices, with io.cancel firing
// SIGTERM and the supervising thread observing completion via io.joined.
// All io.fn-pointers are nullable — NULL degrades gracefully per the Cold-Strict, Hot-Minimal Validation Law.")

static void console_clamp_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

// CONSTRUCTORS (PUBLIC & PRIVATE)
Console *Console_0(void) {
    return Console_1("/bin/sh");
}

Console *Console_1(const char *shell) {
    Console *self = (Console*) calloc(1, sizeof(Console));
    if (!self)
        return NULL;
    console_clamp_copy((*self).shell, CONSOLE_MAX_SHELL, shell);
    (*self).workDir[0] = '\0';
    memset(&(*self).io, 0, sizeof(ConsoleIo));
    atomic_init(&(*self).cancel, false);
    (*self).running = false;
    (*self).exitStatus = 0;
    return self;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
bool Console_run(Console *self) {
    if (!self)
        return false;
    if ((*self).running)
        return false;
    ConsoleIo *io = &(*self).io;
    if (!(*io).spawn)
        return false;

    atomic_store_explicit(&(*self).cancel, false, memory_order_relaxed);
    (*self).exitStatus = 0;
    bool ok = (*io).spawn((*io).ctx, (*self).shell, (*self).workDir);
    (*self).running = ok;
    return ok;
}

bool Console_writeInput(Console *self, const char *line, size_t len) {
    if (!self)
        return false;
    if (!(*self).running)
        return false;
    ConsoleIo *io = &(*self).io;
    if (!(*io).feed)
        return false;
    return (*io).feed((*io).ctx, line, len);
}

bool Console_poll(Console *self, char *out, size_t outCap, size_t *outLen) {
    if (!self)
        return false;
    if (outLen)
        *outLen = 0;
    if (!(*self).running) {
        if (out && outCap > 0)
            out[0] = '\0';
        return true;
    }
    ConsoleIo *io = &(*self).io;
    if (!(*io).reap) {
        if (out && outCap > 0)
            out[0] = '\0';
        return true;
    }
    size_t n = 0;
    bool ok = (*io).reap((*io).ctx, out, outCap, &n);
    if (outLen)
        *outLen = n;
    if ((*io).joined && (*io).joined((*io).ctx)) {
        (*self).running = false;
    }
    return ok;
}

void Console_cancel(Console *self) {
    if (!self)
        return;
    atomic_store_explicit(&(*self).cancel, true, memory_order_relaxed);
    ConsoleIo *io = &(*self).io;
    if ((*io).cancel)
        (*io).cancel((*io).ctx);
}

void Console_free(Console *self) {
    if (!self)
        return;
    if ((*self).running)
        Console_cancel(self);
    memset(&(*self).io, 0, sizeof(ConsoleIo));
    free(self);
}

bool Console_isRunning(const Console *self) {
    if (!self)
        return false;
    return (*self).running;
}

int Console_getExitStatus(const Console *self) {
    if (!self)
        return -1;
    return (*self).exitStatus;
}

// SETTERS (PUBLIC & PRIVATE)
;;SETTER
void Console_setShell(Console *self, const char *shell) {
    if (!self)
        return;
    console_clamp_copy((*self).shell, CONSOLE_MAX_SHELL, shell);
}

;;SETTER
void Console_setWorkDir(Console *self, const char *workDir) {
    if (!self)
        return;
    console_clamp_copy((*self).workDir, CONSOLE_MAX_WORKDIR, workDir);
}

;;SETTER
void Console_setIo(Console *self, const ConsoleIo *io, void *ioCtx) {
    if (!self)
        return;
    if (!io) {
        memset(&(*self).io, 0, sizeof(ConsoleIo));
        return;
    }
    (*self).io = *io;
    (*self).io.ctx = ioCtx;
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
const char *Console_getShell(const Console *self) {
    if (!self)
        return nullptr;
    return (*self).shell;
}

;;GETTER
const char *Console_getWorkDir(const Console *self) {
    if (!self)
        return nullptr;
    return (*self).workDir;
}

;;GETTER
const ConsoleIo *Console_getIo(const Console *self) {
    if (!self)
        return nullptr;
    return (*self).io.ctx ? &(*self).io : nullptr;
}

