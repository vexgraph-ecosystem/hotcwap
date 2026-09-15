#ifndef SPOKE_VEXSPOKE_H
#define SPOKE_VEXSPOKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// spoke/vexspoke.h — the vexspoke API contract.
//
// This file IS the vexspoke API: the fn-table type any vexspoke binary must
// satisfy (statically linked today, dylib tomorrow), plus the section names
// the provider manifest grants per consumer. One public struct per the
// Single Class Per File Law: VexspokeApi.
//
// The table is DELIBERATELY dependency-free: it includes nothing from
// vexspoke, so hotcwap translation units (kernel/) bind the contract without
// naming a single vexspoke type — hotcwap lives on its own. Every handle is
// an opaque void* (arenas, listeners); exact types live only in
// spoke/vexspoke.c, the single seam file, which adapts them via wrappers.
// No other hotcwap file may include vexspoke headers for bridged symbols.
//
// Scope is the measured touch surface — exactly what hotcwap calls — not the
// whole of vexspoke. Pure, stateless vexspoke (math, types, containers) stays
// statically linked; only stateful substrate crosses this bridge:
//   - "arena": MemoryArena_create/destroy (the R2 slabs R1 boots on)
//   - "rings": Key/Mouse/Touch/Focus push + dispatch + attach (input streams)
//
// A consumer binds by (1) naming itself in the provider manifest's
// consumers[] with the sections it needs, then (2) filling this table — from
// static symbols (strangler phase, VexspokeApi_fillStatic) or dlopen
// (refresh phase). Unlisted consumers never reach step 2:
// HotManifest_allows refuses first.
//
// Handle attestation: createArena reports the provider-minted type id
// alongside the handle; holders store both and require nonzero (fail-closed
// on anonymous handles). VEXSPOKE_TYPE_ARENA is provisional — arenas carry
// no vexspoke registry row today, so only the mechanism is real for now;
// P0 mints the row and the manifest comparison upgrades the authority.
//
// First consumer row (lives in hotcwap/MANIFEST.mf — the seed the loader
// reads; P0 generates it from the vexspoke build with ABI rows filled.
// Documented here so table and policy can never drift apart):
//   {"name":"hotcwap","runtime":"R1","sections":["arena","rings"]}

#define VEXSPOKE_SECTION_ARENA "arena"
#define VEXSPOKE_SECTION_RINGS "rings"

// Provisional arena handle tag (see above). Not a vexspoke registry id.
#define VEXSPOKE_TYPE_ARENA ((uint64_t) 1)

// The contract: one pointer per bridged symbol. Camel-case members
// (struct-field style); call sites read (*api).keyPushEvent(...).
typedef struct VexspokeApi {
    // --- arena section ---
    void *(*createArena)(size_t totalBytes, uint64_t *outTypeId);
    void (*destroyArena)(void *arena);

    // --- rings section: key ---
    void (*keyAttachWindow)(uint32_t windowId, const void *listener);
    bool (*keyDetachWindow)(uint32_t windowId, const void *listener);
    void (*keyDetachWindowAll)(uint32_t windowId);
    void (*keyPushEvent)(uint32_t windowId, int keyCode, int action, uint64_t holdThresholdNanos);
    void (*keyPushCharEvent)(uint32_t windowId, uint32_t c);
    void (*keyDispatchEvents)();
    bool (*keyIsDown)(int keyCode);
    void (*keyShutdown)();

    // --- rings section: mouse ---
    void (*mouseAttachWindow)(uint32_t windowId, const void *listener);
    bool (*mouseDetachWindow)(uint32_t windowId, const void *listener);
    void (*mouseDetachWindowAll)(uint32_t windowId);
    void (*mousePushButtonEvent)(uint32_t windowId, int button, int action, uint64_t holdThresholdNanos);
    void (*mousePushMoveEvent)(uint32_t windowId, double x, double y);
    void (*mousePushMoveDeltaEvent)(uint32_t windowId, double dx, double dy);
    void (*mousePushDragEvent)(uint32_t windowId, int button, double x, double y);
    void (*mousePushScrollEvent)(uint32_t windowId, double dx, double dy);
    void (*mousePushZoomEvent)(uint32_t windowId, double magnification);
    void (*mouseDispatchEvents)();
    double (*mouseX)();

    // --- rings section: touch ---
    void (*touchAttachWindow)(uint32_t windowId, const void *listener);
    bool (*touchDetachWindow)(uint32_t windowId, const void *listener);
    void (*touchDetachWindowAll)(uint32_t windowId);
    void (*touchPushTouchEvent)(uint32_t windowId, int touchId, int action, double x, double y,
                                double pressure, uint64_t holdThresholdNanos);
    void (*touchDispatchEvents)();

    // --- rings section: focus ---
    void (*focusSet)(uint32_t windowId);
    bool (*focusIsFocused)(uint32_t windowId);

    // --- binding meta (binder owns these; zero until first fill) ---
    bool bound;
    uint64_t generation;
    uint64_t abi;
} VexspokeApi;

// Strangler phase-1 binder: fill every slot from the statically linked
// vexspoke symbols (wrapping where the opaque shape differs). Null-safe:
// false on null api, true otherwise. Sets bound, generation 1, abi 0
// (unverified — the digest gate arrives with the refresh phase).
bool VexspokeApi_fillStatic(VexspokeApi *api);

#endif
