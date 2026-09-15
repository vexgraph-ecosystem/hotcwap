#include "hot/manifest.h"

#include <string.h>
#include <stdlib.h>
#include "annotation/overview.h"

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: Manifest (hot/manifest.c)
 * LEVEL: L1 — File Metadata (manifest schema other files consume)
 * ============================================================================
  * Module manifest format.
  *
  * STRUCT FIELDS: none — procedural/stateless (operates on HotManifest)
  *
  * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - HotManifest_parse(json, len, out)
 *   - HotManifest_compatible(old_manifest, new_manifest)
 *   - HotManifest_digest(manifest)
 *   - HotManifest_allows(manifest, consumer, section)
 *
 * Getters:
 *   - HotManifest_get_type_id(manifest, name)
 * ============================================================================
 */


// hot/manifest.c — Minimal JSON parser for module manifests.
//
// This is a deliberately simple parser — no external dependencies.
// It handles the specific manifest format we need:
//   {"name": "...", "version": "...", "type_ids": [...], "exports": [...], "dependencies": [...],
//    "consumers": [{"name": "...", "runtime": "...", "sections": [...]}]}
// Type rows carry {"name", "value"} plus optional "parent"/"size" in any order.

// Skip whitespace
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// Parse a JSON string with escape sequence handling (\", \\, \n, \t, \uXXXX)
static const char *parse_string(const char *p, char *out, size_t out_size) {
    if (*p != '"') return nullptr;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            if (*p == '"') { out[i++] = '"'; p++; }
            else if (*p == '\\') { out[i++] = '\\'; p++; }
            else if (*p == '/') { out[i++] = '/'; p++; }
            else if (*p == 'b') { out[i++] = '\b'; p++; }
            else if (*p == 'f') { out[i++] = '\f'; p++; }
            else if (*p == 'n') { out[i++] = '\n'; p++; }
            else if (*p == 'r') { out[i++] = '\r'; p++; }
            else if (*p == 't') { out[i++] = '\t'; p++; }
            else if (*p == 'u') {
                p++;
                uint32_t u = 0;
                for (int h = 0; h < 4 && *p; h++, p++) {
                    u <<= 4;
                    if (*p >= '0' && *p <= '9') u |= (uint32_t) (*p - '0');
                    else if (*p >= 'a' && *p <= 'f') u |= (uint32_t) (*p - 'a' + 10);
                    else if (*p >= 'A' && *p <= 'F') u |= (uint32_t) (*p - 'A' + 10);
                }
                if (u < 0x80) {
                    out[i++] = (char) u;
                } else if (u < 0x800 && i + 1 < out_size - 1) {
                    out[i++] = (char) (0xC0 | (u >> 6));
                    out[i++] = (char) (0x80 | (u & 0x3F));
                } else if (i + 2 < out_size - 1) {
                    out[i++] = (char) (0xE0 | (u >> 12));
                    out[i++] = (char) (0x80 | ((u >> 6) & 0x3F));
                    out[i++] = (char) (0x80 | (u & 0x3F));
                }
            } else {
                out[i++] = *p++;
            }
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

// Parse a JSON number (uint64)
static const char *parse_uint(const char *p, uint64_t *out) {
    uint64_t val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    *out = val;
    return p;
}

// Parse a hex number (0x...)
static const char *parse_hex(const char *p, uint64_t *out) {
    uint64_t val = 0;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
        uint8_t nibble;
        if (*p >= '0' && *p <= '9') nibble = *p - '0';
        else if (*p >= 'a' && *p <= 'f') nibble = *p - 'a' + 10;
        else nibble = *p - 'A' + 10;
        val = (val << 4) | nibble;
        p++;
    }
    *out = val;
    return p;
}

// Parse a JSON object value after the key
static const char *parse_value(const char *p, char *str_out, size_t str_size, uint64_t *num_out, bool *is_hex) {
    p = skip_ws(p);
    if (*p == '"') {
        p = parse_string(p, str_out, str_size);
        *is_hex = false;
    } else if (p[0] == '0' && p[1] == 'x') {
        p += 2;
        p = parse_hex(p, num_out);
        *is_hex = true;
    } else {
        p = parse_uint(p, num_out);
        *is_hex = false;
    }
    return p;
}

bool HotManifest_parse(const char *json, size_t len, HotManifest *out) {
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));
    
    const char *p = json;
    const char *end = json + len;
    
    // Expect opening brace
    p = skip_ws(p);
    if (*p != '{') return false;
    p++;
    
    while (p < end) {
        p = skip_ws(p);
        if (*p == '}') break; // End of object
        
        // Parse key
        char key[64];
        p = parse_string(p, key, sizeof(key));
        if (!p) return false;
        
        // Expect colon
        p = skip_ws(p);
        if (*p != ':') return false;
        p++;
        
        // Parse value
        p = skip_ws(p);
        
        if (strcmp(key, "name") == 0) {
            char value[64];
            bool is_hex;
            p = parse_value(p, value, sizeof(value), nullptr, &is_hex);
            strncpy((*out).name, value, HOT_MANIFEST_MAX_NAME - 1);
        } else if (strcmp(key, "version") == 0) {
            char value[64];
            bool is_hex;
            p = parse_value(p, value, sizeof(value), nullptr, &is_hex);
            strncpy((*out).version, value, HOT_MANIFEST_MAX_VERSION - 1);
        } else if (strcmp(key, "type_ids") == 0) {
            // Expect array
            if (*p != '[') return false;
            p++;
            while (p < end) {
                p = skip_ws(p);
                if (*p == ']') { p++; break; }
                if (*p == ',') { p++; continue; }
                
                // Type row: {"name": "...", "value": 0x...} plus optional
                // "parent" (class number) and "size" (struct bytes), in any
                // order. Absent parent/size stay unstated (legacy wire form).
                if (*p != '{') return false;
                p++;

                HotTypeId tid;
                memset(&tid, 0, sizeof(tid));

                while (p < end) {
                    p = skip_ws(p);
                    if (*p == '}') { p++; break; }
                    if (*p == ',') { p++; continue; }
                    char tkey[32];
                    p = parse_string(p, tkey, sizeof(tkey));
                    if (!p) return false;
                    p = skip_ws(p);
                    if (*p != ':') return false;
                    p++;
                    p = skip_ws(p);
                    if (strcmp(tkey, "name") == 0) {
                        char tname[64];
                        bool is_hex = false;
                        p = parse_value(p, tname, sizeof(tname), nullptr, &is_hex);
                        if (!p) return false;
                        strncpy(tid.name, tname, HOT_MANIFEST_MAX_NAME - 1);
                    } else if (strcmp(tkey, "value") == 0) {
                        uint64_t tval = 0;
                        bool is_hex = false;
                        p = parse_value(p, nullptr, 0, &tval, &is_hex);
                        if (!p) return false;
                        tid.value = tval;
                    } else if (strcmp(tkey, "parent") == 0) {
                        uint64_t tpar = 0;
                        bool is_hex = false;
                        p = parse_value(p, nullptr, 0, &tpar, &is_hex);
                        if (!p) return false;
                        tid.parent = (int32_t) tpar;
                        tid.has_parent = true;
                    } else if (strcmp(tkey, "size") == 0) {
                        uint64_t tsize = 0;
                        bool is_hex = false;
                        p = parse_value(p, nullptr, 0, &tsize, &is_hex);
                        if (!p) return false;
                        tid.size = (uint32_t) tsize;
                    } else {
                        // Unknown key — skip value
                        if (*p == '"') {
                            char dummy[64];
                            bool is_hex = false;
                            p = parse_value(p, dummy, sizeof(dummy), nullptr, &is_hex);
                        } else {
                            uint64_t dummy = 0;
                            bool is_hex = false;
                            p = parse_value(p, nullptr, 0, &dummy, &is_hex);
                        }
                        if (!p) return false;
                    }
                }

                if ((*out).type_id_count < HOT_MANIFEST_MAX_TYPE_IDS) {
                    (*out).type_ids[(*out).type_id_count++] = tid;
                }
            }
        } else if (strcmp(key, "exports") == 0) {
            if (*p != '[') return false;
            p++;
            while (p < end) {
                p = skip_ws(p);
                if (*p == ']') { p++; break; }
                if (*p == ',') { p++; continue; }
                
                char value[64];
                bool is_hex;
                p = parse_value(p, value, sizeof(value), nullptr, &is_hex);
                
                if ((*out).export_count < HOT_MANIFEST_MAX_EXPORTS) {
                    strncpy((*out).exports[(*out).export_count].name, value, HOT_MANIFEST_MAX_NAME - 1);
                    (*out).export_count++;
                }
            }
        } else if (strcmp(key, "dependencies") == 0) {
            if (*p != '[') return false;
            p++;
            while (p < end) {
                p = skip_ws(p);
                if (*p == ']') { p++; break; }
                if (*p == ',') { p++; continue; }
                
                char value[64];
                bool is_hex;
                p = parse_value(p, value, sizeof(value), nullptr, &is_hex);
                
                if ((*out).dependency_count < HOT_MANIFEST_MAX_DEPENDENCIES) {
                    strncpy((*out).dependencies[(*out).dependency_count].name, value, HOT_MANIFEST_MAX_NAME - 1);
                    (*out).dependency_count++;
                }
            }
        } else if (strcmp(key, "consumers") == 0) {
            // Allow-list rows: {"name": "...", "runtime": "...",
            // "sections": ["..."]} in any order. Unknown row keys skip like
            // type rows. Overflow rows drop (bounded, no alloc).
            if (*p != '[') return false;
            p++;
            while (p < end) {
                p = skip_ws(p);
                if (*p == ']') { p++; break; }
                if (*p == ',') { p++; continue; }
                if (*p != '{') return false;
                p++;

                HotConsumer row;
                memset(&row, 0, sizeof(row));

                while (p < end) {
                    p = skip_ws(p);
                    if (*p == '}') { p++; break; }
                    if (*p == ',') { p++; continue; }
                    char ckey[32];
                    p = parse_string(p, ckey, sizeof(ckey));
                    if (!p) return false;
                    p = skip_ws(p);
                    if (*p != ':') return false;
                    p++;
                    p = skip_ws(p);
                    if (strcmp(ckey, "name") == 0) {
                        char cname[64];
                        bool is_hex = false;
                        p = parse_value(p, cname, sizeof(cname), nullptr, &is_hex);
                        if (!p) return false;
                        strncpy(row.name, cname, HOT_MANIFEST_MAX_NAME - 1);
                    } else if (strcmp(ckey, "runtime") == 0) {
                        char cruntime[64];
                        bool is_hex = false;
                        p = parse_value(p, cruntime, sizeof(cruntime), nullptr, &is_hex);
                        if (!p) return false;
                        strncpy(row.runtime, cruntime, HOT_MANIFEST_MAX_RUNTIME - 1);
                    } else if (strcmp(ckey, "sections") == 0) {
                        if (*p != '[') return false;
                        p++;
                        while (p < end) {
                            p = skip_ws(p);
                            if (*p == ']') { p++; break; }
                            if (*p == ',') { p++; continue; }
                            char sec[64];
                            bool is_hex = false;
                            p = parse_value(p, sec, sizeof(sec), nullptr, &is_hex);
                            if (!p) return false;
                            if (row.section_count < HOT_MANIFEST_MAX_CONSUMER_SECTIONS) {
                                strncpy(row.sections[row.section_count], sec, HOT_MANIFEST_MAX_NAME - 1);
                                row.section_count++;
                            }
                        }
                    } else {
                        if (*p == '"') {
                            char dummy[64];
                            bool is_hex = false;
                            p = parse_value(p, dummy, sizeof(dummy), nullptr, &is_hex);
                        } else if (*p == '[') {
                            p++;
                            int depth = 1;
                            while (p < end && depth > 0) {
                                if (*p == '[') depth++;
                                else if (*p == ']') depth--;
                                p++;
                            }
                        } else {
                            uint64_t dummy = 0;
                            bool is_hex = false;
                            p = parse_value(p, nullptr, 0, &dummy, &is_hex);
                        }
                        if (!p) return false;
                    }
                }

                if ((*out).consumer_count < HOT_MANIFEST_MAX_CONSUMERS) {
                    (*out).consumers[(*out).consumer_count++] = row;
                }
            }
        } else {
            // Unknown key — skip value
            if (*p == '"') {
                char dummy[64];
                bool is_hex;
                p = parse_value(p, dummy, sizeof(dummy), nullptr, &is_hex);
            } else {
                uint64_t dummy;
                bool is_hex;
                p = parse_value(p, nullptr, 0, &dummy, &is_hex);
            }
        }
        
        // Expect comma or closing brace
        p = skip_ws(p);
        if (*p == ',') p++;
        else if (*p == '}') break;
    }
    
    return true;
}

static const HotTypeId *find_type_id(const HotManifest *manifest, const char *name) {
    if (!manifest || !name)
        return nullptr;
    for (uint32_t i = 0; i < (*manifest).type_id_count; i++) {
        if (strcmp((*manifest).type_ids[i].name, name) == 0)
            return &(*manifest).type_ids[i];
    }
    return nullptr;
}

bool HotManifest_compatible(const HotManifest *old_manifest, const HotManifest *new_manifest) {
    if (!old_manifest || !new_manifest) return false;

    // Every old type name must exist in the new manifest with the same value.
    // Parent chains and struct sizes must match wherever both sides state
    // them; one-sided parent/size refuses (a side that withholds contract
    // info cannot prove compatibility). New names in the new manifest are
    // allowed (growth); removed or renumbered names refuse.
    for (uint32_t i = 0; i < (*old_manifest).type_id_count; i++) {
        const HotTypeId *o = &(*old_manifest).type_ids[i];
        const HotTypeId *n = find_type_id(new_manifest, (*o).name);
        if (!n)
            return false;
        if ((*n).value != (*o).value)
            return false;
        if ((*o).has_parent || (*n).has_parent) {
            if (!((*o).has_parent && (*n).has_parent))
                return false;
            if ((*o).parent != (*n).parent)
                return false;
        }
        bool oSize = (*o).size != 0;
        bool nSize = (*n).size != 0;
        if (oSize || nSize) {
            if (!(oSize && nSize))
                return false;
            if ((*o).size != (*n).size)
                return false;
        }
    }

    return true;
}

// FNV-1a 64: offset basis 14695981039346656037, prime 1099511628211.
static void digest_bytes(uint64_t *hash, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t*) data;
    for (size_t i = 0; i < len; i++) {
        *hash ^= (uint64_t) bytes[i];
        *hash *= 1099511628211ULL;
    }
}

uint64_t HotManifest_digest(const HotManifest *manifest) {
    if (!manifest)
        return 0;
    // Canonical order: insertion sort of row indices by name (n <= 256, so
    // the quadratic sort is trivial). The digest must not depend on JSON
    // key order — same contract, any order, same digest.
    uint32_t order[HOT_MANIFEST_MAX_TYPE_IDS];
    uint32_t n = (*manifest).type_id_count;
    if (n > HOT_MANIFEST_MAX_TYPE_IDS)
        n = HOT_MANIFEST_MAX_TYPE_IDS;
    for (uint32_t i = 0; i < n; i++)
        order[i] = i;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = order[i];
        uint32_t j = i;
        while (j > 0 && strcmp((*manifest).type_ids[order[j - 1]].name, (*manifest).type_ids[key].name) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    uint64_t hash = 14695981039346656037ULL;
    for (uint32_t i = 0; i < n; i++) {
        const HotTypeId *t = &(*manifest).type_ids[order[i]];
        digest_bytes(&hash, (*t).name, strlen((*t).name) + 1);
        digest_bytes(&hash, &(*t).value, sizeof((*t).value));
        uint8_t stated = (*t).has_parent ? 1u : 0u;
        digest_bytes(&hash, &stated, sizeof(stated));
        digest_bytes(&hash, &(*t).parent, sizeof((*t).parent));
        digest_bytes(&hash, &(*t).size, sizeof((*t).size));
    }
    return hash;
}

uint64_t HotManifest_get_type_id(const HotManifest *manifest, const char *name) {
    if (!manifest || !name) return 0;
    
    for (uint32_t i = 0; i < (*manifest).type_id_count; i++) {
        if (strcmp((*manifest).type_ids[i].name, name) == 0) {
            return (*manifest).type_ids[i].value;
        }
    }
    
    return 0;
}

bool HotManifest_allows(const HotManifest *manifest, const char *consumer, const char *section) {
    if (!manifest || !consumer)
        return false;
    for (uint32_t i = 0; i < (*manifest).consumer_count; i++) {
        const HotConsumer *row = &(*manifest).consumers[i];
        if (strcmp((*row).name, consumer) != 0)
            continue;
        if (section == nullptr)
            return true;
        for (uint32_t s = 0; s < (*row).section_count; s++) {
            if (strcmp((*row).sections[s], section) == 0)
                return true;
        }
        return false;
    }
    return false;
}
