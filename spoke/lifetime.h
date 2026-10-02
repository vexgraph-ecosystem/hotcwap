#ifndef SPOKE_LIFETIME_H
#define SPOKE_LIFETIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// spoke/lifetime.h — the Lifetime memory substrate contract for vexspoke (and vexspoke only).
//
// Single Class Per File Law: Lifetime.
//
// R1 Host (hotcwap) operates strictly on opaque void* handles and never includes
// vexspoke headers. hotcwap verifies pointer legitimacy (16-byte alignment,
// valid address space, non-null, and provider attestation) while keeping the
// memory structures completely opaque.
//
// Every arena pointer held by Lifetime is an opaque void*. All checks are performed
// by Lifetime_isLegit() to guarantee memory consistency across module hot-reloads.

#define VEXSPOKE_TYPE_ARENA ((uint64_t) 1)

typedef struct Lifetime {
    void *persistentArena;    // master session arena (opaque void*)
    void *transientArena;     // per-tick scratch arena (opaque void*)
    uint64_t persistentType;  // provider-reported type id, nonzero = attested
    uint64_t transientType;   // provider-reported type id, nonzero = attested
    void *relational;         // relational engine symbol root (opaque void*)
} Lifetime;

// Pointer Legitimacy Verification:
// Checks that ptr is non-null, 16-byte aligned ((uintptr_t)ptr & 15 == 0),
// and within valid user address space (>= 0x10000).
bool Lifetime_isLegit(const void *ptr);

// Validates that the Lifetime struct holds legitimate, attested arenas.
bool Lifetime_isValid(const Lifetime *lifetime);

// Lifecycle: create arenas using opaque handles, verifying legitimacy.
Lifetime Lifetime_create(size_t persistentBytes, size_t transientBytes);
void Lifetime_destroy(Lifetime *lifetime);
void Lifetime_resetTransient(Lifetime *lifetime);

// Aligned arena allocators (returns verified, 16-byte aligned void* payloads)
void *Lifetime_allocPersistent(Lifetime *lifetime, uint64_t typeId, size_t bytes);
void *Lifetime_allocTransient(Lifetime *lifetime, uint64_t typeId, size_t bytes);
void Lifetime_freePersistent(Lifetime *lifetime, void *ptr);

// Bind external opaque arenas (e.g. passed from vexspoke or host launcher)
Lifetime Lifetime_bind(void *persistentArena, void *transientArena,
                       uint64_t persistentType, uint64_t transientType,
                       void *relational);

#endif

