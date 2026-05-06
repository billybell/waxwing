#include "core/encounter_session.h"

#include <string.h>

#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/hal_crypto.h"

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------
//
// Each message is a CBOR map with integer keys. Key 1 is always the
// message type; the rest carry the half-record fields (PROPOSE, ACCEPT)
// or the signature only (CONFIRM).

// Message type tags (key 1).
#define MSG_PROPOSE  1
#define MSG_ACCEPT   2
#define MSG_CONFIRM  3

// Per-message keys.
#define MK_TYPE              1
#define MK_V                 2
#define MK_PUB               3
#define MK_MEETING_COUNT     4
#define MK_REP_OF_PEER       5
#define MK_BSSIDS            6
#define MK_NONCE             7
#define MK_TX_LIFETIME       8
#define MK_RX_LIFETIME       9
#define MK_FILE_LIFETIME    10
#define MK_SIG              11

// Internal session states.
enum {
    ST_INIT,
    ST_I_AWAIT_ACCEPT,
    ST_R_AWAIT_CONFIRM,
    ST_DONE,
    ST_ERROR,
};

// ---------------------------------------------------------------------------
// CBOR helpers (integer-keyed map walk)
// ---------------------------------------------------------------------------

// Find the value associated with integer key `k` in `map`. Returns true
// on hit and copies the value item into `*out`.
static bool find_int_key(const cbor_item_t *map, const uint8_t *end,
                         uint64_t k, cbor_item_t *out) {
    if (map->type != CBOR_TYPE_MAP) return false;
    const uint8_t *p = map->data;
    for (uint64_t i = 0; i < map->arg; i++) {
        cbor_item_t key, val;
        if (!cbor_parse(p, end, &key)) return false;
        if (!cbor_parse(key.next, end, &val)) return false;
        if (key.type == CBOR_TYPE_UINT && key.arg == k) {
            *out = val;
            return true;
        }
        p = val.next;
    }
    return false;
}

static bool decode_bssid_array(const cbor_item_t *arr, const uint8_t *end,
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

static int32_t cbor_to_int32(const cbor_item_t *v) {
    if (v->type == CBOR_TYPE_UINT) {
        return (v->arg > (uint64_t)INT32_MAX) ? INT32_MAX : (int32_t)v->arg;
    }
    uint64_t n = v->arg + 1;
    return (n > (uint64_t)INT32_MAX + 1ULL) ? INT32_MIN : -(int32_t)n;
}

static size_t encode_bssid_array(uint8_t *p, const uint8_t bssids[][6], uint8_t n) {
    uint8_t *start = p;
    p += cborencode_array_header(p, n);
    for (uint8_t i = 0; i < n; i++) {
        p += cborencode_byte_str(p, bssids[i], 6);
    }
    return (size_t)(p - start);
}

static bool prefix_matches_or_zero(const uint8_t prefix[8], const uint8_t pub[32]) {
    bool all_zero = true;
    for (int i = 0; i < 8; i++) {
        if (prefix[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return true;
    return memcmp(prefix, pub, 8) == 0;
}

// ---------------------------------------------------------------------------
// PROPOSE / ACCEPT share a 10-key half-message body. The local node fills
// either the A side or the B side of the record from `me`; the peer's
// half is decoded into the opposite side. `is_b_side` flips which set
// of fields we read/write.
// ---------------------------------------------------------------------------

// Fill the local side of `rec` from the input. `is_b_side` writes into
// pub_b/meeting_count_b/etc. when true, A side when false.
static void fill_local_half(encounter_record_t *rec,
                            const encounter_local_input_t *me,
                            bool is_b_side) {
    if (is_b_side) {
        memcpy(rec->pub_b, me->pub, ENCOUNTER_PUB_BYTES);
        rec->meeting_count_b              = me->meeting_count;
        rec->rep_of_a_by_b                = me->rep_of_peer;
        memcpy(rec->bssids_b, me->bssids, sizeof(rec->bssids_b));
        rec->bssids_b_count               = me->bssids_count;
        memcpy(rec->nonce_b, me->nonce, ENCOUNTER_NONCE_BYTES);
        rec->tx_bytes_b_to_a_lifetime     = me->tx_bytes_to_peer_lifetime;
        rec->rx_bytes_b_from_a_lifetime   = me->rx_bytes_from_peer_lifetime;
        rec->file_count_b_from_a_lifetime = me->file_count_from_peer_lifetime;
    } else {
        memcpy(rec->pub_a, me->pub, ENCOUNTER_PUB_BYTES);
        rec->meeting_count_a              = me->meeting_count;
        rec->rep_of_b_by_a                = me->rep_of_peer;
        memcpy(rec->bssids_a, me->bssids, sizeof(rec->bssids_a));
        rec->bssids_a_count               = me->bssids_count;
        memcpy(rec->nonce_a, me->nonce, ENCOUNTER_NONCE_BYTES);
        rec->tx_bytes_a_to_b_lifetime     = me->tx_bytes_to_peer_lifetime;
        rec->rx_bytes_a_from_b_lifetime   = me->rx_bytes_from_peer_lifetime;
        rec->file_count_a_from_b_lifetime = me->file_count_from_peer_lifetime;
    }
}

// Encode a PROPOSE or ACCEPT (with optional sig field).
static size_t encode_half_message(uint8_t *out, size_t out_max,
                                  uint8_t msg_type,
                                  uint8_t version,
                                  const uint8_t pub[32],
                                  uint64_t meeting_count,
                                  int32_t  rep_of_peer,
                                  const uint8_t bssids[][6], uint8_t bssids_count,
                                  const uint8_t nonce[16],
                                  uint64_t tx_lifetime,
                                  uint64_t rx_lifetime,
                                  uint64_t file_lifetime,
                                  const uint8_t *sig /* NULL for PROPOSE */) {
    if (out_max < ENCOUNTER_MSG_MAX_BYTES) return 0;
    if (bssids_count > ENCOUNTER_BSSID_MAX) return 0;

    const uint32_t key_count = sig ? 11 : 10;
    uint8_t *p = out;

    p += cborencode_map_header(p, key_count);

    p += cborencode_uint(p, MK_TYPE);
    p += cborencode_uint(p, msg_type);

    p += cborencode_uint(p, MK_V);
    p += cborencode_uint(p, version);

    p += cborencode_uint(p, MK_PUB);
    p += cborencode_byte_str(p, pub, ENCOUNTER_PUB_BYTES);

    p += cborencode_uint(p, MK_MEETING_COUNT);
    p += cborencode_uint64(p, meeting_count);

    p += cborencode_uint(p, MK_REP_OF_PEER);
    p += cborencode_int(p, rep_of_peer);

    p += cborencode_uint(p, MK_BSSIDS);
    p += encode_bssid_array(p, bssids, bssids_count);

    p += cborencode_uint(p, MK_NONCE);
    p += cborencode_byte_str(p, nonce, ENCOUNTER_NONCE_BYTES);

    p += cborencode_uint(p, MK_TX_LIFETIME);
    p += cborencode_uint64(p, tx_lifetime);

    p += cborencode_uint(p, MK_RX_LIFETIME);
    p += cborencode_uint64(p, rx_lifetime);

    p += cborencode_uint(p, MK_FILE_LIFETIME);
    p += cborencode_uint64(p, file_lifetime);

    if (sig) {
        p += cborencode_uint(p, MK_SIG);
        p += cborencode_byte_str(p, sig, ENCOUNTER_SIG_BYTES);
    }

    return (size_t)(p - out);
}

// Decode a PROPOSE/ACCEPT half-message into the matching side of `rec`.
// `expected_type` lets us reject ACCEPT-shaped messages in PROPOSE phase
// and vice versa. `is_b_side` tells us which side to populate (peer's).
// On ACCEPT, also writes sig into `rec->sig_b` (since responder is B).
static bool decode_half_message(const uint8_t *in, size_t in_len,
                                uint8_t expected_type, bool is_b_side,
                                encounter_record_t *rec) {
    const uint8_t *end = in + in_len;
    cbor_item_t map;
    if (!cbor_parse(in, end, &map)) return false;

    cbor_item_t v;
    if (!find_int_key(&map, end, MK_TYPE, &v) ||
        v.type != CBOR_TYPE_UINT || v.arg != expected_type) return false;

    if (!find_int_key(&map, end, MK_V, &v) ||
        v.type != CBOR_TYPE_UINT || v.arg != ENCOUNTER_RECORD_VERSION) return false;
    rec->version = (uint8_t)v.arg;

    if (!find_int_key(&map, end, MK_PUB, &v) ||
        v.type != CBOR_TYPE_BSTR || v.arg != ENCOUNTER_PUB_BYTES) return false;
    uint8_t *pub_dst = is_b_side ? rec->pub_b : rec->pub_a;
    memcpy(pub_dst, v.data, ENCOUNTER_PUB_BYTES);

    if (!find_int_key(&map, end, MK_MEETING_COUNT, &v) ||
        v.type != CBOR_TYPE_UINT) return false;
    if (is_b_side) rec->meeting_count_b = v.arg;
    else           rec->meeting_count_a = v.arg;

    if (!find_int_key(&map, end, MK_REP_OF_PEER, &v)) return false;
    if (v.type != CBOR_TYPE_UINT && v.type != CBOR_TYPE_INT) return false;
    int32_t rep = cbor_to_int32(&v);
    // PROPOSE carries A's rep-of-B; ACCEPT carries B's rep-of-A.
    if (is_b_side) rec->rep_of_a_by_b = rep;
    else           rec->rep_of_b_by_a = rep;

    if (!find_int_key(&map, end, MK_BSSIDS, &v)) return false;
    if (is_b_side) {
        if (!decode_bssid_array(&v, end, rec->bssids_b, &rec->bssids_b_count)) return false;
    } else {
        if (!decode_bssid_array(&v, end, rec->bssids_a, &rec->bssids_a_count)) return false;
    }

    if (!find_int_key(&map, end, MK_NONCE, &v) ||
        v.type != CBOR_TYPE_BSTR || v.arg != ENCOUNTER_NONCE_BYTES) return false;
    uint8_t *nonce_dst = is_b_side ? rec->nonce_b : rec->nonce_a;
    memcpy(nonce_dst, v.data, ENCOUNTER_NONCE_BYTES);

    if (!find_int_key(&map, end, MK_TX_LIFETIME, &v) ||
        v.type != CBOR_TYPE_UINT) return false;
    if (is_b_side) rec->tx_bytes_b_to_a_lifetime = v.arg;
    else           rec->tx_bytes_a_to_b_lifetime = v.arg;

    if (!find_int_key(&map, end, MK_RX_LIFETIME, &v) ||
        v.type != CBOR_TYPE_UINT) return false;
    if (is_b_side) rec->rx_bytes_b_from_a_lifetime = v.arg;
    else           rec->rx_bytes_a_from_b_lifetime = v.arg;

    if (!find_int_key(&map, end, MK_FILE_LIFETIME, &v) ||
        v.type != CBOR_TYPE_UINT) return false;
    if (is_b_side) rec->file_count_b_from_a_lifetime = v.arg;
    else           rec->file_count_a_from_b_lifetime = v.arg;

    if (expected_type == MSG_ACCEPT) {
        if (!find_int_key(&map, end, MK_SIG, &v) ||
            v.type != CBOR_TYPE_BSTR || v.arg != ENCOUNTER_SIG_BYTES) return false;
        // Responder is always B in our schema, so sig from ACCEPT is
        // sig_b regardless of whether is_b_side is set on the local
        // side (the caller is the initiator parsing its peer's accept).
        memcpy(rec->sig_b, v.data, ENCOUNTER_SIG_BYTES);
    }

    return true;
}

static size_t encode_propose(const encounter_local_input_t *me,
                             uint8_t *out, size_t out_max) {
    return encode_half_message(out, out_max,
                               MSG_PROPOSE,
                               ENCOUNTER_RECORD_VERSION,
                               me->pub,
                               me->meeting_count,
                               me->rep_of_peer,
                               me->bssids, me->bssids_count,
                               me->nonce,
                               me->tx_bytes_to_peer_lifetime,
                               me->rx_bytes_from_peer_lifetime,
                               me->file_count_from_peer_lifetime,
                               NULL);
}

static size_t encode_accept(const encounter_record_t *rec,
                            uint8_t *out, size_t out_max) {
    return encode_half_message(out, out_max,
                               MSG_ACCEPT,
                               rec->version,
                               rec->pub_b,
                               rec->meeting_count_b,
                               rec->rep_of_a_by_b,
                               rec->bssids_b, rec->bssids_b_count,
                               rec->nonce_b,
                               rec->tx_bytes_b_to_a_lifetime,
                               rec->rx_bytes_b_from_a_lifetime,
                               rec->file_count_b_from_a_lifetime,
                               rec->sig_b);
}

static size_t encode_confirm(const uint8_t sig_a[64],
                             uint8_t *out, size_t out_max) {
    if (out_max < ENCOUNTER_MSG_MAX_BYTES) return 0;
    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_uint(p, MK_TYPE);
    p += cborencode_uint(p, MSG_CONFIRM);
    p += cborencode_uint(p, MK_SIG);
    p += cborencode_byte_str(p, sig_a, ENCOUNTER_SIG_BYTES);
    return (size_t)(p - out);
}

static bool decode_confirm(const uint8_t *in, size_t in_len,
                           uint8_t sig_out[64]) {
    const uint8_t *end = in + in_len;
    cbor_item_t map;
    if (!cbor_parse(in, end, &map)) return false;

    cbor_item_t v;
    if (!find_int_key(&map, end, MK_TYPE, &v) ||
        v.type != CBOR_TYPE_UINT || v.arg != MSG_CONFIRM) return false;
    if (!find_int_key(&map, end, MK_SIG, &v) ||
        v.type != CBOR_TYPE_BSTR || v.arg != ENCOUNTER_SIG_BYTES) return false;
    memcpy(sig_out, v.data, ENCOUNTER_SIG_BYTES);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static bool inputs_valid(const encounter_local_input_t *me) {
    if (!me) return false;
    if (me->bssids_count > ENCOUNTER_BSSID_MAX) return false;
    return true;
}

encounter_step_t encounter_session_start(encounter_session_t *s,
                                         encounter_role_t role,
                                         const encounter_local_input_t *me,
                                         uint8_t *out_buf, size_t out_max,
                                         size_t *out_len) {
    if (!s || !out_buf || !out_len || !inputs_valid(me) ||
        out_max < ENCOUNTER_MSG_MAX_BYTES) {
        if (s) s->state = ST_ERROR;
        if (out_len) *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }

    memset(s, 0, sizeof(*s));
    s->role = role;
    s->me   = *me;
    s->rec.version = ENCOUNTER_RECORD_VERSION;

    if (role == ENCOUNTER_ROLE_INITIATOR) {
        // Pre-populate the A side of the record from `me`. The B side
        // will be filled when ACCEPT arrives.
        fill_local_half(&s->rec, &s->me, /*is_b_side=*/false);
        size_t n = encode_propose(&s->me, out_buf, out_max);
        if (n == 0) {
            s->state = ST_ERROR;
            *out_len = 0;
            return ENCOUNTER_STEP_ERROR;
        }
        *out_len = n;
        s->state = ST_I_AWAIT_ACCEPT;
        return ENCOUNTER_STEP_NEED_WRITE;
    }

    // Responder waits for PROPOSE. The B side of the record is filled
    // in handle_responder_propose() so the late_bind callback (invoked
    // there with the now-known peer pub) can refresh the per-peer
    // lifetime fields before they're baked into the signed body.
    *out_len = 0;
    s->state = ST_INIT;
    return ENCOUNTER_STEP_NEED_READ;
}

// Initiator received ACCEPT. Verify sig_b, sign sig_a, encode CONFIRM.
static encounter_step_t handle_initiator_accept(encounter_session_t *s,
                                                const uint8_t *in, size_t in_len,
                                                uint8_t *out_buf, size_t out_max,
                                                size_t *out_len) {
    if (!decode_half_message(in, in_len, MSG_ACCEPT, /*is_b_side=*/true, &s->rec)) {
        s->state = ST_ERROR;
        *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }
    if (!prefix_matches_or_zero(s->me.expected_peer_prefix, s->rec.pub_b)) {
        s->state = ST_ERROR;
        *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }

    uint8_t body[ENCOUNTER_BODY_MAX_BYTES];
    size_t  body_len = encounter_record_encode_body(&s->rec, body, sizeof(body));
    if (body_len == 0) { s->state = ST_ERROR; *out_len = 0; return ENCOUNTER_STEP_ERROR; }

    if (!hal_ed25519_verify(s->rec.pub_b, body, body_len, s->rec.sig_b)) {
        s->state = ST_ERROR;
        *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }
    if (!hal_ed25519_sign(s->me.seed, body, body_len, s->rec.sig_a)) {
        s->state = ST_ERROR;
        *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }

    size_t n = encode_confirm(s->rec.sig_a, out_buf, out_max);
    if (n == 0) { s->state = ST_ERROR; *out_len = 0; return ENCOUNTER_STEP_ERROR; }
    *out_len = n;
    s->state = ST_DONE;
    return ENCOUNTER_STEP_NEED_WRITE;
}

// Responder received PROPOSE. Decode A side, sign sig_b, encode ACCEPT.
static encounter_step_t handle_responder_propose(encounter_session_t *s,
                                                 const uint8_t *in, size_t in_len,
                                                 uint8_t *out_buf, size_t out_max,
                                                 size_t *out_len) {
    if (!decode_half_message(in, in_len, MSG_PROPOSE, /*is_b_side=*/false, &s->rec)) {
        s->state = ST_ERROR;
        *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }
    if (!prefix_matches_or_zero(s->me.expected_peer_prefix, s->rec.pub_a)) {
        s->state = ST_ERROR;
        *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }

    // Now that we know pub_a, give the caller a chance to look up
    // per-peer state (peer_ledger entry, reputation) and refresh the
    // four pub-dependent fields. Failure or absence of the callback
    // leaves the zero values from `me` intact (first-contact).
    if (s->me.late_bind) {
        encounter_per_peer_input_t pp;
        memset(&pp, 0, sizeof(pp));
        if (s->me.late_bind(s->rec.pub_a, &pp, s->me.late_bind_ctx)) {
            s->me.rep_of_peer                    = pp.rep_of_peer;
            s->me.tx_bytes_to_peer_lifetime      = pp.tx_bytes_to_peer_lifetime;
            s->me.rx_bytes_from_peer_lifetime    = pp.rx_bytes_from_peer_lifetime;
            s->me.file_count_from_peer_lifetime  = pp.file_count_from_peer_lifetime;
        }
    }

    // Pre-populate the B side now (deferred from session_start) so the
    // body we sign reflects any late-bound updates above.
    fill_local_half(&s->rec, &s->me, /*is_b_side=*/true);

    uint8_t body[ENCOUNTER_BODY_MAX_BYTES];
    size_t  body_len = encounter_record_encode_body(&s->rec, body, sizeof(body));
    if (body_len == 0) { s->state = ST_ERROR; *out_len = 0; return ENCOUNTER_STEP_ERROR; }

    if (!hal_ed25519_sign(s->me.seed, body, body_len, s->rec.sig_b)) {
        s->state = ST_ERROR;
        *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }

    size_t n = encode_accept(&s->rec, out_buf, out_max);
    if (n == 0) { s->state = ST_ERROR; *out_len = 0; return ENCOUNTER_STEP_ERROR; }
    *out_len = n;
    s->state = ST_R_AWAIT_CONFIRM;
    return ENCOUNTER_STEP_NEED_WRITE;
}

// Responder received CONFIRM. Verify sig_a against the body it already
// has fully formed, then DONE.
static encounter_step_t handle_responder_confirm(encounter_session_t *s,
                                                 const uint8_t *in, size_t in_len,
                                                 size_t *out_len) {
    *out_len = 0;
    if (!decode_confirm(in, in_len, s->rec.sig_a)) {
        s->state = ST_ERROR;
        return ENCOUNTER_STEP_ERROR;
    }

    uint8_t body[ENCOUNTER_BODY_MAX_BYTES];
    size_t  body_len = encounter_record_encode_body(&s->rec, body, sizeof(body));
    if (body_len == 0) { s->state = ST_ERROR; return ENCOUNTER_STEP_ERROR; }

    if (!hal_ed25519_verify(s->rec.pub_a, body, body_len, s->rec.sig_a)) {
        s->state = ST_ERROR;
        return ENCOUNTER_STEP_ERROR;
    }
    s->state = ST_DONE;
    return ENCOUNTER_STEP_DONE;
}

encounter_step_t encounter_session_handle(encounter_session_t *s,
                                          const uint8_t *in_buf, size_t in_len,
                                          uint8_t *out_buf, size_t out_max,
                                          size_t *out_len) {
    if (!s || !in_buf || !out_len || out_max < ENCOUNTER_MSG_MAX_BYTES) {
        if (s) s->state = ST_ERROR;
        if (out_len) *out_len = 0;
        return ENCOUNTER_STEP_ERROR;
    }
    *out_len = 0;

    if (s->role == ENCOUNTER_ROLE_INITIATOR) {
        if (s->state == ST_I_AWAIT_ACCEPT) {
            return handle_initiator_accept(s, in_buf, in_len,
                                           out_buf, out_max, out_len);
        }
    } else {
        if (s->state == ST_INIT) {
            return handle_responder_propose(s, in_buf, in_len,
                                            out_buf, out_max, out_len);
        }
        if (s->state == ST_R_AWAIT_CONFIRM) {
            return handle_responder_confirm(s, in_buf, in_len, out_len);
        }
    }
    s->state = ST_ERROR;
    return ENCOUNTER_STEP_ERROR;
}

bool encounter_session_take_record(const encounter_session_t *s,
                                   encounter_record_t *out) {
    if (!s || !out || s->state != ST_DONE) return false;
    *out = s->rec;
    return true;
}
