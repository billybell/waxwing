#include "cbor_decode.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Argument decoding
// ---------------------------------------------------------------------------

// Decode the additional-info argument that follows a CBOR header byte. On
// return, *pp is advanced past the argument bytes and *out_arg holds the
// raw value.
static bool decode_arg(const uint8_t **pp, const uint8_t *end,
                       uint8_t additional, uint64_t *out_arg) {
    const uint8_t *p = *pp;
    if (additional < 24) {
        *out_arg = additional;
        *pp = p;
        return true;
    }
    if (additional == 24) {
        if (p + 1 > end) return false;
        *out_arg = p[0];
        *pp = p + 1;
        return true;
    }
    if (additional == 25) {
        if (p + 2 > end) return false;
        *out_arg = ((uint64_t)p[0] << 8) | p[1];
        *pp = p + 2;
        return true;
    }
    if (additional == 26) {
        if (p + 4 > end) return false;
        *out_arg = ((uint64_t)p[0] << 24) | ((uint64_t)p[1] << 16)
                 | ((uint64_t)p[2] << 8)  |  (uint64_t)p[3];
        *pp = p + 4;
        return true;
    }
    if (additional == 27) {
        if (p + 8 > end) return false;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
        *out_arg = v;
        *pp = p + 8;
        return true;
    }
    return false; // 28..30 reserved, 31 indefinite — we don't support either
}

// ---------------------------------------------------------------------------
// Parse one item
// ---------------------------------------------------------------------------

bool cbor_parse(const uint8_t *buf, const uint8_t *end, cbor_item_t *out) {
    if (!buf || !out || buf >= end) return false;

    out->start = buf;
    out->data = NULL;

    uint8_t header = buf[0];
    uint8_t major = header >> 5;
    uint8_t additional = header & 0x1F;

    const uint8_t *p = buf + 1;
    uint64_t arg = 0;

    switch (major) {
    case CBOR_TYPE_UINT:
    case CBOR_TYPE_INT:
        if (!decode_arg(&p, end, additional, &arg)) return false;
        out->type = (cbor_type_t)major;
        out->arg = arg;
        out->next = p;
        return true;

    case CBOR_TYPE_BSTR:
    case CBOR_TYPE_TSTR:
        if (!decode_arg(&p, end, additional, &arg)) return false;
        if (p + arg > end) return false;
        out->type = (cbor_type_t)major;
        out->arg = arg;
        out->data = p;
        out->next = p + arg;
        return true;

    case CBOR_TYPE_ARRAY:
    case CBOR_TYPE_MAP: {
        if (!decode_arg(&p, end, additional, &arg)) return false;
        out->type = (cbor_type_t)major;
        out->arg = arg;
        out->data = p;  // first child (or first key/value pair for a map)
        uint64_t children = (major == CBOR_TYPE_MAP) ? arg * 2 : arg;
        for (uint64_t i = 0; i < children; i++) {
            cbor_item_t tmp;
            if (!cbor_parse(p, end, &tmp)) return false;
            p = tmp.next;
        }
        out->next = p;
        return true;
    }

    case CBOR_TYPE_BOOL:
        // Major type 7 covers simple values AND floats. We don't need to
        // interpret the float bits — the meta sidecar is opaque CBOR
        // that round-trips through the firmware verbatim. We just need
        // to advance `next` past the payload so the parent map's walker
        // can find the end of the value. iOS sends lat/lon as double
        // (AI=27); without this branch, write_meta with location data
        // failed top-level parse and silently lost the sidecar.
        if (additional == 20) { out->type = CBOR_TYPE_BOOL;  out->arg = 0; out->next = p; return true; }
        if (additional == 21) { out->type = CBOR_TYPE_BOOL;  out->arg = 1; out->next = p; return true; }
        if (additional == 22) { out->type = CBOR_TYPE_BOOL;  out->arg = 0; out->next = p; return true; }
        if (additional == 24) { // simple value (1-byte payload)
            if (p + 1 > end) return false;
            out->type = CBOR_TYPE_BOOL;  out->arg = p[0]; out->next = p + 1; return true;
        }
        if (additional == 25) { // IEEE 754 binary16
            if (p + 2 > end) return false;
            out->type = CBOR_TYPE_FLOAT; out->arg = 25; out->data = p; out->next = p + 2; return true;
        }
        if (additional == 26) { // IEEE 754 binary32
            if (p + 4 > end) return false;
            out->type = CBOR_TYPE_FLOAT; out->arg = 26; out->data = p; out->next = p + 4; return true;
        }
        if (additional == 27) { // IEEE 754 binary64
            if (p + 8 > end) return false;
            out->type = CBOR_TYPE_FLOAT; out->arg = 27; out->data = p; out->next = p + 8; return true;
        }
        return false;

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Map helpers
// ---------------------------------------------------------------------------

bool cbor_map_find(const uint8_t *map_data, const uint8_t *end,
                   uint64_t pair_count, const char *key,
                   cbor_item_t *out_value) {
    if (!map_data || !key || !out_value) return false;
    size_t key_len = strlen(key);
    const uint8_t *p = map_data;

    for (uint64_t i = 0; i < pair_count; i++) {
        cbor_item_t k;
        if (!cbor_parse(p, end, &k)) return false;
        cbor_item_t v;
        if (!cbor_parse(k.next, end, &v)) return false;
        if (k.type == CBOR_TYPE_TSTR && k.arg == key_len &&
            memcmp(k.data, key, key_len) == 0) {
            *out_value = v;
            return true;
        }
        p = v.next;
    }
    return false;
}

bool cbor_map_get_uint(const uint8_t *map_data, const uint8_t *end,
                      uint64_t pair_count, const char *key, uint64_t *out_val) {
    cbor_item_t v;
    if (!cbor_map_find(map_data, end, pair_count, key, &v)) return false;
    if (v.type != CBOR_TYPE_UINT) return false;
    *out_val = v.arg;
    return true;
}

bool cbor_map_get_text(const uint8_t *map_data, const uint8_t *end,
                       uint64_t pair_count, const char *key,
                       char *out_buf, size_t out_buf_size, size_t *out_len) {
    cbor_item_t v;
    if (!cbor_map_find(map_data, end, pair_count, key, &v)) return false;
    if (v.type != CBOR_TYPE_TSTR) return false;
    if (v.arg + 1 > out_buf_size) return false; // need room for NUL terminator
    memcpy(out_buf, v.data, (size_t)v.arg);
    out_buf[v.arg] = '\0';
    if (out_len) *out_len = (size_t)v.arg;
    return true;
}

bool cbor_map_get_bytes(const uint8_t *map_data, const uint8_t *end,
                        uint64_t pair_count, const char *key,
                        const uint8_t **out_ptr, size_t *out_len) {
    cbor_item_t v;
    if (!cbor_map_find(map_data, end, pair_count, key, &v)) return false;
    if (v.type != CBOR_TYPE_BSTR) return false;
    if (out_ptr) *out_ptr = v.data;
    if (out_len) *out_len = (size_t)v.arg;
    return true;
}
