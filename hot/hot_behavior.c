#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: Hot_behavior (hot/hot_behavior.c)
 * LEVEL: L3 — Module Code (reloaded dylib pulse/bar business logic)
 * ============================================================================
 * Phase-2 L2/L3 behavior subject: pure pulse/bar math + texture path.
 *
 * Deliberately stateless-on-GPU: this module NEVER calls Vk_* or touches
 * the Panel tree. Host handlers (main/test_suite.c, _tests/hotcwap/) own
 * cmdBuffer + Panel pointers and delegate only the math here. That keeps swap safe by
 * construction — no code pointers cross the dylib boundary, no dangling
 * renderHandler after dlclose.
 *
* State schema (versioned for L3 migration + #8.5 rollback validation):
 *   v1 (1.0.0): [phaseBias f32][modeShadow i32] = 8 bytes
 *   v2 (1.1.0): [phaseBias f32][modeShadow i32][glowStrength f32] = 12 bytes
 *   v3 (1.2.0): [schemaMagic u32][phaseBias f32][modeShadow i32][glowStrength f32] = 16 bytes
 * Hot_save emits v3 blobs carrying HOT_BEHAVIOR_SCHEMA_MAGIC as an ownership
 * tag. Hot_restore adopts a v3 blob only when its magic matches the current
 * build (a foreign magic returns false), wraps legacy v1/v2 sizes into the
 * current schema, and rejects unknown lengths. The loader (hot/hot.c) treats
 * a restore rejection as #8.5 Automated State Rollback: it keeps the previous
 * generation live and never advances the stamp. Rebuild the module with
 * -DHOT_BEHAVIOR_SCHEMA_MAGIC=<other> to produce a "foreign-generation" build
 * — the bad payload manifest_rollback_test promotes as generation 2.
 * Hot_migrate carries v1/v2 -> v3 forward (glow defaults to 1.0); the loader
 * calls it when the saved blob predates the new module, same-version swaps
 * take the direct Hot_save/Hot_restore path.
 *
 * STRUCT FIELDS: none — procedural/stateless (operates on HotModule via Hot_* module exports)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - Hot_init_module(void)
 *
 * Core Functions:
 *   - Hot_shutdown_module(void)
 *   - Hot_save(buf, cap, outLen)
 *   - Hot_restore(buf, len)
 *   - Hot_migrate(oldVersion, oldBuf, oldLen, newBuf, newCap, outLen)
 *   - Hot_manifest(void)
 *   - VkModuleGetTrampolines(outCount)   : loader-ABI trampoline table export
 *   - hot_behavior_pulse(nowSeconds)
 *   - hot_behavior_bar(w, h, pulse, outBarH, outBarW)
 *   - hot_texture_path(void)
 *
 * Setters:
 *   - hot_behavior_set_phase_bias(value)
 *
 * Getters:
 *   - hot_behavior_get_phase_bias(void)
 * ============================================================================
 */

#define HOT_BEHAVIOR_VERSION "1.2.0"
#define HOT_BEHAVIOR_V1_SIZE 8
#define HOT_BEHAVIOR_V2_SIZE 12
#define HOT_BEHAVIOR_V3_SIZE 16

#ifndef HOT_BEHAVIOR_SCHEMA_MAGIC
#define HOT_BEHAVIOR_SCHEMA_MAGIC 0x56455842u   // "VEXB" — v3 state-blob ownership tag
#endif

static float s_phaseBias = 0.0f;
static int32_t s_modeShadow = 0;
static float s_glowStrength = 1.0f;

bool Hot_init_module(void) {
    return true;
}

void Hot_shutdown_module(void) {
    return;
}

bool Hot_save(void *buf, size_t cap, size_t *outLen) {
    if (!buf || !outLen)
        return false;
    if (cap < HOT_BEHAVIOR_V3_SIZE)
        return false;
    uint8_t *b = (uint8_t*) buf;
    const uint32_t magic = HOT_BEHAVIOR_SCHEMA_MAGIC;
    memcpy(b, &magic, 4);
    memcpy(b + 4, &s_phaseBias, 4);
    memcpy(b + 8, &s_modeShadow, 4);
    memcpy(b + 12, &s_glowStrength, 4);
    (*outLen) = HOT_BEHAVIOR_V3_SIZE;
    return true;
}

bool Hot_restore(const void *buf, size_t len) {
    if (!buf)
        return false;
    const uint8_t *b = (const uint8_t*) buf;
    if (len == HOT_BEHAVIOR_V1_SIZE) {
        memcpy(&s_phaseBias, b, 4);
        memcpy(&s_modeShadow, b + 4, 4);
        s_glowStrength = 1.0f;
        return true;
    }
    if (len == HOT_BEHAVIOR_V2_SIZE) {
        memcpy(&s_phaseBias, b, 4);
        memcpy(&s_modeShadow, b + 4, 4);
        memcpy(&s_glowStrength, b + 8, 4);
        return true;
    }
    if (len != HOT_BEHAVIOR_V3_SIZE)
        return false;
    uint32_t magic = 0;
    memcpy(&magic, b, 4);
    if (magic != HOT_BEHAVIOR_SCHEMA_MAGIC)
        return false;
    memcpy(&s_phaseBias, b + 4, 4);
    memcpy(&s_modeShadow, b + 8, 4);
    memcpy(&s_glowStrength, b + 12, 4);
    return true;
}

bool Hot_migrate(const char *oldVersion, const void *oldBuf, size_t oldLen,
                 void *newBuf, size_t newCap, size_t *outLen) {
    if (!oldVersion || !oldBuf || !newBuf || !outLen)
        return false;
    const uint32_t magic = HOT_BEHAVIOR_SCHEMA_MAGIC;
    if (strcmp(oldVersion, "1.0.0") == 0 && oldLen == HOT_BEHAVIOR_V1_SIZE) {
        if (newCap < HOT_BEHAVIOR_V3_SIZE)
            return false;
        memcpy(newBuf, &magic, 4);
        memcpy((uint8_t*) newBuf + 4, oldBuf, HOT_BEHAVIOR_V1_SIZE);
        float glow = 1.0f;
        memcpy((uint8_t*) newBuf + HOT_BEHAVIOR_V1_SIZE + 4, &glow, 4);
        (*outLen) = HOT_BEHAVIOR_V3_SIZE;
        return true;
    }
    if (strcmp(oldVersion, "1.1.0") == 0 && oldLen == HOT_BEHAVIOR_V2_SIZE) {
        if (newCap < HOT_BEHAVIOR_V3_SIZE)
            return false;
        memcpy(newBuf, &magic, 4);
        memcpy((uint8_t*) newBuf + 4, oldBuf, oldLen);
        (*outLen) = HOT_BEHAVIOR_V3_SIZE;
        return true;
    }
    if (oldLen <= newCap) {
        memcpy(newBuf, oldBuf, oldLen);
        (*outLen) = oldLen;
        return true;
    }
    return false;
}

const char *Hot_manifest(void) {
    return "{\"name\": \"hot_behavior\", \"version\": \"" HOT_BEHAVIOR_VERSION "\", "
        "\"type_ids\": [{\"name\": \"BehaviorState\", \"value\": 2}], "
        "\"exports\": [\"hot_behavior_pulse\", \"hot_behavior_bar\", "
        "\"hot_texture_path\", \"hot_behavior_set_phase_bias\", "
        "\"hot_behavior_get_phase_bias\", \"Hot_init_module\", "
        "\"Hot_shutdown_module\", \"Hot_save\", \"Hot_restore\", \"Hot_migrate\", "
        "\"VkModuleGetTrampolines\"], "
        "\"dependencies\": []}";
}

// LOADER ABI EXPORT — the trampoline table the manifest loader (hot/hot.c)
// adopts on dlopen. Row layout MUST match hot.c's contract:
//   { const char *name; void *fn; } entries[], count via VkModuleGetTrampolines.
typedef struct HotModuleExport {
    const char *name;
    void *fn;
} HotModuleExport;

float hot_behavior_pulse(double nowSeconds);
void hot_behavior_bar(float w, float h, float pulse, float *outBarH, float *outBarW);
const char *hot_texture_path(void);
void hot_behavior_set_phase_bias(float value);
float hot_behavior_get_phase_bias(void);

static const HotModuleExport s_exports[] = {
    { "hot_behavior_pulse",    (void*) hot_behavior_pulse },
    { "hot_behavior_bar",      (void*) hot_behavior_bar },
    { "hot_texture_path",      (void*) hot_texture_path },
    { "hot_behavior_set_phase_bias", (void*) hot_behavior_set_phase_bias },
    { "hot_behavior_get_phase_bias", (void*) hot_behavior_get_phase_bias },
    { "Hot_init_module",       (void*) Hot_init_module },
    { "Hot_shutdown_module",   (void*) Hot_shutdown_module },
    { "Hot_save",              (void*) Hot_save },
    { "Hot_restore",           (void*) Hot_restore },
    { "Hot_migrate",           (void*) Hot_migrate },
};

const void *VkModuleGetTrampolines(uint32_t *outCount) {
    if (outCount)
        (*outCount) = (uint32_t) (sizeof(s_exports) / sizeof(s_exports[0]));
    return s_exports;
}

float hot_behavior_pulse(double nowSeconds) {
    double t = nowSeconds + (double) s_phaseBias;
    float base = 0.5f + 0.5f * sinf((float) (t * 6.28318530718));
    return base * s_glowStrength > 1.0f ? 1.0f : base * s_glowStrength;
}

void hot_behavior_bar(float w, float h, float pulse, float *outBarH, float *outBarW) {
    float barH = h * 0.08f;
    float barW = w * pulse;
    if (outBarH)
        (*outBarH) = barH;
    if (outBarW)
        (*outBarW) = barW;
    (void) w;
}

const char *hot_texture_path(void) {
    // Portable asset-relative path: the host resolves this against its asset
    // root (never an absolute developer-machine path — those break every
    // other checkout and leak local filesystem layout).
    return "assets/sunflower.png";
}

void hot_behavior_set_phase_bias(float value) {
    s_phaseBias = value;
}

float hot_behavior_get_phase_bias(void) {
    return s_phaseBias;
}
