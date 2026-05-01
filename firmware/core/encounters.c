#include "core/encounters.h"

#include <stdio.h>
#include <string.h>

#include "core/cborencode.h"
#include "core/filestore.h"
#include "core/hal_crypto.h"

#define HEADER_BYTES   16
#define BUFFER_BYTES   (HEADER_BYTES + ENCOUNTERS_CAP * ENCOUNTERS_SLOT_BYTES)
#define BLOB_NAME      "encounters.bin"

// CBOR keys: integers 1..5 to keep records compact.
#define KEY_V       1
#define KEY_NODE    2
#define KEY_TIME    3
#define KEY_BSSIDS  4
#define KEY_SIG     5

static uint8_t g_buf[BUFFER_BYTES];
static bool    g_initialized = false;

static uint8_t  *header_ptr(void)         { return g_buf; }
static uint8_t  *slot_ptr(uint8_t i)      { return g_buf + HEADER_BYTES + (size_t)i * ENCOUNTERS_SLOT_BYTES; }
static uint16_t  slot_record_len(uint8_t i) {
    const uint8_t *p = slot_ptr(i);
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static const uint8_t *slot_record_ptr(uint8_t i) { return slot_ptr(i) + 2; }

static uint8_t  next_slot_get(void)       { return header_ptr()[6]; }
static void     next_slot_set(uint8_t v)  { header_ptr()[6] = v; }
static uint8_t  used_count_get(void)      { return header_ptr()[7]; }
static void     used_count_set(uint8_t v) { header_ptr()[7] = v; }

static void blank_buffer(void) {
    memset(g_buf, 0, sizeof(g_buf));
    uint8_t *h = header_ptr();
    h[0] = ENCOUNTERS_MAGIC0;
    h[1] = ENCOUNTERS_MAGIC1;
    h[2] = ENCOUNTERS_MAGIC2;
    h[3] = ENCOUNTERS_MAGIC3;
    h[4] = ENCOUNTERS_VERSION;
    h[5] = 0; // reserved
    h[6] = 0; // next_slot
    h[7] = 0; // used_count
    // h[8..15] reserved
}

static bool header_valid(const uint8_t *h) {
    return h[0] == ENCOUNTERS_MAGIC0 && h[1] == ENCOUNTERS_MAGIC1 &&
           h[2] == ENCOUNTERS_MAGIC2 && h[3] == ENCOUNTERS_MAGIC3 &&
           h[4] == ENCOUNTERS_VERSION &&
           h[6] < ENCOUNTERS_CAP && h[7] <= ENCOUNTERS_CAP;
}

void encounters_reset(void) {
    blank_buffer();
    g_initialized = true;
}

void encounters_init(void) {
    int n = fs_system_read(BLOB_NAME, g_buf, sizeof(g_buf));
    if (n != (int)sizeof(g_buf) || !header_valid(header_ptr())) {
        blank_buffer();
        // Don't bother persisting an empty buffer until the first record.
    }
    g_initialized = true;
}

static uint8_t newest_slot(void) {
    // (next_slot - 1) mod ENCOUNTERS_CAP
    uint8_t ns = next_slot_get();
    return (uint8_t)((ns + ENCOUNTERS_CAP - 1) % ENCOUNTERS_CAP);
}

static uint8_t oldest_slot(void) {
    if (used_count_get() < ENCOUNTERS_CAP) return 0;
    return next_slot_get();
}

// Encode the signed body of a record (everything except the signature) into
// `out`. Returns bytes written.
static size_t encode_signed_body(uint8_t *out,
                                 const uint8_t node[32],
                                 uint32_t captured_at_ms,
                                 const ssid_scan_t *scan) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 4); // v, node, time, bssids
    p += cborencode_uint(p, KEY_V);
    p += cborencode_uint(p, ENCOUNTERS_VERSION);
    p += cborencode_uint(p, KEY_NODE);
    p += cborencode_byte_str(p, node, 32);
    p += cborencode_uint(p, KEY_TIME);
    p += cborencode_uint(p, captured_at_ms);
    p += cborencode_uint(p, KEY_BSSIDS);
    p += cborencode_array_header(p, scan->count);
    for (uint8_t i = 0; i < scan->count; i++) {
        p += cborencode_byte_str(p, scan->obs[i].bssid, 6);
    }
    return (size_t)(p - out);
}

// Encode the full record (signed body + sig field, wrapped in a 5-key map).
// We re-emit the signed fields then append KEY_SIG to keep parsing trivial.
// Returns bytes written, or 0 if the record would not fit in a slot.
static size_t encode_full_record(uint8_t *out, size_t out_max,
                                 const uint8_t node[32],
                                 uint32_t captured_at_ms,
                                 const ssid_scan_t *scan,
                                 const uint8_t sig[64]) {
    if (out_max < ENCOUNTERS_SLOT_BYTES - 2) return 0;
    uint8_t *p = out;
    p += cborencode_map_header(p, 5);
    p += cborencode_uint(p, KEY_V);
    p += cborencode_uint(p, ENCOUNTERS_VERSION);
    p += cborencode_uint(p, KEY_NODE);
    p += cborencode_byte_str(p, node, 32);
    p += cborencode_uint(p, KEY_TIME);
    p += cborencode_uint(p, captured_at_ms);
    p += cborencode_uint(p, KEY_BSSIDS);
    p += cborencode_array_header(p, scan->count);
    for (uint8_t i = 0; i < scan->count; i++) {
        p += cborencode_byte_str(p, scan->obs[i].bssid, 6);
    }
    p += cborencode_uint(p, KEY_SIG);
    p += cborencode_byte_str(p, sig, 64);
    return (size_t)(p - out);
}

bool encounters_should_record(const ssid_scan_t *scan,
                              uint32_t now_ms,
                              uint32_t min_age_ms) {
    if (!g_initialized || scan->count == 0) return false;
    if (used_count_get() == 0) return true;

    uint8_t last = newest_slot();
    uint16_t rec_len = slot_record_len(last);
    if (rec_len == 0 || rec_len > ENCOUNTERS_SLOT_BYTES - 2) return true;

    // Pull the previous BSSID set + captured_at_ms out of the record. Records
    // are CBOR-encoded; rather than parse, we walk the known structure. For
    // dedup we just need: last captured_at_ms, and the set of BSSIDs.
    //
    // Simpler: keep a small in-RAM mirror of the most recent scan's BSSIDs
    // and time, refreshed when we record. Avoids re-parsing CBOR here.

    // Rebuild the comparison from CBOR so the dedup logic survives reboot.
    // The signed body has a fixed prefix shape: map(5), 1, 1, 2, bstr(32),
    // 3, uint, 4, array(N), N * bstr(6), 5, bstr(64). We seek to KEY_TIME
    // and KEY_BSSIDS.
    const uint8_t *r = slot_record_ptr(last);
    // Skip map header (1 byte for ≤23 entries).
    size_t off = 1;
    // 1, 1
    off += 1 + 1;
    // 2, bstr(32) — header byte 0x58 0x20 OR 0x40+32. Our encoder uses len<=23
    // for the key (1 byte), and bstr(32) emits 0x58 0x20 (additional 24 + 1
    // length byte). Skip key + bstr header (2 bytes) + 32 payload.
    off += 1 + 2 + 32;
    // 3 (key), then captured_at uint.
    off += 1;
    uint32_t prev_time = 0;
    {
        uint8_t hdr = r[off++];
        uint8_t add = hdr & 0x1F;
        if (add < 24) {
            prev_time = add;
        } else if (add == 24) {
            prev_time = r[off++];
        } else if (add == 25) {
            prev_time = ((uint32_t)r[off] << 8) | r[off+1]; off += 2;
        } else if (add == 26) {
            prev_time = ((uint32_t)r[off] << 24) | ((uint32_t)r[off+1] << 16)
                      | ((uint32_t)r[off+2] << 8) | r[off+3];
            off += 4;
        } else {
            return true; // unexpected encoding — be safe and record
        }
    }

    if ((uint32_t)(now_ms - prev_time) >= min_age_ms) return true;

    // 4 (key), then array(N) of bstr(6).
    off += 1;
    uint8_t hdr = r[off++];
    uint8_t arr_count = hdr & 0x1F;
    if ((hdr >> 5) != 4 || arr_count > SSID_SCAN_MAX) return true;

    if (arr_count != scan->count) return true;
    for (uint8_t i = 0; i < arr_count; i++) {
        if (r[off] != 0x46) return true; // expected bstr(6) header byte
        off++;
        const uint8_t *bs = &r[off];
        off += 6;
        bool found = false;
        for (uint8_t j = 0; j < scan->count; j++) {
            if (memcmp(scan->obs[j].bssid, bs, 6) == 0) { found = true; break; }
        }
        if (!found) return true;
    }
    return false;
}

int encounters_record(const ssid_scan_t *scan,
                      uint32_t captured_at_ms,
                      const uint8_t node_pub[32],
                      const uint8_t seed[32]) {
    if (!g_initialized) return -1;
    if (scan->count == 0 || scan->count > SSID_SCAN_MAX) return -1;

    uint8_t signed_body[ENCOUNTERS_SLOT_BYTES];
    size_t  sb_len = encode_signed_body(signed_body, node_pub, captured_at_ms, scan);

    uint8_t sig[64];
    if (!hal_ed25519_sign(seed, signed_body, sb_len, sig)) return -1;

    uint8_t  ns      = next_slot_get();
    uint8_t *slot    = slot_ptr(ns);
    uint8_t *rec_buf = slot + 2;
    size_t   rec_max = ENCOUNTERS_SLOT_BYTES - 2;

    size_t rec_len = encode_full_record(rec_buf, rec_max, node_pub,
                                        captured_at_ms, scan, sig);
    if (rec_len == 0 || rec_len > rec_max) return -1;

    slot[0] = (uint8_t)(rec_len & 0xFF);
    slot[1] = (uint8_t)((rec_len >> 8) & 0xFF);
    if (rec_len < rec_max) memset(rec_buf + rec_len, 0, rec_max - rec_len);

    next_slot_set((uint8_t)((ns + 1) % ENCOUNTERS_CAP));
    uint8_t uc = used_count_get();
    if (uc < ENCOUNTERS_CAP) used_count_set(uc + 1);

    if (fs_system_write(BLOB_NAME, g_buf, sizeof(g_buf)) != 0) {
        printf("[encounters] fs_system_write failed\r\n");
        return -1;
    }
    return 0;
}

int encounters_count(void) {
    if (!g_initialized) return 0;
    return (int)used_count_get();
}

bool encounters_record_captured_at(const uint8_t *cbor, size_t len,
                                   uint32_t *captured_at_ms_out) {
    if (!cbor || len < 4 || !captured_at_ms_out) return false;
    // Walk the known-fixed prefix shape: map(5), key 1, val 1, key 2, bstr(32),
    // key 3, captured_at_ms. Encoder emits map(5)=0xa5, then 1, 1, 2, then
    // bstr(32) header (0x58 0x20), then 32 payload, then key 3, then the
    // captured_at_ms uint.
    size_t off = 0;
    if (cbor[off++] != 0xa5) return false;
    if (cbor[off++] != 0x01) return false;        // KEY_V
    if (cbor[off++] != 0x01) return false;        // value 1
    if (cbor[off++] != 0x02) return false;        // KEY_NODE
    if (cbor[off++] != 0x58) return false;        // bstr(additional=24)
    if (cbor[off++] != 0x20) return false;        // length 32
    off += 32;
    if (off >= len) return false;
    if (cbor[off++] != 0x03) return false;        // KEY_TIME
    if (off >= len) return false;
    uint8_t hdr = cbor[off++];
    uint8_t add = hdr & 0x1F;
    uint32_t v = 0;
    if (add < 24) {
        v = add;
    } else if (add == 24 && off + 1 <= len) {
        v = cbor[off++];
    } else if (add == 25 && off + 2 <= len) {
        v = ((uint32_t)cbor[off] << 8) | cbor[off+1]; off += 2;
    } else if (add == 26 && off + 4 <= len) {
        v = ((uint32_t)cbor[off] << 24) | ((uint32_t)cbor[off+1] << 16)
          | ((uint32_t)cbor[off+2] << 8) | cbor[off+3];
        off += 4;
    } else {
        return false;
    }
    *captured_at_ms_out = v;
    return true;
}

int encounters_for_each(void *ctx, encounters_iter_cb cb) {
    if (!g_initialized || !cb) return 0;
    uint8_t uc = used_count_get();
    if (uc == 0) return 0;

    uint8_t start = oldest_slot();
    int seen = 0;
    for (uint8_t k = 0; k < uc; k++) {
        uint8_t i = (uint8_t)((start + k) % ENCOUNTERS_CAP);
        uint16_t len = slot_record_len(i);
        if (len == 0 || len > ENCOUNTERS_SLOT_BYTES - 2) continue;
        int rc = cb(ctx, slot_record_ptr(i), len);
        seen++;
        if (rc != 0) break;
    }
    return seen;
}
