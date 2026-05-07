#ifndef WAXWING_ENCOUNTER_SESSION_H
#define WAXWING_ENCOUNTER_SESSION_H

#include "core/encounter_record.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Three-message encounter handshake (M4 stage 2).
//
//   A → B   ENCOUNTER_PROPOSE   { v, pub_a, meeting_count_a,
//                                 rep_of_b_by_a, bssids_a, nonce_a,
//                                 tx_ab_lt, rx_ab_lt, file_ab_lt }
//   B → A   ENCOUNTER_ACCEPT    { v, pub_b, meeting_count_b,
//                                 rep_of_a_by_b, bssids_b, nonce_b,
//                                 tx_ba_lt, rx_ba_lt, file_ba_lt, sig_b }
//   A → B   ENCOUNTER_CONFIRM   { sig_a }
//
// After CONFIRM, both sides hold an identical encounter_record_t with
// both signatures. The session is the *price of admission* for any
// peer-sync work — the BLE driver will reject all other commands until
// this state machine reports DONE.
//
// This module is just the protocol state machine. Storage, replay
// detection (nonce-A reuse), and integration with the peer characteristic
// belong to follow-up stages.
// ---------------------------------------------------------------------------

typedef enum {
    ENCOUNTER_ROLE_INITIATOR = 0,
    ENCOUNTER_ROLE_RESPONDER = 1,
} encounter_role_t;

typedef enum {
    ENCOUNTER_STEP_NEED_WRITE = 0,   // out_buf has bytes to send to the peer
    ENCOUNTER_STEP_NEED_READ  = 1,   // wait for the peer's next message
    ENCOUNTER_STEP_DONE       = 2,   // record is complete; call take_record()
    ENCOUNTER_STEP_ERROR      = 3,
} encounter_step_t;

// Sized to fit the largest message (ACCEPT, ~231 bytes) with margin.
// Matches peer_sync's I/O buffer convention so the peer characteristic
// driver can reuse a single 256-byte staging buffer for both flows.
#define ENCOUNTER_MSG_MAX_BYTES   256

// Per-peer fields whose values depend on the peer's identity. The
// responder doesn't know the peer at start time, so it can leave the
// matching members of `encounter_local_input_t` at zero and supply a
// `late_bind` callback (below) that fills these in once PROPOSE arrives
// and the peer's pub is known.
typedef struct {
    int32_t  rep_of_peer;
    uint64_t tx_bytes_to_peer_lifetime;
    uint64_t rx_bytes_from_peer_lifetime;
    uint64_t file_count_from_peer_lifetime;
} encounter_per_peer_input_t;

// Optional callback invoked by the responder after PROPOSE arrives and
// before ACCEPT is signed, with the peer's now-known 32-byte pub. The
// callback writes a per-peer state block (e.g., from peer_ledger) into
// `*out`. Returning false leaves the responder's lifetime fields at
// zero — correct first-contact semantics.
//
// Initiators ignore this field; they already know the peer's prefix at
// start time and populate the lifetime fields directly.
typedef bool (*encounter_late_bind_fn)(const uint8_t peer_pub[ENCOUNTER_PUB_BYTES],
                                       encounter_per_peer_input_t *out,
                                       void *ctx);

// Inputs the local side contributes to its half of the record. Caller
// fills these out before calling encounter_session_start. Numeric fields
// are read once (copied into the session) and may freely change after
// the call returns.
//
// `expected_peer_prefix` is checked against the first 8 bytes of the
// peer's pub on receipt of ACCEPT (initiator) or PROPOSE (responder).
// All-zero means "any peer." Initiators will normally fill this from
// the BLE advertisement that triggered the connect. Responders that
// don't know the prefix in advance pass all-zero.
typedef struct {
    uint8_t  pub[ENCOUNTER_PUB_BYTES];
    uint8_t  seed[ENCOUNTER_PUB_BYTES];
    uint8_t  nonce[ENCOUNTER_NONCE_BYTES];
    uint8_t  expected_peer_prefix[8];
    uint64_t meeting_count;
    int32_t  rep_of_peer;
    uint8_t  bssids[ENCOUNTER_BSSID_MAX][6];
    uint8_t  bssids_count;
    uint64_t tx_bytes_to_peer_lifetime;
    uint64_t rx_bytes_from_peer_lifetime;
    uint64_t file_count_from_peer_lifetime;
    // Responder-only late-binding hook. Ignored for initiators.
    encounter_late_bind_fn late_bind;
    void                  *late_bind_ctx;
} encounter_local_input_t;

// Opaque-by-convention. Callers should treat the fields as private and
// use the public API for everything. Exposed in the header so callers
// can stack-allocate the session.
//
// `body_buf` is scratch space for sign/verify that used to live on the
// stack; embedding it here keeps it out of the BLE callback's tight
// stack frame on platforms with small task stacks (e.g., the
// Arduino-ESP32 loopTask on the CardPuter caps at 8 KB).
typedef struct {
    encounter_role_t        role;
    int                     state;        // private; see encounter_session.c
    encounter_local_input_t me;
    encounter_record_t      rec;
    uint8_t                 body_buf[ENCOUNTER_BODY_MAX_BYTES];
} encounter_session_t;

// Begin a session in the given role.
//
//   Initiator: encodes PROPOSE into out_buf, returns NEED_WRITE. Caller
//              should send out_buf to the peer over BLE.
//   Responder: returns NEED_READ. Caller waits for the peer's PROPOSE
//              and feeds it via encounter_session_handle().
//
// Returns ENCOUNTER_STEP_ERROR on invalid inputs (bssids overflow, NULL
// pointers, out_max too small).
encounter_step_t encounter_session_start(encounter_session_t *s,
                                         encounter_role_t role,
                                         const encounter_local_input_t *me,
                                         uint8_t *out_buf, size_t out_max,
                                         size_t *out_len);

// Drive the state machine on an incoming message.
encounter_step_t encounter_session_handle(encounter_session_t *s,
                                          const uint8_t *in_buf, size_t in_len,
                                          uint8_t *out_buf, size_t out_max,
                                          size_t *out_len);

// After ENCOUNTER_STEP_DONE: copy the completed record. Returns true on
// success. Calling before DONE returns false and leaves *out untouched.
bool encounter_session_take_record(const encounter_session_t *s,
                                   encounter_record_t *out);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_ENCOUNTER_SESSION_H
