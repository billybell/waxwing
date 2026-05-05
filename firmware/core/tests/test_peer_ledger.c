#include "core/peer_ledger.h"
#include "mock_filestore.h"
#include "test.h"

#include <string.h>

// Build a deterministic 32-byte pub. The first byte uniquely names the
// peer in tests so we can spot which one survived eviction.
static void make_pub(uint8_t pub[32], uint8_t tag) {
    memset(pub, 0, 32);
    pub[0] = tag;
    for (int i = 1; i < 32; i++) pub[i] = (uint8_t)(tag + i);
}

static void clear_state(void) {
    mock_fs_clear();
    peer_ledger_init();   // reload from now-empty mock → empty
}

void test_peer_ledger_starts_empty(void) {
    clear_state();
    TEST_ASSERT(peer_ledger_count() == 0, "fresh init: empty ledger");

    uint8_t pub[32];
    make_pub(pub, 0xAA);
    peer_ledger_entry_t e;
    TEST_ASSERT(!peer_ledger_get(pub, &e), "missing peer: get returns false");
    TEST_ASSERT(e.tx_bytes_lifetime == 0, "out is zeroed on miss");
}

void test_peer_ledger_first_contact_creates_entry(void) {
    clear_state();
    uint8_t pub[32];
    make_pub(pub, 0xAA);

    int rc = peer_ledger_apply(pub, 100, 50, 1, 7);
    TEST_ASSERT(rc == 0, "apply succeeds");
    TEST_ASSERT(peer_ledger_count() == 1, "one peer in ledger");

    peer_ledger_entry_t e;
    TEST_ASSERT(peer_ledger_get(pub, &e),                "peer found");
    TEST_ASSERT(e.tx_bytes_lifetime == 100,              "tx delta applied");
    TEST_ASSERT(e.rx_bytes_lifetime == 50,               "rx delta applied");
    TEST_ASSERT(e.file_count_lifetime == 1,              "file delta applied");
    TEST_ASSERT(e.last_meeting_count == 7,               "peer count recorded");
    TEST_ASSERT(e.my_rep_score == 0,                     "rep starts at 0");
}

void test_peer_ledger_deltas_accumulate(void) {
    clear_state();
    uint8_t pub[32];
    make_pub(pub, 0xAA);

    peer_ledger_apply(pub, 100, 50, 1, 7);
    peer_ledger_apply(pub, 200, 75, 2, 8);
    peer_ledger_apply(pub,   1,  1, 0, 8);

    peer_ledger_entry_t e;
    peer_ledger_get(pub, &e);
    TEST_ASSERT(e.tx_bytes_lifetime == 301,   "tx accumulates");
    TEST_ASSERT(e.rx_bytes_lifetime == 126,   "rx accumulates");
    TEST_ASSERT(e.file_count_lifetime == 3,   "files accumulate");
    TEST_ASSERT(e.last_meeting_count == 8,    "meeting count tracks max");
}

void test_peer_ledger_meeting_count_does_not_decrease(void) {
    clear_state();
    uint8_t pub[32];
    make_pub(pub, 0xAA);

    peer_ledger_apply(pub, 0, 0, 0, 100);
    peer_ledger_apply(pub, 0, 0, 0,  50);    // peer claimed lower count

    peer_ledger_entry_t e;
    peer_ledger_get(pub, &e);
    TEST_ASSERT(e.last_meeting_count == 100,
                "lower peer count is ignored — could be replay/fraud");
}

void test_peer_ledger_persists_across_init(void) {
    clear_state();
    uint8_t pub_a[32], pub_b[32];
    make_pub(pub_a, 0xAA);
    make_pub(pub_b, 0xBB);

    peer_ledger_apply(pub_a, 100, 200, 3, 10);
    peer_ledger_apply(pub_b, 999, 888,  7, 42);

    // Simulate reboot.
    peer_ledger_reset();
    TEST_ASSERT(peer_ledger_count() == 0, "reset clears RAM");
    peer_ledger_init();
    TEST_ASSERT(peer_ledger_count() == 2, "both peers reload");

    peer_ledger_entry_t e;
    TEST_ASSERT(peer_ledger_get(pub_a, &e), "pub_a survives reboot");
    TEST_ASSERT(e.tx_bytes_lifetime == 100, "pub_a tx survives");
    TEST_ASSERT(e.rx_bytes_lifetime == 200, "pub_a rx survives");
    TEST_ASSERT(peer_ledger_get(pub_b, &e), "pub_b survives reboot");
    TEST_ASSERT(e.last_meeting_count == 42, "pub_b meeting count survives");
    TEST_ASSERT(e.file_count_lifetime == 7, "pub_b files survive");
}

void test_peer_ledger_eviction_picks_lowest_meeting_count(void) {
    clear_state();
    // Fill the table with peers whose meeting counts are i+1 (so the
    // peer at slot 0 has count=1 and is the eviction target).
    for (int i = 0; i < PEER_LEDGER_CAP; i++) {
        uint8_t pub[32];
        make_pub(pub, (uint8_t)(0x10 + i));
        peer_ledger_apply(pub, (uint64_t)i, 0, 0, (uint64_t)(i + 1));
    }
    TEST_ASSERT(peer_ledger_count() == PEER_LEDGER_CAP, "table full");

    // Insert one more peer; victim should be the lowest-meeting-count one.
    uint8_t newcomer[32];
    make_pub(newcomer, 0xFE);
    peer_ledger_apply(newcomer, 7, 7, 1, 999);
    TEST_ASSERT(peer_ledger_count() == PEER_LEDGER_CAP, "still full");

    // Original peer at slot 0 (count=1) should be gone.
    uint8_t evicted[32];
    make_pub(evicted, 0x10);
    peer_ledger_entry_t e;
    TEST_ASSERT(!peer_ledger_get(evicted, &e),
                "lowest-meeting-count peer was evicted");

    // Newcomer should be in the ledger with the right state.
    TEST_ASSERT(peer_ledger_get(newcomer, &e), "newcomer present");
    TEST_ASSERT(e.last_meeting_count == 999, "newcomer state recorded");
    TEST_ASSERT(e.tx_bytes_lifetime == 7,    "newcomer tx recorded");

    // A high-meeting-count peer should survive (e.g. peer 0x10+CAP-1 at
    // count=CAP is the highest seeded).
    uint8_t survivor[32];
    make_pub(survivor, (uint8_t)(0x10 + PEER_LEDGER_CAP - 1));
    TEST_ASSERT(peer_ledger_get(survivor, &e), "highest-count peer survives");
}

void test_peer_ledger_set_rep_creates_entry(void) {
    clear_state();
    uint8_t pub[32];
    make_pub(pub, 0xAA);

    int rc = peer_ledger_set_rep(pub, -5);
    TEST_ASSERT(rc == 0, "set_rep on new peer succeeds");
    TEST_ASSERT(peer_ledger_count() == 1, "creates entry on first contact");

    peer_ledger_entry_t e;
    peer_ledger_get(pub, &e);
    TEST_ASSERT(e.my_rep_score == -5, "rep score recorded");
    TEST_ASSERT(e.tx_bytes_lifetime == 0, "no byte deltas applied");
}

void test_peer_ledger_get_zeroes_out_on_miss(void) {
    clear_state();
    uint8_t pub[32];
    make_pub(pub, 0xAA);
    peer_ledger_entry_t e;
    memset(&e, 0xFF, sizeof(e));   // garbage-fill
    bool found = peer_ledger_get(pub, &e);
    TEST_ASSERT(!found, "miss returns false");
    TEST_ASSERT(e.tx_bytes_lifetime == 0, "out cleared even after garbage");
    TEST_ASSERT(e.last_meeting_count == 0, "out cleared even after garbage");
}

void test_peer_ledger_corrupted_blob_resets(void) {
    clear_state();
    // Plant garbage with the wrong magic in /system/.
    uint8_t junk[200] = {0};
    memcpy(junk, "BAD!", 4);
    mock_fs_add_system_entry("peer_ledger.bin", junk, sizeof(junk));
    peer_ledger_init();
    TEST_ASSERT(peer_ledger_count() == 0, "wrong-magic blob → empty");
}
