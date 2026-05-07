#include "core/encounter_session.h"
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

static void make_input(encounter_local_input_t *me,
                       const uint8_t seed[32],
                       uint64_t meeting_count,
                       int32_t  rep_of_peer,
                       uint8_t nonce_seed) {
    memset(me, 0, sizeof(*me));
    memcpy(me->seed, seed, 32);
    hal_ed25519_derive_pub(seed, me->pub);
    for (int i = 0; i < ENCOUNTER_NONCE_BYTES; i++) {
        me->nonce[i] = (uint8_t)(nonce_seed + i);
    }
    me->meeting_count = meeting_count;
    me->rep_of_peer   = rep_of_peer;
    me->bssids_count  = 2;
    uint8_t b0[6] = {nonce_seed, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    uint8_t b1[6] = {nonce_seed, 0x10, 0x20, 0x30, 0x40, 0x50};
    memcpy(me->bssids[0], b0, 6);
    memcpy(me->bssids[1], b1, 6);
    me->tx_bytes_to_peer_lifetime     = 1000 * (nonce_seed + 1);
    me->rx_bytes_from_peer_lifetime   =  500 * (nonce_seed + 1);
    me->file_count_from_peer_lifetime = nonce_seed;
}

// Drive both sides through the three-message exchange. Returns true if
// both sides reach DONE and their records compare byte-for-byte equal.
static bool run_handshake(encounter_session_t *init,
                          encounter_session_t *resp,
                          const encounter_local_input_t *me_a,
                          const encounter_local_input_t *me_b) {
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t  n_a = 0, n_b = 0;

    encounter_step_t step;

    // A starts → PROPOSE in buf_a.
    step = encounter_session_start(init, ENCOUNTER_ROLE_INITIATOR, me_a,
                                   buf_a, sizeof(buf_a), &n_a);
    if (step != ENCOUNTER_STEP_NEED_WRITE) return false;

    // B starts → NEED_READ.
    step = encounter_session_start(resp, ENCOUNTER_ROLE_RESPONDER, me_b,
                                   buf_b, sizeof(buf_b), &n_b);
    if (step != ENCOUNTER_STEP_NEED_READ) return false;

    // B receives PROPOSE → emits ACCEPT.
    step = encounter_session_handle(resp, buf_a, n_a,
                                    buf_b, sizeof(buf_b), &n_b);
    if (step != ENCOUNTER_STEP_NEED_WRITE) return false;

    // A receives ACCEPT → emits CONFIRM, transitions to DONE.
    step = encounter_session_handle(init, buf_b, n_b,
                                    buf_a, sizeof(buf_a), &n_a);
    if (step != ENCOUNTER_STEP_NEED_WRITE) return false;

    // B receives CONFIRM → DONE.
    step = encounter_session_handle(resp, buf_a, n_a,
                                    buf_b, sizeof(buf_b), &n_b);
    if (step != ENCOUNTER_STEP_DONE) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_encounter_session_happy_path(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 12, -3, 0x10);
    make_input(&me_b, SEED_B,  7,  5, 0x20);

    encounter_session_t init = {0}, resp = {0};
    bool ok = run_handshake(&init, &resp, &me_a, &me_b);
    TEST_ASSERT(ok, "handshake completes for both sides");

    encounter_record_t rec_a, rec_b;
    TEST_ASSERT(encounter_session_take_record(&init, &rec_a),
                "initiator surrenders record on DONE");
    TEST_ASSERT(encounter_session_take_record(&resp, &rec_b),
                "responder surrenders record on DONE");

    // Field-level checks of A's record.
    TEST_ASSERT(memcmp(rec_a.pub_a, me_a.pub, 32) == 0, "rec_a.pub_a");
    TEST_ASSERT(memcmp(rec_a.pub_b, me_b.pub, 32) == 0, "rec_a.pub_b");
    TEST_ASSERT(rec_a.meeting_count_a == me_a.meeting_count, "rec_a.mc_a");
    TEST_ASSERT(rec_a.meeting_count_b == me_b.meeting_count, "rec_a.mc_b");
    TEST_ASSERT(rec_a.rep_of_b_by_a == me_a.rep_of_peer,     "rec_a.rep_b_by_a");
    TEST_ASSERT(rec_a.rep_of_a_by_b == me_b.rep_of_peer,     "rec_a.rep_a_by_b");
    TEST_ASSERT(memcmp(rec_a.nonce_a, me_a.nonce, 16) == 0,  "rec_a.nonce_a");
    TEST_ASSERT(memcmp(rec_a.nonce_b, me_b.nonce, 16) == 0,  "rec_a.nonce_b");

    // Both sides converged on the same record.
    TEST_ASSERT(memcmp(&rec_a, &rec_b, sizeof(rec_a)) == 0,
                "initiator's and responder's records are byte-equal");

    // Record verifies (both sigs against canonical body).
    TEST_ASSERT(encounter_record_verify(&rec_a),
                "completed record's sigs both verify");
}

void test_encounter_session_id_agrees_between_sides(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x33);
    make_input(&me_b, SEED_B, 2, 0, 0x44);

    encounter_session_t init = {0}, resp = {0};
    TEST_ASSERT(run_handshake(&init, &resp, &me_a, &me_b),
                "handshake completes");

    encounter_record_t rec_a, rec_b;
    encounter_session_take_record(&init, &rec_a);
    encounter_session_take_record(&resp, &rec_b);

    uint8_t id_a[ENCOUNTER_ID_BYTES], id_b[ENCOUNTER_ID_BYTES];
    encounter_record_id(&rec_a, id_a);
    encounter_record_id(&rec_b, id_b);
    TEST_ASSERT(memcmp(id_a, id_b, ENCOUNTER_ID_BYTES) == 0,
                "encounter_id matches on both sides");
}

void test_encounter_session_initiator_rejects_wrong_prefix(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);
    // Initiator expects a peer prefix that doesn't match B's pub.
    uint8_t bogus[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(me_a.expected_peer_prefix, bogus, 8);

    encounter_session_t init = {0}, resp = {0};
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t n_a = 0, n_b = 0;

    encounter_session_start(&init, ENCOUNTER_ROLE_INITIATOR, &me_a,
                            buf_a, sizeof(buf_a), &n_a);
    encounter_session_start(&resp, ENCOUNTER_ROLE_RESPONDER, &me_b,
                            buf_b, sizeof(buf_b), &n_b);
    encounter_session_handle(&resp, buf_a, n_a, buf_b, sizeof(buf_b), &n_b);

    encounter_step_t step = encounter_session_handle(&init, buf_b, n_b,
                                                      buf_a, sizeof(buf_a), &n_a);
    TEST_ASSERT(step == ENCOUNTER_STEP_ERROR,
                "initiator aborts on prefix mismatch");
    encounter_record_t out;
    TEST_ASSERT(!encounter_session_take_record(&init, &out),
                "no record surrendered after prefix mismatch");
}

void test_encounter_session_responder_rejects_wrong_prefix(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);
    uint8_t bogus[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(me_b.expected_peer_prefix, bogus, 8);

    encounter_session_t init = {0}, resp = {0};
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t n_a = 0, n_b = 0;

    encounter_session_start(&init, ENCOUNTER_ROLE_INITIATOR, &me_a,
                            buf_a, sizeof(buf_a), &n_a);
    encounter_session_start(&resp, ENCOUNTER_ROLE_RESPONDER, &me_b,
                            buf_b, sizeof(buf_b), &n_b);
    encounter_step_t step = encounter_session_handle(&resp, buf_a, n_a,
                                                      buf_b, sizeof(buf_b), &n_b);
    TEST_ASSERT(step == ENCOUNTER_STEP_ERROR,
                "responder aborts on prefix mismatch");
}

void test_encounter_session_initiator_rejects_bad_sig_b(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);

    encounter_session_t init = {0}, resp = {0};
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t n_a = 0, n_b = 0;

    encounter_session_start(&init, ENCOUNTER_ROLE_INITIATOR, &me_a,
                            buf_a, sizeof(buf_a), &n_a);
    encounter_session_start(&resp, ENCOUNTER_ROLE_RESPONDER, &me_b,
                            buf_b, sizeof(buf_b), &n_b);
    encounter_session_handle(&resp, buf_a, n_a, buf_b, sizeof(buf_b), &n_b);

    // Tamper with the last byte of ACCEPT (which is inside sig_b's payload).
    buf_b[n_b - 1] ^= 0xFF;

    encounter_step_t step = encounter_session_handle(&init, buf_b, n_b,
                                                      buf_a, sizeof(buf_a), &n_a);
    TEST_ASSERT(step == ENCOUNTER_STEP_ERROR,
                "initiator rejects accept with tampered sig_b");
}

void test_encounter_session_responder_rejects_bad_sig_a(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);

    encounter_session_t init = {0}, resp = {0};
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t n_a = 0, n_b = 0;

    encounter_session_start(&init, ENCOUNTER_ROLE_INITIATOR, &me_a,
                            buf_a, sizeof(buf_a), &n_a);
    encounter_session_start(&resp, ENCOUNTER_ROLE_RESPONDER, &me_b,
                            buf_b, sizeof(buf_b), &n_b);
    encounter_session_handle(&resp, buf_a, n_a, buf_b, sizeof(buf_b), &n_b);
    encounter_session_handle(&init, buf_b, n_b, buf_a, sizeof(buf_a), &n_a);

    // Tamper CONFIRM's sig_a payload (last byte).
    buf_a[n_a - 1] ^= 0xFF;

    encounter_step_t step = encounter_session_handle(&resp, buf_a, n_a,
                                                      buf_b, sizeof(buf_b), &n_b);
    TEST_ASSERT(step == ENCOUNTER_STEP_ERROR,
                "responder rejects confirm with tampered sig_a");
}

void test_encounter_session_initiator_drop_after_propose(void) {
    encounter_local_input_t me_a;
    make_input(&me_a, SEED_A, 1, 0, 0x10);

    encounter_session_t init = {0};
    uint8_t buf[ENCOUNTER_MSG_MAX_BYTES];
    size_t n = 0;

    encounter_step_t step = encounter_session_start(
        &init, ENCOUNTER_ROLE_INITIATOR, &me_a, buf, sizeof(buf), &n);
    TEST_ASSERT(step == ENCOUNTER_STEP_NEED_WRITE,
                "initiator emits PROPOSE");

    // Caller drops the link before sending CONFIRM. No record persisted.
    encounter_record_t out;
    TEST_ASSERT(!encounter_session_take_record(&init, &out),
                "no record after drop in I_AWAIT_ACCEPT");
}

void test_encounter_session_responder_drop_after_accept(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);

    encounter_session_t init = {0}, resp = {0};
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t n_a = 0, n_b = 0;

    encounter_session_start(&init, ENCOUNTER_ROLE_INITIATOR, &me_a,
                            buf_a, sizeof(buf_a), &n_a);
    encounter_session_start(&resp, ENCOUNTER_ROLE_RESPONDER, &me_b,
                            buf_b, sizeof(buf_b), &n_b);
    encounter_step_t step = encounter_session_handle(&resp, buf_a, n_a,
                                                      buf_b, sizeof(buf_b), &n_b);
    TEST_ASSERT(step == ENCOUNTER_STEP_NEED_WRITE,
                "responder emits ACCEPT");

    // Caller drops the link before CONFIRM arrives.
    encounter_record_t out;
    TEST_ASSERT(!encounter_session_take_record(&resp, &out),
                "no record after drop in R_AWAIT_CONFIRM");
}

void test_encounter_session_responder_rejects_propose_wrong_type(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);

    encounter_session_t init = {0}, resp = {0};
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t n_a = 0, n_b = 0;

    encounter_session_start(&init, ENCOUNTER_ROLE_INITIATOR, &me_a,
                            buf_a, sizeof(buf_a), &n_a);
    encounter_session_start(&resp, ENCOUNTER_ROLE_RESPONDER, &me_b,
                            buf_b, sizeof(buf_b), &n_b);

    // The map header is 0xaa (map(10)), then key 1 (0x01), then value 1
    // (0x01) for type=1=PROPOSE. Patch type to 2 (ACCEPT) — responder
    // should reject because it expects a PROPOSE.
    buf_a[2] = 0x02;
    encounter_step_t step = encounter_session_handle(&resp, buf_a, n_a,
                                                      buf_b, sizeof(buf_b), &n_b);
    TEST_ASSERT(step == ENCOUNTER_STEP_ERROR,
                "responder rejects ACCEPT-typed first message");
}

void test_encounter_session_handle_before_start_is_error(void) {
    encounter_session_t s;
    memset(&s, 0, sizeof(s));   // role = INITIATOR, state = ST_INIT
    uint8_t buf[ENCOUNTER_MSG_MAX_BYTES];
    size_t n = 0;
    uint8_t junk[4] = {0xa0, 0, 0, 0};

    encounter_step_t step = encounter_session_handle(&s, junk, sizeof(junk),
                                                      buf, sizeof(buf), &n);
    TEST_ASSERT(step == ENCOUNTER_STEP_ERROR,
                "handle without a prior start refuses to advance");
}

// ---------------------------------------------------------------------------
// Late-bind callback (responder lookup of per-peer state from PROPOSE)
// ---------------------------------------------------------------------------

typedef struct {
    bool       called;
    uint8_t    seen_pub[ENCOUNTER_PUB_BYTES];
    bool       return_hit;
    int32_t    rep_to_return;
    uint64_t   tx_to_return;
    uint64_t   rx_to_return;
    uint64_t   files_to_return;
} late_bind_probe_t;

static bool late_bind_probe_cb(const uint8_t peer_pub[ENCOUNTER_PUB_BYTES],
                               encounter_per_peer_input_t *out, void *ctx) {
    late_bind_probe_t *p = (late_bind_probe_t *)ctx;
    p->called = true;
    memcpy(p->seen_pub, peer_pub, ENCOUNTER_PUB_BYTES);
    if (!p->return_hit) return false;
    out->rep_of_peer                    = p->rep_to_return;
    out->tx_bytes_to_peer_lifetime      = p->tx_to_return;
    out->rx_bytes_from_peer_lifetime    = p->rx_to_return;
    out->file_count_from_peer_lifetime  = p->files_to_return;
    return true;
}

void test_encounter_session_responder_late_bind_populates_b_side(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);

    // Wipe the per-peer fields on B; they'll be filled by the callback.
    me_b.rep_of_peer                    = 0;
    me_b.tx_bytes_to_peer_lifetime      = 0;
    me_b.rx_bytes_from_peer_lifetime    = 0;
    me_b.file_count_from_peer_lifetime  = 0;

    late_bind_probe_t probe = {0};
    probe.return_hit       = true;
    probe.rep_to_return    = 17;
    probe.tx_to_return     = 0xAAAA;
    probe.rx_to_return     = 0xBBBB;
    probe.files_to_return  = 7;
    me_b.late_bind         = late_bind_probe_cb;
    me_b.late_bind_ctx     = &probe;

    encounter_session_t init = {0}, resp = {0};
    TEST_ASSERT(run_handshake(&init, &resp, &me_a, &me_b),
                "handshake completes with late_bind");

    TEST_ASSERT(probe.called, "responder invoked late_bind");
    TEST_ASSERT(memcmp(probe.seen_pub, me_a.pub, ENCOUNTER_PUB_BYTES) == 0,
                "late_bind saw the initiator's pub");

    encounter_record_t rec_a, rec_b;
    encounter_session_take_record(&init, &rec_a);
    encounter_session_take_record(&resp, &rec_b);

    TEST_ASSERT(rec_a.tx_bytes_b_to_a_lifetime    == 0xAAAA, "B's signed tx visible to A");
    TEST_ASSERT(rec_a.rx_bytes_b_from_a_lifetime  == 0xBBBB, "B's signed rx visible to A");
    TEST_ASSERT(rec_a.file_count_b_from_a_lifetime == 7,     "B's signed file count visible to A");
    TEST_ASSERT(rec_a.rep_of_a_by_b               == 17,    "B's signed rep_of_a visible to A");

    TEST_ASSERT(memcmp(&rec_a, &rec_b, sizeof(rec_a)) == 0,
                "both sides converge on the late-bound record");
    TEST_ASSERT(encounter_record_verify(&rec_a),
                "late-bound record verifies (sig_b covers updated body)");
}

void test_encounter_session_responder_late_bind_miss_keeps_zeros(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);

    me_b.rep_of_peer                    = 0;
    me_b.tx_bytes_to_peer_lifetime      = 0;
    me_b.rx_bytes_from_peer_lifetime    = 0;
    me_b.file_count_from_peer_lifetime  = 0;

    late_bind_probe_t probe = {0};
    probe.return_hit = false;  // simulate first contact / not in ledger
    me_b.late_bind     = late_bind_probe_cb;
    me_b.late_bind_ctx = &probe;

    encounter_session_t init = {0}, resp = {0};
    TEST_ASSERT(run_handshake(&init, &resp, &me_a, &me_b),
                "handshake completes when late_bind returns false");
    TEST_ASSERT(probe.called, "callback was still invoked");

    encounter_record_t rec_b;
    encounter_session_take_record(&resp, &rec_b);
    TEST_ASSERT(rec_b.tx_bytes_b_to_a_lifetime     == 0, "no late_bind hit → tx stays 0");
    TEST_ASSERT(rec_b.rx_bytes_b_from_a_lifetime   == 0, "no late_bind hit → rx stays 0");
    TEST_ASSERT(rec_b.file_count_b_from_a_lifetime == 0, "no late_bind hit → file count stays 0");
    TEST_ASSERT(rec_b.rep_of_a_by_b                == 0, "no late_bind hit → rep stays 0");
    TEST_ASSERT(encounter_record_verify(&rec_b),
                "first-contact record (zeros) still verifies");
}

// Mirror the real firmware path: both sides at first contact, zero
// scan / zero meeting counts / zero lifetime fields, late_bind set on
// the responder but returning false (no peer_ledger entry yet). The
// previous tests use make_input which seeds nonzero bssids/lifetimes;
// this strips that down to the wire shape the firmware actually sees
// on first connect between two fresh devices.
void test_encounter_session_first_contact_with_late_bind_set(void) {
    encounter_local_input_t me_a, me_b;
    memset(&me_a, 0, sizeof(me_a));
    memset(&me_b, 0, sizeof(me_b));
    memcpy(me_a.seed, SEED_A, 32);
    memcpy(me_b.seed, SEED_B, 32);
    hal_ed25519_derive_pub(me_a.seed, me_a.pub);
    hal_ed25519_derive_pub(me_b.seed, me_b.pub);
    // Distinct nonces — otherwise the encounter id would collide.
    for (int i = 0; i < ENCOUNTER_NONCE_BYTES; i++) {
        me_a.nonce[i] = (uint8_t)(0xA0 + i);
        me_b.nonce[i] = (uint8_t)(0xB0 + i);
    }
    // bssids_count = 0, lifetime fields = 0, meeting_count = 0.

    late_bind_probe_t probe = {0};
    probe.return_hit = false; // first contact
    me_b.late_bind     = late_bind_probe_cb;
    me_b.late_bind_ctx = &probe;

    encounter_session_t init = {0}, resp = {0};
    TEST_ASSERT(run_handshake(&init, &resp, &me_a, &me_b),
                "first-contact handshake completes (matches firmware path)");
    TEST_ASSERT(probe.called, "late_bind invoked even on first contact");

    encounter_record_t rec_a, rec_b;
    encounter_session_take_record(&init, &rec_a);
    encounter_session_take_record(&resp, &rec_b);
    TEST_ASSERT(memcmp(&rec_a, &rec_b, sizeof(rec_a)) == 0,
                "first-contact records byte-equal across sides");
    TEST_ASSERT(encounter_record_verify(&rec_a),
                "first-contact record verifies");
}

void test_encounter_session_responder_late_bind_runs_after_prefix_check(void) {
    encounter_local_input_t me_a, me_b;
    make_input(&me_a, SEED_A, 1, 0, 0x10);
    make_input(&me_b, SEED_B, 1, 0, 0x20);
    uint8_t bogus[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(me_b.expected_peer_prefix, bogus, 8);

    late_bind_probe_t probe = {0};
    probe.return_hit = true;
    probe.tx_to_return = 9999;
    me_b.late_bind     = late_bind_probe_cb;
    me_b.late_bind_ctx = &probe;

    encounter_session_t init = {0}, resp = {0};
    uint8_t buf_a[ENCOUNTER_MSG_MAX_BYTES];
    uint8_t buf_b[ENCOUNTER_MSG_MAX_BYTES];
    size_t n_a = 0, n_b = 0;

    encounter_session_start(&init, ENCOUNTER_ROLE_INITIATOR, &me_a,
                            buf_a, sizeof(buf_a), &n_a);
    encounter_session_start(&resp, ENCOUNTER_ROLE_RESPONDER, &me_b,
                            buf_b, sizeof(buf_b), &n_b);
    encounter_step_t step = encounter_session_handle(&resp, buf_a, n_a,
                                                      buf_b, sizeof(buf_b), &n_b);
    TEST_ASSERT(step == ENCOUNTER_STEP_ERROR,
                "responder rejects on prefix mismatch");
    TEST_ASSERT(!probe.called,
                "late_bind is skipped when the prefix check fails");
}
