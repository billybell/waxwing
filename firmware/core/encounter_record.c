#include "core/encounter_record.h"

#include <string.h>

#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/hal_crypto.h"
#include "core/thirdparty/sha256/sha256.h"

// CBOR map keys. Ordered numerically; the encoder emits them in this
// exact order to keep the canonical body byte-for-byte reproducible
// across both sides of the handshake.
#define KEY_V                       1
#define KEY_PUB_A                   2
#define KEY_PUB_B                   3
#define KEY_MEETING_COUNT_A         4
#define KEY_MEETING_COUNT_B         5
#define KEY_REP_OF_B_BY_A           6
#define KEY_REP_OF_A_BY_B           7
#define KEY_BSSIDS_A                8
#define KEY_BSSIDS_B                9
#define KEY_NONCE_A                 10
#define KEY_NONCE_B                 11
#define KEY_TX_AB_LIFETIME          12
#define KEY_RX_AB_LIFETIME          13
#define KEY_TX_BA_LIFETIME          14
#define KEY_RX_BA_LIFETIME          15
#define KEY_FILE_AB_LIFETIME        16
#define KEY_FILE_BA_LIFETIME        17
#define KEY_SIG_A                   18
#define KEY_SIG_B                   19

#define BODY_KEY_COUNT              17
#define FULL_KEY_COUNT              19

// ---------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------

// Each header is at most 5 bytes; uint64 is at most 9 bytes; bstr(N) is
// 1..3 bytes of header + N. The largest single field is bssids_a (8 *
// (1+6) + 1 array header = 57 bytes). We bound the body at
// ENCOUNTER_BODY_MAX_BYTES (384) and the full record at
// ENCOUNTER_RECORD_MAX_BYTES (512), both with margin.

// Encode the bssids array field: array(N), N copies of bstr(6).
static size_t encode_bssids(uint8_t *p, const uint8_t bssids[][6], uint8_t n) {
    uint8_t *start = p;
    p += cborencode_array_header(p, n);
    for (uint8_t i = 0; i < n; i++) {
        p += cborencode_byte_str(p, bssids[i], 6);
    }
    return (size_t)(p - start);
}

// Common path for body and full encoders. `with_sigs` adds keys 18 and
// 19 and bumps the map header from 17 to 19 entries.
static size_t encode_record(const encounter_record_t *rec,
                            uint8_t *out, size_t out_max,
                            bool with_sigs) {
    if (!rec || !out) return 0;
    if (rec->version != ENCOUNTER_RECORD_VERSION) return 0;
    if (rec->bssids_a_count > ENCOUNTER_BSSID_MAX ||
        rec->bssids_b_count > ENCOUNTER_BSSID_MAX) return 0;

    const size_t cap = with_sigs ? ENCOUNTER_RECORD_MAX_BYTES
                                 : ENCOUNTER_BODY_MAX_BYTES;
    if (out_max < cap) return 0;

    uint8_t *p = out;

    p += cborencode_map_header(p, with_sigs ? FULL_KEY_COUNT : BODY_KEY_COUNT);

    p += cborencode_uint(p, KEY_V);
    p += cborencode_uint(p, rec->version);

    p += cborencode_uint(p, KEY_PUB_A);
    p += cborencode_byte_str(p, rec->pub_a, ENCOUNTER_PUB_BYTES);

    p += cborencode_uint(p, KEY_PUB_B);
    p += cborencode_byte_str(p, rec->pub_b, ENCOUNTER_PUB_BYTES);

    p += cborencode_uint(p, KEY_MEETING_COUNT_A);
    p += cborencode_uint64(p, rec->meeting_count_a);

    p += cborencode_uint(p, KEY_MEETING_COUNT_B);
    p += cborencode_uint64(p, rec->meeting_count_b);

    p += cborencode_uint(p, KEY_REP_OF_B_BY_A);
    p += cborencode_int(p, rec->rep_of_b_by_a);

    p += cborencode_uint(p, KEY_REP_OF_A_BY_B);
    p += cborencode_int(p, rec->rep_of_a_by_b);

    p += cborencode_uint(p, KEY_BSSIDS_A);
    p += encode_bssids(p, rec->bssids_a, rec->bssids_a_count);

    p += cborencode_uint(p, KEY_BSSIDS_B);
    p += encode_bssids(p, rec->bssids_b, rec->bssids_b_count);

    p += cborencode_uint(p, KEY_NONCE_A);
    p += cborencode_byte_str(p, rec->nonce_a, ENCOUNTER_NONCE_BYTES);

    p += cborencode_uint(p, KEY_NONCE_B);
    p += cborencode_byte_str(p, rec->nonce_b, ENCOUNTER_NONCE_BYTES);

    p += cborencode_uint(p, KEY_TX_AB_LIFETIME);
    p += cborencode_uint64(p, rec->tx_bytes_a_to_b_lifetime);

    p += cborencode_uint(p, KEY_RX_AB_LIFETIME);
    p += cborencode_uint64(p, rec->rx_bytes_a_from_b_lifetime);

    p += cborencode_uint(p, KEY_TX_BA_LIFETIME);
    p += cborencode_uint64(p, rec->tx_bytes_b_to_a_lifetime);

    p += cborencode_uint(p, KEY_RX_BA_LIFETIME);
    p += cborencode_uint64(p, rec->rx_bytes_b_from_a_lifetime);

    p += cborencode_uint(p, KEY_FILE_AB_LIFETIME);
    p += cborencode_uint64(p, rec->file_count_a_from_b_lifetime);

    p += cborencode_uint(p, KEY_FILE_BA_LIFETIME);
    p += cborencode_uint64(p, rec->file_count_b_from_a_lifetime);

    if (with_sigs) {
        p += cborencode_uint(p, KEY_SIG_A);
        p += cborencode_byte_str(p, rec->sig_a, ENCOUNTER_SIG_BYTES);

        p += cborencode_uint(p, KEY_SIG_B);
        p += cborencode_byte_str(p, rec->sig_b, ENCOUNTER_SIG_BYTES);
    }

    return (size_t)(p - out);
}

size_t encounter_record_encode_body(const encounter_record_t *rec,
                                    uint8_t *out, size_t out_max) {
    return encode_record(rec, out, out_max, false);
}

size_t encounter_record_encode_full(const encounter_record_t *rec,
                                    uint8_t *out, size_t out_max) {
    return encode_record(rec, out, out_max, true);
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

// CBOR_TYPE_INT carries the negated-encoded form: arg = -1 - actual.
// Convert back to int32, clamping if out of range.
static int32_t cbor_to_int32(const cbor_item_t *v) {
    if (v->type == CBOR_TYPE_UINT) {
        if (v->arg > (uint64_t)INT32_MAX) return INT32_MAX;
        return (int32_t)v->arg;
    }
    // CBOR_TYPE_INT
    uint64_t n = v->arg + 1;          // |actual|
    if (n > (uint64_t)INT32_MAX + 1ULL) return INT32_MIN;
    return -(int32_t)n;
}

static bool decode_bssids(const cbor_item_t *arr, const uint8_t *end,
                          uint8_t out[][6], uint8_t *out_count) {
    if (arr->type != CBOR_TYPE_ARRAY) return false;
    if (arr->arg > ENCOUNTER_BSSID_MAX) return false;
    const uint8_t *p = arr->data;
    for (uint64_t i = 0; i < arr->arg; i++) {
        cbor_item_t item;
        if (!cbor_parse(p, end, &item)) return false;
        if (item.type != CBOR_TYPE_BSTR || item.arg != 6) return false;
        memcpy(out[i], item.data, 6);
        p = item.next;
    }
    *out_count = (uint8_t)arr->arg;
    return true;
}

static bool decode_bstr_fixed(const cbor_item_t *v, uint8_t *out, size_t expected) {
    if (v->type != CBOR_TYPE_BSTR) return false;
    if (v->arg != expected) return false;
    memcpy(out, v->data, expected);
    return true;
}

bool encounter_record_decode(const uint8_t *cbor, size_t len,
                             encounter_record_t *out) {
    if (!cbor || !out) return false;
    const uint8_t *end = cbor + len;

    cbor_item_t map;
    if (!cbor_parse(cbor, end, &map)) return false;
    if (map.type != CBOR_TYPE_MAP) return false;

    memset(out, 0, sizeof(*out));

    // Track which required keys we've seen. Keys 1..19 fit in a uint32
    // bitmap (we use bits 1..19).
    uint32_t seen = 0;
    const uint32_t REQUIRED =
        (1u << KEY_V) |
        (1u << KEY_PUB_A) | (1u << KEY_PUB_B) |
        (1u << KEY_MEETING_COUNT_A) | (1u << KEY_MEETING_COUNT_B) |
        (1u << KEY_REP_OF_B_BY_A)   | (1u << KEY_REP_OF_A_BY_B) |
        (1u << KEY_BSSIDS_A)        | (1u << KEY_BSSIDS_B) |
        (1u << KEY_NONCE_A)         | (1u << KEY_NONCE_B) |
        (1u << KEY_TX_AB_LIFETIME)  | (1u << KEY_RX_AB_LIFETIME) |
        (1u << KEY_TX_BA_LIFETIME)  | (1u << KEY_RX_BA_LIFETIME) |
        (1u << KEY_FILE_AB_LIFETIME)| (1u << KEY_FILE_BA_LIFETIME) |
        (1u << KEY_SIG_A)           | (1u << KEY_SIG_B);

    const uint8_t *p = map.data;
    for (uint64_t i = 0; i < map.arg; i++) {
        cbor_item_t k, v;
        if (!cbor_parse(p, end, &k)) return false;
        if (!cbor_parse(k.next, end, &v)) return false;
        p = v.next;

        if (k.type != CBOR_TYPE_UINT) continue;     // ignore non-int keys
        uint64_t key = k.arg;
        if (key == 0 || key > 31) continue;          // unknown key, skip

        switch (key) {
        case KEY_V:
            if (v.type != CBOR_TYPE_UINT) return false;
            if (v.arg > UINT8_MAX) return false;
            out->version = (uint8_t)v.arg;
            break;
        case KEY_PUB_A:
            if (!decode_bstr_fixed(&v, out->pub_a, ENCOUNTER_PUB_BYTES)) return false;
            break;
        case KEY_PUB_B:
            if (!decode_bstr_fixed(&v, out->pub_b, ENCOUNTER_PUB_BYTES)) return false;
            break;
        case KEY_MEETING_COUNT_A:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->meeting_count_a = v.arg;
            break;
        case KEY_MEETING_COUNT_B:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->meeting_count_b = v.arg;
            break;
        case KEY_REP_OF_B_BY_A:
            if (v.type != CBOR_TYPE_UINT && v.type != CBOR_TYPE_INT) return false;
            out->rep_of_b_by_a = cbor_to_int32(&v);
            break;
        case KEY_REP_OF_A_BY_B:
            if (v.type != CBOR_TYPE_UINT && v.type != CBOR_TYPE_INT) return false;
            out->rep_of_a_by_b = cbor_to_int32(&v);
            break;
        case KEY_BSSIDS_A:
            if (!decode_bssids(&v, end, out->bssids_a, &out->bssids_a_count)) return false;
            break;
        case KEY_BSSIDS_B:
            if (!decode_bssids(&v, end, out->bssids_b, &out->bssids_b_count)) return false;
            break;
        case KEY_NONCE_A:
            if (!decode_bstr_fixed(&v, out->nonce_a, ENCOUNTER_NONCE_BYTES)) return false;
            break;
        case KEY_NONCE_B:
            if (!decode_bstr_fixed(&v, out->nonce_b, ENCOUNTER_NONCE_BYTES)) return false;
            break;
        case KEY_TX_AB_LIFETIME:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->tx_bytes_a_to_b_lifetime = v.arg;
            break;
        case KEY_RX_AB_LIFETIME:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->rx_bytes_a_from_b_lifetime = v.arg;
            break;
        case KEY_TX_BA_LIFETIME:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->tx_bytes_b_to_a_lifetime = v.arg;
            break;
        case KEY_RX_BA_LIFETIME:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->rx_bytes_b_from_a_lifetime = v.arg;
            break;
        case KEY_FILE_AB_LIFETIME:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->file_count_a_from_b_lifetime = v.arg;
            break;
        case KEY_FILE_BA_LIFETIME:
            if (v.type != CBOR_TYPE_UINT) return false;
            out->file_count_b_from_a_lifetime = v.arg;
            break;
        case KEY_SIG_A:
            if (!decode_bstr_fixed(&v, out->sig_a, ENCOUNTER_SIG_BYTES)) return false;
            break;
        case KEY_SIG_B:
            if (!decode_bstr_fixed(&v, out->sig_b, ENCOUNTER_SIG_BYTES)) return false;
            break;
        default:
            // Unknown key in our reserved range — tolerate.
            break;
        }
        seen |= (1u << key);
    }

    if ((seen & REQUIRED) != REQUIRED) return false;
    if (out->version != ENCOUNTER_RECORD_VERSION) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Verify
// ---------------------------------------------------------------------------

bool encounter_record_verify(const encounter_record_t *rec) {
    if (!rec || rec->version != ENCOUNTER_RECORD_VERSION) return false;

    uint8_t body[ENCOUNTER_BODY_MAX_BYTES];
    size_t  body_len = encounter_record_encode_body(rec, body, sizeof(body));
    if (body_len == 0) return false;

    if (!hal_ed25519_verify(rec->pub_a, body, body_len, rec->sig_a)) return false;
    if (!hal_ed25519_verify(rec->pub_b, body, body_len, rec->sig_b)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Encounter id
// ---------------------------------------------------------------------------

void encounter_record_id(const encounter_record_t *rec,
                         uint8_t out[ENCOUNTER_ID_BYTES]) {
    // Side-symmetric ordering: lex-min(pub) first.
    const uint8_t *lo = rec->pub_a;
    const uint8_t *hi = rec->pub_b;
    if (memcmp(rec->pub_a, rec->pub_b, ENCOUNTER_PUB_BYTES) > 0) {
        lo = rec->pub_b;
        hi = rec->pub_a;
    }

    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, lo, ENCOUNTER_PUB_BYTES);
    sha256_update(&ctx, hi, ENCOUNTER_PUB_BYTES);
    sha256_update(&ctx, rec->nonce_a, ENCOUNTER_NONCE_BYTES);
    sha256_update(&ctx, rec->nonce_b, ENCOUNTER_NONCE_BYTES);

    uint8_t digest[32];
    sha256_final(&ctx, digest);
    memcpy(out, digest, ENCOUNTER_ID_BYTES);
}
