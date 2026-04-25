// CBOR command dispatcher for the BLE File Command characteristic.
//
// Parses an incoming command map, runs the requested filesystem operation,
// and serialises a response map back into out_buf. The wire format is
// pinned by the iOS companion app (see ios/.../BLEManager.swift); field
// names here must match exactly.

#include "commands.h"
#include "filestore.h"
#include "cborencode.h"
#include "cbor_decode.h"
#include "ble.h"

#include <string.h>
#include <stdio.h>

// Worst-case CBOR envelope around a `data` payload in a response map:
//   map(2) + "ok"+true + "data" + bstr_hdr(2) ≈ 14 bytes. Round up.
#define RESPONSE_ENVELOPE_BUDGET 16

// Hard cap on inline data we ever try to ship in one notification. The
// effective cap is `min(MAX_INLINE_DATA, mtu - 3 - envelope)` and is
// computed at call time so we never overflow what the central can
// actually receive.
#define MAX_INLINE_DATA 220

// Compute the largest payload chunk we can stuff into a response so that
// the encoded notification fits the negotiated ATT MTU.
static size_t safe_chunk_for_response(void) {
    int mtu = (int)ble_get_mtu();
    int budget = mtu - 3 - RESPONSE_ENVELOPE_BUDGET;
    if (budget < 16) budget = 16;            // keep something usable even at min MTU
    if (budget > MAX_INLINE_DATA) budget = MAX_INLINE_DATA;
    return (size_t)budget;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

static int emit_error(uint8_t *out, size_t out_max, const char *msg) {
    if (out_max < 32) return -1;
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "error", 5);
    size_t mlen = strlen(msg);
    if (mlen > 24) mlen = 24;
    p += cborencode_text_str(p, msg, mlen);
    return (int)(p - out);
}

static int emit_ok(uint8_t *out, size_t out_max) {
    if (out_max < 8) return -1;
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    return (int)(p - out);
}

// ---------------------------------------------------------------------------
// Per-command implementations
//
// All command handlers receive `body` (start of map pairs), `body_end`
// (one past the last input byte), and `pair_count`. They write their
// response into `out` and return the response length, or call emit_error.
// ---------------------------------------------------------------------------

static int cmd_ls(const uint8_t *body, const uint8_t *body_end, uint64_t pc,
                  uint8_t *out, size_t out_max) {
    enum { PAGE = 16 };

    uint64_t offset_u = 0;
    cbor_map_get_uint(body, body_end, pc, "offset", &offset_u);
    int offset = (int)offset_u;

    char names[PAGE][FS_MAX_NAME_LEN];
    uint32_t sizes[PAGE];
    uint8_t hashes[PAGE][8];
    int next_offset = 0;
    int count = fs_list(names, sizes, hashes, PAGE, offset, PAGE, &next_offset);
    if (count < 0) return emit_error(out, out_max, "list failed");

    uint8_t *p = out;
    int n_fields = next_offset > 0 ? 2 : 1;
    p += cborencode_map_header(p, n_fields);

    p += cborencode_text_str(p, "files", 5);
    p += cborencode_array_header(p, count);
    for (int i = 0; i < count; i++) {
        p += cborencode_map_header(p, 3);
        p += cborencode_text_str(p, "name", 4);
        p += cborencode_text_str(p, names[i], strlen(names[i]));
        p += cborencode_text_str(p, "size", 4);
        p += cborencode_uint(p, sizes[i]);
        p += cborencode_text_str(p, "hash", 4);
        p += cborencode_byte_str(p, hashes[i], 8);
    }

    if (next_offset > 0) {
        p += cborencode_text_str(p, "next_offset", 11);
        p += cborencode_uint(p, (uint32_t)next_offset);
    }
    return (int)(p - out);
}

static int cmd_storage_info(uint8_t *out, size_t out_max) {
    uint32_t free_b = 0, used_b = 0, reserve_b = 0, count_b = 0;
    fs_storage_info(&free_b, &used_b, &reserve_b, &count_b);

    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "info", 4);
    p += cborencode_map_header(p, 4);

    // Field names match StorageInfo.fromCBOR on the iOS side.
    p += cborencode_text_str(p, "file_count", 10);
    p += cborencode_uint(p, count_b);
    p += cborencode_text_str(p, "free", 4);
    p += cborencode_uint(p, free_b);
    p += cborencode_text_str(p, "reserve", 7);
    p += cborencode_uint(p, reserve_b);
    p += cborencode_text_str(p, "used", 4);
    p += cborencode_uint(p, used_b);
    (void)out_max;
    return (int)(p - out);
}

static int cmd_read(const uint8_t *body, const uint8_t *body_end, uint64_t pc,
                    uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");

    uint8_t scratch[MAX_INLINE_DATA];
    size_t cap = safe_chunk_for_response();
    int n = fs_read(name, scratch, cap);
    if (n < 0) return emit_error(out, out_max, "not found");

    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_text_str(p, (const char *)scratch, (size_t)n);
    return (int)(p - out);
}

static int cmd_write(const uint8_t *body, const uint8_t *body_end, uint64_t pc,
                     uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");

    // iOS sends `data` as a CBOR text string for plain-text writes.
    cbor_item_t v;
    if (!cbor_map_find(body, body_end, pc, "data", &v) || v.type != CBOR_TYPE_TSTR)
        return emit_error(out, out_max, "missing data");
    if (v.arg > MAX_INLINE_DATA) return emit_error(out, out_max, "too big");

    if (fs_write(name, v.data, (size_t)v.arg) != 0)
        return emit_error(out, out_max, "write failed");
    return emit_ok(out, out_max);
}

static int cmd_write_start(const uint8_t *body, const uint8_t *body_end,
                           uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    uint64_t total = 0;
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");
    if (!cbor_map_get_uint(body, body_end, pc, "size", &total))
        return emit_error(out, out_max, "missing size");

    if (fs_chunked_start(name, (uint32_t)total) != 0)
        return emit_error(out, out_max, "write_start failed");
    return emit_ok(out, out_max);
}

static int cmd_write_chunk(const uint8_t *body, const uint8_t *body_end,
                           uint64_t pc, uint8_t *out, size_t out_max) {
    // We don't strictly need the name here — only one chunked write can be
    // active at a time — but accept and ignore it for protocol clarity.
    cbor_item_t v;
    if (!cbor_map_find(body, body_end, pc, "data", &v) || v.type != CBOR_TYPE_BSTR)
        return emit_error(out, out_max, "missing data");
    if (fs_chunked_append(v.data, (size_t)v.arg) < 0)
        return emit_error(out, out_max, "write_chunk failed");
    return emit_ok(out, out_max);
}

static int cmd_write_end(const uint8_t *body, const uint8_t *body_end,
                         uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");
    if (fs_chunked_finish(name) < 0)
        return emit_error(out, out_max, "size mismatch");
    return emit_ok(out, out_max);
}

static int cmd_read_start(const uint8_t *body, const uint8_t *body_end,
                          uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");
    int sz = fs_read_start(name);
    if (sz < 0) return emit_error(out, out_max, "not found");

    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, (uint32_t)sz);
    (void)out_max;
    return (int)(p - out);
}

static int cmd_read_chunk(const uint8_t *body, const uint8_t *body_end,
                          uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    uint64_t offset_u = 0, size_u = MAX_INLINE_DATA;
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");
    cbor_map_get_uint(body, body_end, pc, "offset", &offset_u);
    cbor_map_get_uint(body, body_end, pc, "size", &size_u);
    size_t mtu_cap = safe_chunk_for_response();
    if (size_u > mtu_cap) size_u = mtu_cap;

    uint8_t scratch[MAX_INLINE_DATA];
    int n = fs_read_chunk(name, (uint32_t)offset_u, (uint32_t)size_u,
                          scratch, sizeof(scratch));
    if (n < 0) return emit_error(out, out_max, "read_chunk failed");

    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_byte_str(p, scratch, (size_t)n);
    (void)out_max;
    return (int)(p - out);
}

static int cmd_delete(const uint8_t *body, const uint8_t *body_end,
                      uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");
    if (fs_delete(name) != 0) return emit_error(out, out_max, "not found");
    return emit_ok(out, out_max);
}

static int cmd_read_meta(const uint8_t *body, const uint8_t *body_end,
                         uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");

    uint8_t meta_buf[200];
    int n = fs_read_meta(name, meta_buf, sizeof(meta_buf));
    if (n < 0) {
        // No sidecar: return empty meta map so iOS gets a defined shape.
        uint8_t *p = out;
        p += cborencode_map_header(p, 1);
        p += cborencode_text_str(p, "meta", 4);
        p += cborencode_map_header(p, 0);
        return (int)(p - out);
    }

    // The sidecar already contains a CBOR-encoded map (written verbatim
    // by cmd_write_meta), so inline its bytes into the response after
    // the "meta" key.
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "meta", 4);
    if (p + n > out + out_max) return emit_error(out, out_max, "meta too big");
    memcpy(p, meta_buf, (size_t)n);
    p += n;
    return (int)(p - out);
}

static int cmd_write_meta(const uint8_t *body, const uint8_t *body_end,
                          uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (!cbor_map_get_text(body, body_end, pc, "name", name, sizeof(name), NULL))
        return emit_error(out, out_max, "missing name");

    cbor_item_t meta;
    if (!cbor_map_find(body, body_end, pc, "meta", &meta) || meta.type != CBOR_TYPE_MAP)
        return emit_error(out, out_max, "missing meta");

    // Persist the raw CBOR bytes for the meta map so we can hand them
    // back verbatim on read_meta.
    size_t raw_len = (size_t)(meta.next - meta.start);
    if (fs_write_meta(name, meta.start, raw_len) != 0)
        return emit_error(out, out_max, "write_meta failed");
    return emit_ok(out, out_max);
}

// ---------------------------------------------------------------------------
// Top-level dispatch
// ---------------------------------------------------------------------------

int commands_handle(const uint8_t *cmd_data, size_t cmd_len,
                    uint8_t *out_buf, size_t out_max) {
    if (!cmd_data || !out_buf || cmd_len == 0 || out_max < 32) return -1;

    cbor_item_t root;
    const uint8_t *end = cmd_data + cmd_len;
    if (!cbor_parse(cmd_data, end, &root) || root.type != CBOR_TYPE_MAP)
        return emit_error(out_buf, out_max, "bad request");

    uint64_t pc = root.arg;
    const uint8_t *body = root.data;
    const uint8_t *body_end = end;

    char cmd_name[16];
    if (!cbor_map_get_text(body, body_end, pc, "cmd", cmd_name, sizeof(cmd_name), NULL))
        return emit_error(out_buf, out_max, "missing cmd");

    int rc;
    if      (strcmp(cmd_name, "ls") == 0)            rc = cmd_ls(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "storage_info") == 0)  rc = cmd_storage_info(out_buf, out_max);
    else if (strcmp(cmd_name, "read") == 0)          rc = cmd_read(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "write") == 0)         rc = cmd_write(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "write_start") == 0)   rc = cmd_write_start(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "write_chunk") == 0)   rc = cmd_write_chunk(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "write_end") == 0)     rc = cmd_write_end(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "read_start") == 0)    rc = cmd_read_start(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "read_chunk") == 0)    rc = cmd_read_chunk(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "delete") == 0)        rc = cmd_delete(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "read_meta") == 0)     rc = cmd_read_meta(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "write_meta") == 0)    rc = cmd_write_meta(body, body_end, pc, out_buf, out_max);
    else                                             rc = emit_error(out_buf, out_max, "unknown cmd");

    if (rc < 0) rc = emit_error(out_buf, out_max, "internal error");
    return rc;
}
