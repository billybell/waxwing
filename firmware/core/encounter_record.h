#ifndef WAXWING_ENCOUNTER_RECORD_H
#define WAXWING_ENCOUNTER_RECORD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Verifiable two-party encounter record (M4).
//
// An encounter is the cryptographic price-of-admission for a peer-sync
// session: when two nodes connect on the peer characteristic, they
// exchange (PROPOSE, ACCEPT, CONFIRM) and converge on a single CBOR map
// signed by both of them. The state machine driving that exchange is in
// encounter_session.{h,c} (a follow-up). This module is just the data
// shape — struct, canonical encoder, decoder, signature verification.
//
// All numeric reputation / byte-count fields are *lifetime* values
// reported by each side from its own ledger at the START of the session.
// They reflect the prior relationship between A and B, not what flows
// during this session. The next encounter between A and B will carry
// updated lifetime numbers; this one merely commits the current view.
// ---------------------------------------------------------------------------

#define ENCOUNTER_RECORD_VERSION    1
#define ENCOUNTER_BSSID_MAX         8
#define ENCOUNTER_NONCE_BYTES       16
#define ENCOUNTER_SIG_BYTES         64
#define ENCOUNTER_PUB_BYTES         32
#define ENCOUNTER_ID_BYTES          16

// Worst-case byte counts. Sized for the schema below, with a small
// margin so future additions don't immediately bust the slot budget.
#define ENCOUNTER_RECORD_MAX_BYTES  512
#define ENCOUNTER_BODY_MAX_BYTES    384

// Signed-body schema (CBOR map with integer keys 1..17). The full record
// is the signed body plus keys 18 and 19 (sig_a, sig_b).
//
//  1: v                                   uint
//  2: pub_a                               bstr(32)
//  3: pub_b                               bstr(32)
//  4: meeting_count_a                     uint
//  5: meeting_count_b                     uint
//  6: rep_of_b_by_a                       int
//  7: rep_of_a_by_b                       int
//  8: bssids_a                            array of bstr(6), 0..8 entries
//  9: bssids_b                            array of bstr(6), 0..8 entries
// 10: nonce_a                             bstr(16)
// 11: nonce_b                             bstr(16)
// 12: tx_bytes_a_to_b_lifetime            uint
// 13: rx_bytes_a_from_b_lifetime          uint
// 14: tx_bytes_b_to_a_lifetime            uint
// 15: rx_bytes_b_from_a_lifetime          uint
// 16: file_count_a_from_b_lifetime        uint
// 17: file_count_b_from_a_lifetime        uint
// 18: sig_a                               bstr(64) — over canonical body
// 19: sig_b                               bstr(64) — over canonical body
typedef struct {
    uint8_t  version;

    uint8_t  pub_a[ENCOUNTER_PUB_BYTES];
    uint8_t  pub_b[ENCOUNTER_PUB_BYTES];

    uint64_t meeting_count_a;
    uint64_t meeting_count_b;

    int32_t  rep_of_b_by_a;
    int32_t  rep_of_a_by_b;

    uint8_t  bssids_a_count;          // 0..ENCOUNTER_BSSID_MAX
    uint8_t  bssids_a[ENCOUNTER_BSSID_MAX][6];
    uint8_t  bssids_b_count;
    uint8_t  bssids_b[ENCOUNTER_BSSID_MAX][6];

    uint8_t  nonce_a[ENCOUNTER_NONCE_BYTES];
    uint8_t  nonce_b[ENCOUNTER_NONCE_BYTES];

    uint64_t tx_bytes_a_to_b_lifetime;
    uint64_t rx_bytes_a_from_b_lifetime;
    uint64_t tx_bytes_b_to_a_lifetime;
    uint64_t rx_bytes_b_from_a_lifetime;

    uint64_t file_count_a_from_b_lifetime;
    uint64_t file_count_b_from_a_lifetime;

    uint8_t  sig_a[ENCOUNTER_SIG_BYTES];
    uint8_t  sig_b[ENCOUNTER_SIG_BYTES];
} encounter_record_t;

// Encode the canonical signed body (keys 1..17). Returns bytes written,
// or 0 on encoder overflow / bssid count overflow / version mismatch.
//
// Both A and B must produce IDENTICAL bytes from this function so their
// signatures cover the same payload. The encoder is canonical (smallest
// integer encoding per RFC 8949 §3) by construction; both sides need
// only agree on field values.
size_t encounter_record_encode_body(const encounter_record_t *rec,
                                    uint8_t *out, size_t out_max);

// Encode the full record (keys 1..19). Returns bytes written, or 0.
size_t encounter_record_encode_full(const encounter_record_t *rec,
                                    uint8_t *out, size_t out_max);

// Decode a CBOR-encoded full record into `out`. Returns true on shape
// success; signatures are NOT verified here — call
// encounter_record_verify() explicitly.
//
// Strictness: bssid arrays must contain only bstr(6) entries; the array
// length must be ≤ ENCOUNTER_BSSID_MAX. Missing required keys reject.
// Extra unknown keys are tolerated (forward-compat) but ignored.
bool encounter_record_decode(const uint8_t *cbor, size_t len,
                             encounter_record_t *out);

// Verify both signatures against the canonical signed body bytes. Returns
// true iff sig_a is valid for pub_a AND sig_b is valid for pub_b. A
// record with a mismatched version field is rejected.
bool encounter_record_verify(const encounter_record_t *rec);

// Compute the content-addressed encounter id:
//   sha256(min(pub_a, pub_b) || max(pub_a, pub_b) || nonce_a || nonce_b)[:16]
//
// Side-symmetric (A and B compute the same value) and untamperable
// without changing one of the inputs that's already cryptographically
// committed.
void encounter_record_id(const encounter_record_t *rec,
                         uint8_t out[ENCOUNTER_ID_BYTES]);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_ENCOUNTER_RECORD_H
