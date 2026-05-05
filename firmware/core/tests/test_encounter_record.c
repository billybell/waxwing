#include "core/encounter_record.h"
#include "core/hal_crypto.h"

#include "stub_hal_crypto.h"
#include "test.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Fixture builders
// ---------------------------------------------------------------------------

static const uint8_t SEED_A[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
};
static const uint8_t SEED_B[32] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
};

// Build a valid signed record with the stub HAL. `pub_a/b` are derived
// from the seeds (`pub = seed XOR 0x5A` per the stub), and both halves
// sign the canonical body so encounter_record_verify() returns true.
static void build_signed(encounter_record_t *r) {
    memset(r, 0, sizeof(*r));
    r->version = ENCOUNTER_RECORD_VERSION;
    hal_ed25519_derive_pub(SEED_A, r->pub_a);
    hal_ed25519_derive_pub(SEED_B, r->pub_b);

    r->meeting_count_a = 12;
    r->meeting_count_b = 7;
    r->rep_of_b_by_a = -3;
    r->rep_of_a_by_b = 5;

    r->bssids_a_count = 2;
    uint8_t a0[6] = {0xAA, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t a1[6] = {0xAA, 0x10, 0x20, 0x30, 0x40, 0x50};
    memcpy(r->bssids_a[0], a0, 6);
    memcpy(r->bssids_a[1], a1, 6);

    r->bssids_b_count = 1;
    uint8_t b0[6] = {0xBB, 0x99, 0x88, 0x77, 0x66, 0x55};
    memcpy(r->bssids_b[0], b0, 6);

    for (int i = 0; i < ENCOUNTER_NONCE_BYTES; i++) {
        r->nonce_a[i] = (uint8_t)(0x70 + i);
        r->nonce_b[i] = (uint8_t)(0x80 + i);
    }

    r->tx_bytes_a_to_b_lifetime     = 4096;
    r->rx_bytes_a_from_b_lifetime   = 1024;
    r->tx_bytes_b_to_a_lifetime     = 1024;
    r->rx_bytes_b_from_a_lifetime   = 4000;     // small drift
    r->file_count_a_from_b_lifetime = 3;
    r->file_count_b_from_a_lifetime = 5;

    uint8_t body[ENCOUNTER_BODY_MAX_BYTES];
    size_t  body_len = encounter_record_encode_body(r, body, sizeof(body));
    hal_ed25519_sign(SEED_A, body, body_len, r->sig_a);
    hal_ed25519_sign(SEED_B, body, body_len, r->sig_b);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_encounter_record_body_byte_for_byte_reproducible(void) {
    encounter_record_t r;
    build_signed(&r);

    uint8_t body1[ENCOUNTER_BODY_MAX_BYTES];
    uint8_t body2[ENCOUNTER_BODY_MAX_BYTES];
    size_t  n1 = encounter_record_encode_body(&r, body1, sizeof(body1));
    size_t  n2 = encounter_record_encode_body(&r, body2, sizeof(body2));
    TEST_ASSERT(n1 > 0,        "body encodes");
    TEST_ASSERT(n1 == n2,      "two encode passes produce same length");
    TEST_ASSERT(memcmp(body1, body2, n1) == 0,
                "two encode passes produce identical bytes");
}

void test_encounter_record_full_round_trip(void) {
    encounter_record_t r, decoded;
    build_signed(&r);

    uint8_t buf[ENCOUNTER_RECORD_MAX_BYTES];
    size_t  n = encounter_record_encode_full(&r, buf, sizeof(buf));
    TEST_ASSERT(n > 0,                          "full encode succeeds");
    TEST_ASSERT(n <= ENCOUNTER_RECORD_MAX_BYTES, "fits in slot budget");

    bool ok = encounter_record_decode(buf, n, &decoded);
    TEST_ASSERT(ok, "decode succeeds");

    TEST_ASSERT(decoded.version == r.version,                  "version round-trips");
    TEST_ASSERT(memcmp(decoded.pub_a, r.pub_a, 32) == 0,       "pub_a round-trips");
    TEST_ASSERT(memcmp(decoded.pub_b, r.pub_b, 32) == 0,       "pub_b round-trips");
    TEST_ASSERT(decoded.meeting_count_a == r.meeting_count_a,  "meeting_count_a round-trips");
    TEST_ASSERT(decoded.meeting_count_b == r.meeting_count_b,  "meeting_count_b round-trips");
    TEST_ASSERT(decoded.rep_of_b_by_a == r.rep_of_b_by_a,      "rep_of_b_by_a round-trips (negative)");
    TEST_ASSERT(decoded.rep_of_a_by_b == r.rep_of_a_by_b,      "rep_of_a_by_b round-trips (positive)");
    TEST_ASSERT(decoded.bssids_a_count == r.bssids_a_count,    "bssids_a count");
    TEST_ASSERT(memcmp(decoded.bssids_a, r.bssids_a,
                       (size_t)r.bssids_a_count * 6) == 0,
                "bssids_a payload");
    TEST_ASSERT(decoded.bssids_b_count == r.bssids_b_count,    "bssids_b count");
    TEST_ASSERT(memcmp(decoded.bssids_b, r.bssids_b,
                       (size_t)r.bssids_b_count * 6) == 0,
                "bssids_b payload");
    TEST_ASSERT(memcmp(decoded.nonce_a, r.nonce_a, 16) == 0,   "nonce_a round-trips");
    TEST_ASSERT(memcmp(decoded.nonce_b, r.nonce_b, 16) == 0,   "nonce_b round-trips");
    TEST_ASSERT(decoded.tx_bytes_a_to_b_lifetime  == 4096,     "tx_ab");
    TEST_ASSERT(decoded.rx_bytes_a_from_b_lifetime == 1024,    "rx_ab");
    TEST_ASSERT(decoded.tx_bytes_b_to_a_lifetime  == 1024,     "tx_ba");
    TEST_ASSERT(decoded.rx_bytes_b_from_a_lifetime == 4000,    "rx_ba");
    TEST_ASSERT(decoded.file_count_a_from_b_lifetime == 3,     "file_ab");
    TEST_ASSERT(decoded.file_count_b_from_a_lifetime == 5,     "file_ba");
    TEST_ASSERT(memcmp(decoded.sig_a, r.sig_a, 64) == 0,       "sig_a round-trips");
    TEST_ASSERT(memcmp(decoded.sig_b, r.sig_b, 64) == 0,       "sig_b round-trips");
}

void test_encounter_record_verify_happy_path(void) {
    encounter_record_t r;
    build_signed(&r);
    TEST_ASSERT(encounter_record_verify(&r), "both sigs verify");
}

void test_encounter_record_verify_rejects_tampered_field(void) {
    encounter_record_t r;
    build_signed(&r);

    // Flipping any signed field should invalidate both sigs (the body
    // bytes change). Pick a field that isn't a signature itself.
    r.tx_bytes_a_to_b_lifetime = 0;
    TEST_ASSERT(!encounter_record_verify(&r),
                "tampered tx count breaks verification");
}

void test_encounter_record_verify_rejects_tampered_sig(void) {
    encounter_record_t r;
    build_signed(&r);
    r.sig_a[0] ^= 0xFF;
    TEST_ASSERT(!encounter_record_verify(&r),
                "flipped byte in sig_a fails verify");

    build_signed(&r);
    r.sig_b[63] ^= 0x01;
    TEST_ASSERT(!encounter_record_verify(&r),
                "flipped byte in sig_b fails verify");
}

void test_encounter_record_verify_rejects_swapped_sigs(void) {
    encounter_record_t r;
    build_signed(&r);
    // Swap sig_a and sig_b. Each was signed by a different seed, so
    // neither will verify against the other side's pub.
    uint8_t tmp[64];
    memcpy(tmp,    r.sig_a, 64);
    memcpy(r.sig_a, r.sig_b, 64);
    memcpy(r.sig_b, tmp,    64);
    TEST_ASSERT(!encounter_record_verify(&r),
                "swapping the two signatures fails verify");
}

void test_encounter_record_decode_rejects_wrong_version(void) {
    encounter_record_t r, decoded;
    build_signed(&r);
    r.version = ENCOUNTER_RECORD_VERSION + 1;

    uint8_t buf[ENCOUNTER_RECORD_MAX_BYTES];
    size_t n = encounter_record_encode_full(&r, buf, sizeof(buf));
    TEST_ASSERT(n == 0, "encoder refuses unknown version");

    // Hand-craft a record with the wrong version field by tampering
    // with the bytes from a valid encode. Encode at v=1, then patch
    // the value byte for key 1 (the second uint after the map header).
    r.version = ENCOUNTER_RECORD_VERSION;
    n = encounter_record_encode_full(&r, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "valid encode succeeds");
    // Map header (1 byte 0xb3 for 19 entries), then key 1 (0x01),
    // then value 1 (0x01) — flip that to 0x02.
    buf[2] = 0x02;
    bool ok = encounter_record_decode(buf, n, &decoded);
    TEST_ASSERT(!ok, "decoder rejects v=2 payload");
}

void test_encounter_record_decode_rejects_oversized_bssids(void) {
    encounter_record_t r;
    build_signed(&r);
    r.bssids_a_count = ENCOUNTER_BSSID_MAX + 1;
    uint8_t buf[ENCOUNTER_RECORD_MAX_BYTES];
    size_t  n = encounter_record_encode_full(&r, buf, sizeof(buf));
    TEST_ASSERT(n == 0, "encoder refuses bssid overflow");
}

void test_encounter_record_id_is_side_symmetric(void) {
    encounter_record_t r1, r2;
    build_signed(&r1);

    // Same record but with pubs swapped (i.e. compute as if the OTHER
    // side built it). The id must be identical.
    memset(&r2, 0, sizeof(r2));
    r2.version = r1.version;
    memcpy(r2.pub_a, r1.pub_b, 32);
    memcpy(r2.pub_b, r1.pub_a, 32);
    memcpy(r2.nonce_a, r1.nonce_a, 16);
    memcpy(r2.nonce_b, r1.nonce_b, 16);

    uint8_t id1[ENCOUNTER_ID_BYTES], id2[ENCOUNTER_ID_BYTES];
    encounter_record_id(&r1, id1);
    encounter_record_id(&r2, id2);
    TEST_ASSERT(memcmp(id1, id2, ENCOUNTER_ID_BYTES) == 0,
                "id is side-symmetric (lex-min ordering of pubs)");
}

void test_encounter_record_id_changes_with_nonce(void) {
    encounter_record_t r;
    build_signed(&r);
    uint8_t id1[ENCOUNTER_ID_BYTES], id2[ENCOUNTER_ID_BYTES];
    encounter_record_id(&r, id1);
    r.nonce_a[0] ^= 0x01;
    encounter_record_id(&r, id2);
    TEST_ASSERT(memcmp(id1, id2, ENCOUNTER_ID_BYTES) != 0,
                "flipping a nonce bit changes the id");
}

void test_encounter_record_decode_rejects_truncated(void) {
    encounter_record_t r, decoded;
    build_signed(&r);
    uint8_t buf[ENCOUNTER_RECORD_MAX_BYTES];
    size_t  n = encounter_record_encode_full(&r, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "encode succeeds");
    // Lop off 4 bytes — sig_b will be incomplete.
    bool ok = encounter_record_decode(buf, n - 4, &decoded);
    TEST_ASSERT(!ok, "truncated payload rejected");
}

void test_encounter_record_empty_bssids_allowed(void) {
    encounter_record_t r, decoded;
    build_signed(&r);
    r.bssids_a_count = 0;
    r.bssids_b_count = 0;

    // Re-sign over the modified body.
    uint8_t body[ENCOUNTER_BODY_MAX_BYTES];
    size_t  body_len = encounter_record_encode_body(&r, body, sizeof(body));
    hal_ed25519_sign(SEED_A, body, body_len, r.sig_a);
    hal_ed25519_sign(SEED_B, body, body_len, r.sig_b);

    uint8_t buf[ENCOUNTER_RECORD_MAX_BYTES];
    size_t  n = encounter_record_encode_full(&r, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "encode with empty bssids succeeds");
    TEST_ASSERT(encounter_record_decode(buf, n, &decoded), "decode succeeds");
    TEST_ASSERT(decoded.bssids_a_count == 0, "bssids_a empty round-trips");
    TEST_ASSERT(decoded.bssids_b_count == 0, "bssids_b empty round-trips");
    TEST_ASSERT(encounter_record_verify(&decoded), "empty-bssid record verifies");
}
