// CBOR command dispatcher for the BLE File Command characteristic.
//
// Parses an incoming command map, runs the requested filesystem operation,
// and serialises a response map back into out_buf. The wire format is
// pinned by the iOS companion app (see ios/.../BLEManager.swift); field
// names here must match exactly.

#include "core/commands.h"
#include "core/attest_cache.h"
#include "core/attest_query.h"
#include "core/attestations.h"
#include "core/filestore.h"
#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/encounters.h"
#include "core/manifest_counter.h"
#include "core/ssid_scan.h"
#include "core/ssid_scan_hal.h"

#include <string.h>
#include <stdio.h>

// Provided by the active hw port (hw/pico-w/ble.c) or by tests/stub_ble.c.
// Keeping it as a forward declaration here lets core/ stay free of any
// hw/* include path.
uint16_t ble_get_mtu(void);

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
    if (out_max < 40) return -1;
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

// Number of bytes a CBOR header takes for an arg of value `v` (RFC 8949 §3).
static size_t cbor_arg_size(uint32_t v) {
    if (v <= 23)     return 1;
    if (v <= 0xFF)   return 2;
    if (v <= 0xFFFF) return 3;
    return 5;
}

static int resolve_hash_to_name(const uint8_t hash[8], char *out_name) {
    char names[16][FS_MAX_NAME_LEN];
    uint32_t sizes[16];
    uint8_t hashes[16][8];
    int next_offset = 0;
    int offset = 0;

    while (1) {
        int avail = fs_list(names, sizes, hashes, 16, offset, 16, &next_offset);
        if (avail < 0) return -1;
        for (int i = 0; i < avail; i++) {
            if (memcmp(hashes[i], hash, 8) == 0) {
                strncpy(out_name, names[i], FS_MAX_NAME_LEN);
                return 0;
            }
        }
        if (next_offset <= offset || avail == 0) break;
        offset = next_offset;
    }
    return -1;
}

static int resolve_filename(const uint8_t *body, const uint8_t *body_end, uint64_t pc, char *out_name) {
    if (cbor_map_get_text(body, body_end, pc, "name", out_name, FS_MAX_NAME_LEN, NULL)) {
        return 0;
    }

    const uint8_t *hash_ptr = NULL;
    size_t hash_len = 0;
    if (cbor_map_get_bytes(body, body_end, pc, "hash", &hash_ptr, &hash_len) && hash_len == 8) {
        return resolve_hash_to_name(hash_ptr, out_name);
    }

    return -1;
}

// Encoded size of one ls entry: map(3) of "name"+"size"+"hash". Must stay
// in sync with the writes in cmd_ls below — if you change one, change both.
static size_t ls_entry_size(const char *name, uint32_t size) {
    size_t name_len = strlen(name);
    return 1                                          // map(3) header
         + 1 + 4                                      // "name" key (text len 4)
         + cbor_arg_size((uint32_t)name_len) + name_len
         + 1 + 4                                      // "size" key
         + cbor_arg_size(size)                        // size uint
         + 1 + 4                                      // "hash" key
         + 1 + 8;                                     // bstr(8) hash
}

static int cmd_ls(const uint8_t *body, const uint8_t *body_end, uint64_t pc,
                  uint8_t *out, size_t out_max) {
    enum { PAGE = 16 };

    uint64_t offset_u = 0;
    cbor_map_get_uint(body, body_end, pc, "offset", &offset_u);
    int offset = (int)offset_u;

    char names[PAGE][FS_MAX_NAME_LEN];
    uint32_t sizes[PAGE];
    uint8_t hashes[PAGE][8];
    int fs_next = 0;
    int avail = fs_list(names, sizes, hashes, PAGE, offset, PAGE, &fs_next);
    if (avail < 0) return emit_error(out, out_max, "list failed");

    // Per-response byte budget. The notification has to fit in a single ATT
    // packet (mtu - 3) and within out_max. Reserve enough for the outer
    // envelope ("files" key + array header) and a possible "next_offset"
    // trailer. Worst-case envelope ≈ 9 bytes, trailer ≈ 18 bytes — round up.
    size_t mtu = (size_t)ble_get_mtu();
    size_t budget = (mtu > 3) ? (mtu - 3) : 20;
    if (budget > out_max) budget = out_max;
    const size_t ENVELOPE_RESERVE = 9;
    const size_t TRAILER_RESERVE  = 18;
    size_t entry_budget = (budget > ENVELOPE_RESERVE + TRAILER_RESERVE)
                          ? budget - ENVELOPE_RESERVE - TRAILER_RESERVE
                          : 0;

    int cnt = 0;
    size_t used = 0;
    for (int i = 0; i < avail; i++) {
        size_t esz = ls_entry_size(names[i], sizes[i]);
        if (used + esz > entry_budget) break;
        used += esz;
        cnt++;
    }

    // If we couldn't ship every entry fs_list handed us, the next page must
    // resume just past the last entry we encoded — overriding fs_next, which
    // only knows about *files* beyond the staged page.
    int next_offset = (cnt < avail) ? (offset + cnt) : fs_next;

    uint8_t *p = out;
    int n_fields = next_offset > 0 ? 2 : 1;
    p += cborencode_map_header(p, n_fields);

    p += cborencode_text_str(p, "files", 5);
    p += cborencode_array_header(p, cnt);
    for (int i = 0; i < cnt; i++) {
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
    if (out_max < 128) return emit_error(out, out_max, "buffer too small");
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
    return (int)(p - out);
}

static int cmd_read(const uint8_t *body, const uint8_t *body_end, uint64_t pc,
                    uint8_t *out, size_t out_max) {
    if (out_max < 256) return emit_error(out, out_max, "buffer too small");
    char name[FS_MAX_NAME_LEN];
    if (resolve_filename(body, body_end, pc, name) != 0)
        return emit_error(out, out_max, "missing name or hash");

    uint8_t scratch[MAX_INLINE_DATA];
    size_t cap = safe_chunk_for_response();
    int n = fs_read(name, scratch, cap);
    if (n < 0) return emit_error(out, out_max, "not found");

    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_byte_str(p, scratch, (size_t)n);
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
    manifest_counter_bump();    // /files/ changed → bump advertised hint
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
    manifest_counter_bump();    // /files/ changed → bump advertised hint
    return emit_ok(out, out_max);
}

static int cmd_read_start(const uint8_t *body, const uint8_t *body_end,
                          uint64_t pc, uint8_t *out, size_t out_max) {
    if (out_max < 64) return emit_error(out, out_max, "buffer too small");
    char name[FS_MAX_NAME_LEN];
    if (resolve_filename(body, body_end, pc, name) != 0)
        return emit_error(out, out_max, "missing name or hash");
    int sz = fs_read_start(name);
    if (sz < 0) return emit_error(out, out_max, "not found");

    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, (uint32_t)sz);
    return (int)(p - out);
}

static int cmd_read_chunk(const uint8_t *body, const uint8_t *body_end,
                          uint64_t pc, uint8_t *out, size_t out_max) {
    if (out_max < 256) return emit_error(out, out_max, "buffer too small");
    char name[FS_MAX_NAME_LEN];
    uint64_t offset_u = 0, size_u = MAX_INLINE_DATA;
    if (resolve_filename(body, body_end, pc, name) != 0)
        return emit_error(out, out_max, "missing name or hash");
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
    return (int)(p - out);
}

static int cmd_delete(const uint8_t *body, const uint8_t *body_end,
                      uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (resolve_filename(body, body_end, pc, name) != 0)
        return emit_error(out, out_max, "missing name or hash");
    if (fs_delete(name) != 0) return emit_error(out, out_max, "not found");
    // Note: deletes do NOT bump the manifest counter (M4 stage 7). The
    // counter is a "new content available" hint for peers, and our sync
    // protocol never deletes — peers that already pulled the file keep
    // it locally. Bumping here would just trigger redundant peer
    // connects (and now redundant encounter handshakes) that propagate
    // nothing useful. A subsequent re-add of the same name still bumps
    // via cmd_write / cmd_write_end.
    return emit_ok(out, out_max);
}

static int cmd_read_meta(const uint8_t *body, const uint8_t *body_end,
                         uint64_t pc, uint8_t *out, size_t out_max) {
    char name[FS_MAX_NAME_LEN];
    if (resolve_filename(body, body_end, pc, name) != 0)
        return emit_error(out, out_max, "missing name or hash");

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
    if (resolve_filename(body, body_end, pc, name) != 0)
        return emit_error(out, out_max, "missing name or hash");

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

// Conservative estimate of one observation's encoded size. Uses worst-case
// encodings for rssi/channel (2 bytes) so we never over-pack the budget.
static size_t obs_entry_size(uint8_t ssid_len) {
    size_t s = 1;                                          // map(4) header
    s += 1 + 5;                                            // "bssid" key
    s += 2 + 6;                                            // bstr(6)
    s += 1 + 4;                                            // "ssid" key
    s += (ssid_len <= 23 ? 1 : 2) + ssid_len;              // tstr(L)
    s += 1 + 4;                                            // "rssi" key
    s += 2;                                                // worst-case neg int
    s += 1 + 7;                                            // "channel" key
    s += 2;                                                // worst-case uint
    return s;
}

static int cmd_scan_get(const uint8_t *body, const uint8_t *body_end, uint64_t pc,
                        uint8_t *out, size_t out_max) {
    ssid_scan_t scan;
    if (!ssid_scan_hal_latest(&scan)) {
        return emit_error(out, out_max, "no scan");
    }
    uint64_t offset_u = 0;
    cbor_map_get_uint(body, body_end, pc, "offset", &offset_u);
    uint8_t start = (offset_u > scan.count) ? scan.count : (uint8_t)offset_u;

    // Budget the response against the negotiated MTU. Worst-case envelope:
    //   map(4) hdr (1) + "ok"+true (4) + "scanned_ms"+uint64 (21)
    //   + "obs"+array_hdr (5) + "next_offset"+uint (14) ≈ 45 bytes. Reserve 48.
    size_t mtu    = (size_t)ble_get_mtu();
    size_t budget = (mtu > 3) ? (mtu - 3) : 20;
    if (budget > out_max) budget = out_max;
    const size_t ENVELOPE_RESERVE = 48;
    size_t entry_budget = (budget > ENVELOPE_RESERVE) ? budget - ENVELOPE_RESERVE : 0;

    uint8_t cnt  = 0;
    size_t  used = 0;
    for (uint8_t i = start; i < scan.count; i++) {
        size_t esz = obs_entry_size(scan.obs[i].ssid_len);
        if (used + esz > entry_budget) break;
        used += esz;
        cnt++;
    }
    uint8_t  next_offset = (uint8_t)(start + cnt);
    bool     has_next    = next_offset < scan.count;
    uint8_t  field_count = has_next ? 4 : 3;

    uint8_t *p = out;
    p += cborencode_map_header(p, field_count);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "scanned_ms", 10);
    p += cborencode_uint(p, scan.scanned_ms);
    p += cborencode_text_str(p, "obs", 3);
    p += cborencode_array_header(p, cnt);
    for (uint8_t i = start; i < start + cnt; i++) {
        p += cborencode_map_header(p, 4);
        p += cborencode_text_str(p, "bssid", 5);
        p += cborencode_byte_str(p, scan.obs[i].bssid, 6);
        p += cborencode_text_str(p, "ssid", 4);
        p += cborencode_text_str(p, (const char*)scan.obs[i].ssid, scan.obs[i].ssid_len);
        p += cborencode_text_str(p, "rssi", 4);
        p += cborencode_int(p, scan.obs[i].rssi);
        p += cborencode_text_str(p, "channel", 7);
        p += cborencode_uint(p, scan.obs[i].channel);
    }
    if (has_next) {
        p += cborencode_text_str(p, "next_offset", 11);
        p += cborencode_uint(p, next_offset);
    }
    return (int)(p - out);
}

static int cmd_attestation_write(const uint8_t *body, const uint8_t *body_end,
                                  uint64_t pc, uint8_t *out, size_t out_max) {
    const uint8_t *blob_ptr = NULL;
    size_t         blob_len = 0;
    if (!cbor_map_get_bytes(body, body_end, pc, "blob", &blob_ptr, &blob_len) ||
        blob_ptr == NULL || blob_len == 0) {
        return emit_error(out, out_max, "missing blob");
    }
    if (attestations_write(blob_ptr, blob_len) != 0) {
        return emit_error(out, out_max, "attestation_write failed");
    }
    return emit_ok(out, out_max);
}

typedef struct {
    uint32_t skip_remaining;
    size_t   budget;
    size_t   used;
    uint32_t to_emit;
    uint32_t emitted;
    uint8_t *p;
    bool     overflow;
} attest_walk_t;

static size_t attest_outer_size(size_t blob_len) {
    if (blob_len <= 23)   return 1 + blob_len;
    if (blob_len <= 0xFF) return 2 + blob_len;
    return 3 + blob_len;
}

static int attest_count_cb(void *ctx, const uint8_t *blob, size_t len) {
    attest_walk_t *w = (attest_walk_t*)ctx;
    if (w->skip_remaining > 0) { w->skip_remaining--; return 0; }
    size_t outer = attest_outer_size(len);
    if (w->used + outer > w->budget) { w->overflow = true; return 1; }
    w->used += outer;
    w->to_emit++;
    return 0;
}

static int attest_emit_cb(void *ctx, const uint8_t *blob, size_t len) {
    attest_walk_t *w = (attest_walk_t*)ctx;
    if (w->skip_remaining > 0) { w->skip_remaining--; return 0; }
    if (w->emitted >= w->to_emit) return 1;
    w->p += cborencode_byte_str(w->p, blob, len);
    w->emitted++;
    return 0;
}

static int cmd_attestations_get(const uint8_t *body, const uint8_t *body_end,
                                 uint64_t pc, uint8_t *out, size_t out_max) {
    uint64_t offset_u = 0;
    cbor_map_get_uint(body, body_end, pc, "offset", &offset_u);

    size_t mtu    = (size_t)ble_get_mtu();
    size_t budget = (mtu > 3) ? (mtu - 3) : 20;
    if (budget > out_max) budget = out_max;
    const size_t ENVELOPE_RESERVE = 32;
    size_t rec_budget = (budget > ENVELOPE_RESERVE) ? budget - ENVELOPE_RESERVE : 0;

    attest_walk_t w = {
        .skip_remaining = (uint32_t)offset_u,
        .budget         = rec_budget,
    };
    attestations_for_each(&w, attest_count_cb);

    bool     has_next    = w.overflow;
    uint32_t next_offset = (uint32_t)offset_u + w.to_emit;

    uint8_t *p = out;
    p += cborencode_map_header(p, has_next ? 3 : 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "records", 7);
    p += cborencode_array_header(p, w.to_emit);

    attest_walk_t emit = {
        .skip_remaining = (uint32_t)offset_u,
        .to_emit        = w.to_emit,
        .p              = p,
    };
    attestations_for_each(&emit, attest_emit_cb);
    p = emit.p;

    if (has_next) {
        p += cborencode_text_str(p, "next_offset", 11);
        p += cborencode_uint(p, next_offset);
    }
    (void)out_max;
    return (int)(p - out);
}

// State for the two-pass walk used by cmd_encounters_get.
typedef struct {
    uint32_t since_ms;
    uint32_t skip_remaining;   // records to skip (already returned in earlier pages)
    size_t   budget;           // bytes available for record payload
    size_t   used;             // bytes already accounted for
    uint32_t to_emit;          // records that will fit in this page
    uint32_t emitted;          // records emitted in pass 2
    uint8_t *p;                // write cursor in pass 2
    bool     overflow;         // true if at least one record didn't fit
} enc_walk_t;

static size_t enc_record_outer_size(size_t rec_len) {
    // bstr header: 1 byte for ≤23, 2 bytes for ≤255, 3 for ≤65535.
    if (rec_len <= 23)   return 1 + rec_len;
    if (rec_len <= 0xFF) return 2 + rec_len;
    return 3 + rec_len;
}

static int enc_count_cb(void *ctx, const uint8_t *cbor, size_t len) {
    enc_walk_t *w = (enc_walk_t*)ctx;
    uint32_t cap_at = 0;
    if (!encounters_record_captured_at(cbor, len, &cap_at)) return 0;
    if (cap_at <= w->since_ms) return 0;
    if (w->skip_remaining > 0) { w->skip_remaining--; return 0; }
    size_t outer = enc_record_outer_size(len);
    if (w->used + outer > w->budget) {
        w->overflow = true;
        return 1; // stop scanning
    }
    w->used += outer;
    w->to_emit++;
    return 0;
}

static int enc_emit_cb(void *ctx, const uint8_t *cbor, size_t len) {
    enc_walk_t *w = (enc_walk_t*)ctx;
    uint32_t cap_at = 0;
    if (!encounters_record_captured_at(cbor, len, &cap_at)) return 0;
    if (cap_at <= w->since_ms) return 0;
    if (w->skip_remaining > 0) { w->skip_remaining--; return 0; }
    if (w->emitted >= w->to_emit) return 1;
    w->p += cborencode_byte_str(w->p, cbor, len);
    w->emitted++;
    return 0;
}

static int cmd_encounters_get(const uint8_t *body, const uint8_t *body_end,
                              uint64_t pc, uint8_t *out, size_t out_max) {
    uint64_t since_ms_u = 0, offset_u = 0;
    cbor_map_get_uint(body, body_end, pc, "since_ms", &since_ms_u);
    cbor_map_get_uint(body, body_end, pc, "offset",   &offset_u);

    size_t mtu = (size_t)ble_get_mtu();
    size_t budget = (mtu > 3) ? (mtu - 3) : 20;
    if (budget > out_max) budget = out_max;
    // Outer envelope: map(2|3) + "ok"+true + "records"+array_hdr + optional next_offset.
    const size_t ENVELOPE_RESERVE = 32;
    size_t rec_budget = (budget > ENVELOPE_RESERVE) ? budget - ENVELOPE_RESERVE : 0;

    enc_walk_t w = {
        .since_ms       = (uint32_t)since_ms_u,
        .skip_remaining = (uint32_t)offset_u,
        .budget         = rec_budget,
        .used           = 0,
        .to_emit        = 0,
        .emitted        = 0,
        .p              = NULL,
        .overflow       = false,
    };
    encounters_for_each(&w, enc_count_cb);

    bool has_next = w.overflow;
    uint32_t next_offset = (uint32_t)offset_u + w.to_emit;

    uint8_t *p = out;
    p += cborencode_map_header(p, has_next ? 3 : 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "records", 7);
    p += cborencode_array_header(p, w.to_emit);

    enc_walk_t emit = {
        .since_ms       = (uint32_t)since_ms_u,
        .skip_remaining = (uint32_t)offset_u,
        .to_emit        = w.to_emit,
        .emitted        = 0,
        .p              = p,
    };
    encounters_for_each(&emit, enc_emit_cb);
    p = emit.p;

    if (has_next) {
        p += cborencode_text_str(p, "next_offset", 11);
        p += cborencode_uint(p, next_offset);
    }
    (void)out_max;
    return (int)(p - out);
}

// ---------------------------------------------------------------------------
// attest_query / attest_ingest — peer-to-peer attestation lookup (M3 stage 3)
// ---------------------------------------------------------------------------
//
// attest_query inputs `{cmd, bssids:[bstr...], offset?}` and answers
// with `{ok, blobs:[bstr...], next_offset?}`. The answering side walks
// both the local self-ring (attestations) and the peer cache-ring
// (attest_cache); blobs whose claimed BSSID set intersects the query
// are returned, paginated against the negotiated MTU.
//
// attest_ingest inputs `{cmd, blobs:[bstr...]}` and feeds each blob
// into attest_cache. Returns `{ok, stored, dup, err}` so the caller
// can tell which arms of the ring the records hit.

#define ATTEST_QUERY_MAX_BSSIDS 12

typedef struct {
    uint32_t skip_remaining;
    size_t   budget;
    size_t   used;
    uint32_t to_emit;
    uint32_t emitted;
    uint8_t *p;
    bool     overflow;
} attq_walk_t;

static size_t attq_outer_size(size_t blob_len) {
    if (blob_len <= 23)   return 1 + blob_len;
    if (blob_len <= 0xFF) return 2 + blob_len;
    return 3 + blob_len;
}

static int attq_count_cb(void *ctx, const uint8_t *blob, size_t len) {
    attq_walk_t *w = (attq_walk_t*)ctx;
    (void)blob;
    if (w->skip_remaining > 0) { w->skip_remaining--; return 0; }
    size_t outer = attq_outer_size(len);
    if (w->used + outer > w->budget) { w->overflow = true; return 1; }
    w->used += outer;
    w->to_emit++;
    return 0;
}

static int attq_emit_cb(void *ctx, const uint8_t *blob, size_t len) {
    attq_walk_t *w = (attq_walk_t*)ctx;
    if (w->skip_remaining > 0) { w->skip_remaining--; return 0; }
    if (w->emitted >= w->to_emit) return 1;
    w->p += cborencode_byte_str(w->p, blob, len);
    w->emitted++;
    return 0;
}

static int cmd_attest_query(const uint8_t *body, const uint8_t *body_end,
                            uint64_t pc, uint8_t *out, size_t out_max) {
    cbor_item_t arr;
    if (!cbor_map_find(body, body_end, pc, "bssids", &arr) ||
        arr.type != CBOR_TYPE_ARRAY) {
        return emit_error(out, out_max, "missing bssids");
    }

    uint8_t  query[ATTEST_QUERY_MAX_BSSIDS][6];
    int      qcount = 0;
    const uint8_t *p_in = arr.data;
    for (uint64_t i = 0; i < arr.arg && qcount < ATTEST_QUERY_MAX_BSSIDS; i++) {
        cbor_item_t item;
        if (!cbor_parse(p_in, body_end, &item)) {
            return emit_error(out, out_max, "bad bssid array");
        }
        p_in = item.next;
        if (item.type != CBOR_TYPE_BSTR || item.arg != 6) continue;
        memcpy(query[qcount], item.data, 6);
        qcount++;
    }
    if (qcount == 0) {
        return emit_error(out, out_max, "no valid bssids");
    }

    uint64_t offset_u = 0;
    cbor_map_get_uint(body, body_end, pc, "offset", &offset_u);

    size_t mtu    = (size_t)ble_get_mtu();
    size_t budget = (mtu > 3) ? (mtu - 3) : 20;
    if (budget > out_max) budget = out_max;
    const size_t ENVELOPE_RESERVE = 32;
    size_t rec_budget = (budget > ENVELOPE_RESERVE) ? budget - ENVELOPE_RESERVE : 0;

    attq_walk_t w = {
        .skip_remaining = (uint32_t)offset_u,
        .budget         = rec_budget,
    };
    attest_query_for_each(query, qcount, &w, attq_count_cb);

    bool     has_next    = w.overflow;
    uint32_t next_offset = (uint32_t)offset_u + w.to_emit;

    uint8_t *p = out;
    p += cborencode_map_header(p, has_next ? 3 : 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "blobs", 5);
    p += cborencode_array_header(p, w.to_emit);

    attq_walk_t emit = {
        .skip_remaining = (uint32_t)offset_u,
        .to_emit        = w.to_emit,
        .p              = p,
    };
    attest_query_for_each(query, qcount, &emit, attq_emit_cb);
    p = emit.p;

    if (has_next) {
        p += cborencode_text_str(p, "next_offset", 11);
        p += cborencode_uint(p, next_offset);
    }
    (void)out_max;
    return (int)(p - out);
}

static int cmd_attest_ingest(const uint8_t *body, const uint8_t *body_end,
                             uint64_t pc, uint8_t *out, size_t out_max) {
    cbor_item_t arr;
    if (!cbor_map_find(body, body_end, pc, "blobs", &arr) ||
        arr.type != CBOR_TYPE_ARRAY) {
        return emit_error(out, out_max, "missing blobs");
    }

    uint32_t stored = 0, dup = 0, err = 0;
    const uint8_t *p_in = arr.data;
    for (uint64_t i = 0; i < arr.arg; i++) {
        cbor_item_t item;
        if (!cbor_parse(p_in, body_end, &item)) {
            return emit_error(out, out_max, "bad blobs array");
        }
        p_in = item.next;
        if (item.type != CBOR_TYPE_BSTR || item.arg == 0) { err++; continue; }
        int rc = attest_cache_write(item.data, (size_t)item.arg);
        if      (rc == 1) stored++;
        else if (rc == 0) dup++;
        else              err++;
    }

    uint8_t *p = out;
    p += cborencode_map_header(p, 4);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "stored", 6);
    p += cborencode_uint(p, stored);
    p += cborencode_text_str(p, "dup", 3);
    p += cborencode_uint(p, dup);
    p += cborencode_text_str(p, "err", 3);
    p += cborencode_uint(p, err);
    (void)out_max;
    return (int)(p - out);
}

// ---------------------------------------------------------------------------
// Top-level dispatch
// ---------------------------------------------------------------------------

// True if `cmd_name` is permitted on the peer characteristic. Peer
// sessions (post-encounter-handshake, M4 stage 6) get only the read
// surface they need to drive peer_sync — list, read, read_meta,
// storage_info — plus the attestation-exchange commands introduced
// in M3. Anything that mutates local state or exposes companion-private
// data (live SSID scans, the encounter store, attestations metadata)
// stays companion-only.
//
// Defense in depth: even if a peer's firmware skips the encounter
// handshake, the responder rejects mutating commands. Peers that need
// to push data must do so via files/, not via cmd_write.
static bool peer_allowed(const char *cmd_name) {
    return strcmp(cmd_name, "ls")            == 0 ||
           strcmp(cmd_name, "storage_info")  == 0 ||
           strcmp(cmd_name, "read")          == 0 ||
           strcmp(cmd_name, "read_start")    == 0 ||
           strcmp(cmd_name, "read_chunk")    == 0 ||
           strcmp(cmd_name, "read_meta")     == 0 ||
           strcmp(cmd_name, "attest_query")  == 0 ||
           strcmp(cmd_name, "attest_ingest") == 0;
}

int commands_handle_session(commands_session_kind_t kind,
                            const uint8_t *cmd_data, size_t cmd_len,
                            uint8_t *out_buf, size_t out_max) {
    if (!cmd_data || !out_buf || cmd_len == 0 || out_max < 32) return -1;

    cbor_item_t root;
    const uint8_t *end = cmd_data + cmd_len;
    if (!cbor_parse(cmd_data, end, &root) || root.type != CBOR_TYPE_MAP)
        return emit_error(out_buf, out_max, "bad request");

    uint64_t pc = root.arg;
    const uint8_t *body = root.data;
    const uint8_t *body_end = end;

    char cmd_name[24];
    if (!cbor_map_get_text(body, body_end, pc, "cmd", cmd_name, sizeof(cmd_name), NULL))
        return emit_error(out_buf, out_max, "missing cmd");

    if (kind == COMMANDS_SESSION_PEER && !peer_allowed(cmd_name)) {
        return emit_error(out_buf, out_max, "companion only");
    }

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
    else if (strcmp(cmd_name, "scan_get") == 0)      rc = cmd_scan_get(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "encounters_get") == 0) rc = cmd_encounters_get(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "attestation_write") == 0) rc = cmd_attestation_write(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "attestations_get") == 0) rc = cmd_attestations_get(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "attest_query") == 0)  rc = cmd_attest_query(body, body_end, pc, out_buf, out_max);
    else if (strcmp(cmd_name, "attest_ingest") == 0) rc = cmd_attest_ingest(body, body_end, pc, out_buf, out_max);
    else                                             rc = emit_error(out_buf, out_max, "unknown cmd");

    if (rc < 0) rc = emit_error(out_buf, out_max, "internal error");
    return rc;
}

int commands_handle(const uint8_t *cmd_data, size_t cmd_len,
                    uint8_t *out_buf, size_t out_max) {
    return commands_handle_session(COMMANDS_SESSION_COMPANION,
                                   cmd_data, cmd_len, out_buf, out_max);
}
