#include "test.h"
#include "core/peer_table.h"
#include "core/constants.h"

#include <string.h>

// Three distinct TPK prefixes used across the tests.
static const uint8_t TPK_A[PEER_TPK_PREFIX_LEN] = { 0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA };
static const uint8_t TPK_B[PEER_TPK_PREFIX_LEN] = { 0xBB,0xBB,0xBB,0xBB,0xBB,0xBB,0xBB,0xBB };
static const uint8_t TPK_C[PEER_TPK_PREFIX_LEN] = { 0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC };

// ---------------------------------------------------------------------------
// test_peer_unknown_connects — never-seen peer always triggers a sync.
// ---------------------------------------------------------------------------
void test_peer_unknown_connects(void) {
    peer_table_init();
    TEST_ASSERT(peer_table_decide(TPK_A, 0,   1000) == PEER_DECISION_CONNECT,
                "unknown peer triggers CONNECT regardless of clock");
    TEST_ASSERT(peer_table_decide(TPK_A, 255, 9999999) == PEER_DECISION_CONNECT,
                "unknown peer triggers CONNECT for any version byte");
}

// ---------------------------------------------------------------------------
// test_peer_version_changed_connects — same peer, advertised counter
// differs from what we recorded at last sync. Triggers CONNECT even
// when the backoff window has not expired.
// ---------------------------------------------------------------------------
void test_peer_version_changed_connects(void) {
    peer_table_init();

    peer_table_record_sync(TPK_A, /*synced_version=*/5,
                           PEER_SYNC_RESULT_SUCCESS, 1000);

    // Same peer, different counter, only 100 ms later — well inside backoff.
    TEST_ASSERT(peer_table_decide(TPK_A, 6, 1100) == PEER_DECISION_CONNECT,
                "version changed → CONNECT regardless of backoff");
}

// ---------------------------------------------------------------------------
// test_peer_caught_up_skips_within_window — same peer, same counter,
// recorded recently → SKIP.
// ---------------------------------------------------------------------------
void test_peer_caught_up_skips_within_window(void) {
    peer_table_init();
    peer_table_record_sync(TPK_A, 7, PEER_SYNC_RESULT_SUCCESS, 1000);

    // 100 ms after a clean sync — backoff is 10 minutes.
    TEST_ASSERT(peer_table_decide(TPK_A, 7, 1100) == PEER_DECISION_SKIP,
                "caught-up peer inside success backoff → SKIP");
}

// ---------------------------------------------------------------------------
// test_peer_caught_up_connects_after_window — same peer, same counter,
// but enough time has passed that the backoff has expired.
// ---------------------------------------------------------------------------
void test_peer_caught_up_connects_after_window(void) {
    peer_table_init();
    peer_table_record_sync(TPK_A, 7, PEER_SYNC_RESULT_SUCCESS, 1000);

    uint32_t past = 1000 + PEER_SUCCESS_BACKOFF_MS + 1;
    TEST_ASSERT(peer_table_decide(TPK_A, 7, past) == PEER_DECISION_CONNECT,
                "caught-up peer past success backoff → CONNECT");
}

// ---------------------------------------------------------------------------
// test_peer_failed_uses_short_backoff — a failed sync uses
// PEER_FAILED_BACKOFF_MS (much shorter than the success window).
// ---------------------------------------------------------------------------
void test_peer_failed_uses_short_backoff(void) {
    peer_table_init();
    peer_table_record_sync(TPK_A, 9, PEER_SYNC_RESULT_FAILED, 1000);

    // Inside short backoff: SKIP.
    TEST_ASSERT(peer_table_decide(TPK_A, 9, 1000 + PEER_FAILED_BACKOFF_MS - 1)
                == PEER_DECISION_SKIP,
                "failed peer inside failed-backoff → SKIP");
    // Past short backoff: CONNECT.
    TEST_ASSERT(peer_table_decide(TPK_A, 9, 1000 + PEER_FAILED_BACKOFF_MS + 1)
                == PEER_DECISION_CONNECT,
                "failed peer past failed-backoff → CONNECT");
    // Crucially: the long success backoff has NOT elapsed at this
    // point, but a failed entry should still be retried sooner.
    TEST_ASSERT(PEER_FAILED_BACKOFF_MS < PEER_SUCCESS_BACKOFF_MS,
                "failed backoff is shorter than success backoff "
                "(constants sanity)");
}

// ---------------------------------------------------------------------------
// test_peer_wraparound_inequality_triggers_sync — bump a recorded
// peer's version through a full 256-step cycle. Every step that lands
// on a value different from the recorded one MUST trigger CONNECT.
// Catches the obvious `>` regression — under wrap it would falsely
// SKIP about half the values.
// ---------------------------------------------------------------------------
void test_peer_wraparound_inequality_triggers_sync(void) {
    peer_table_init();
    peer_table_record_sync(TPK_A, /*v=*/0, PEER_SYNC_RESULT_SUCCESS, 1000);

    // Stay well inside the backoff window throughout — that way the
    // *only* signal that distinguishes CONNECT from SKIP is the
    // version-equality check.
    uint32_t now = 1100;

    int connects = 0;
    for (int v = 1; v <= 256; v++) {
        uint8_t advertised = (uint8_t)v;     // wraps at 256 → 0
        peer_decision_t d = peer_table_decide(TPK_A, advertised, now);

        if (advertised == 0) {
            // The 256th iteration: `advertised` wraps to 0, the
            // recorded version. Equal → SKIP is correct.
            TEST_ASSERT(d == PEER_DECISION_SKIP,
                        "wrap back to recorded value → SKIP "
                        "(no false CONNECT on equality)");
        } else {
            TEST_ASSERT(d == PEER_DECISION_CONNECT,
                        "any non-equal advertised value within backoff "
                        "→ CONNECT (catches `>` regression)");
            if (d == PEER_DECISION_CONNECT) connects++;
        }
    }
    TEST_ASSERT(connects == 255,
                "exactly 255 of 256 advertised values trigger CONNECT");
}

// ---------------------------------------------------------------------------
// test_peer_lru_evicts_oldest — fill the table, add one more, the
// oldest entry should be gone and the newest plus the survivors should
// all still resolve.
// ---------------------------------------------------------------------------
void test_peer_lru_evicts_oldest(void) {
    peer_table_init();

    uint8_t prefix[PEER_TPK_PREFIX_LEN];
    // Fill with PEER_TABLE_CAP synthetic peers, each with timestamp i.
    // i==0 will be the LRU once we add one more.
    for (int i = 0; i < PEER_TABLE_CAP; i++) {
        memset(prefix, (uint8_t)(0x10 + i), PEER_TPK_PREFIX_LEN);
        peer_table_record_sync(prefix, /*v=*/0, PEER_SYNC_RESULT_SUCCESS,
                               (uint32_t)(1000 + i));
    }
    TEST_ASSERT(peer_table_size() == PEER_TABLE_CAP,
                "table is full after PEER_TABLE_CAP insertions");

    // One more peer — evicts whichever has the smallest timestamp, i.e. i==0.
    memset(prefix, 0xFF, PEER_TPK_PREFIX_LEN);
    peer_table_record_sync(prefix, /*v=*/42, PEER_SYNC_RESULT_SUCCESS,
                           /*now=*/9999);
    TEST_ASSERT(peer_table_size() == PEER_TABLE_CAP,
                "table size stays at cap after eviction");

    // The original oldest (0x10..) should now miss → CONNECT.
    uint8_t oldest[PEER_TPK_PREFIX_LEN];
    memset(oldest, 0x10, PEER_TPK_PREFIX_LEN);
    TEST_ASSERT(peer_table_decide(oldest, 0, 9999) == PEER_DECISION_CONNECT,
                "evicted oldest peer is treated as never-seen");

    // The newcomer is recorded → SKIP at same version, same time-ish.
    TEST_ASSERT(peer_table_decide(prefix, 42, 9999) == PEER_DECISION_SKIP,
                "newcomer survives; SKIP at recorded version");

    // A peer in the middle (e.g. i==15) should still be remembered.
    uint8_t middle[PEER_TPK_PREFIX_LEN];
    memset(middle, 0x10 + 15, PEER_TPK_PREFIX_LEN);
    TEST_ASSERT(peer_table_decide(middle, 0, 1000 + 15 + 1)
                == PEER_DECISION_SKIP,
                "non-evicted entry survives eviction event");
}

// ---------------------------------------------------------------------------
// test_peer_record_updates_in_place — re-recording the same TPK
// updates its slot rather than creating a new one.
// ---------------------------------------------------------------------------
void test_peer_record_updates_in_place(void) {
    peer_table_init();
    peer_table_record_sync(TPK_A, 1, PEER_SYNC_RESULT_SUCCESS, 1000);
    peer_table_record_sync(TPK_B, 2, PEER_SYNC_RESULT_SUCCESS, 2000);
    TEST_ASSERT(peer_table_size() == 2, "two distinct peers → size 2");

    // Re-record TPK_A with a different version and a later timestamp.
    peer_table_record_sync(TPK_A, 99, PEER_SYNC_RESULT_FAILED, 3000);
    TEST_ASSERT(peer_table_size() == 2,
                "re-recording an existing TPK does not add a new slot");

    // Now decide for TPK_A: same version → SKIP only if backoff applies.
    // The recorded version is 99 and the result is FAILED; same version
    // 50 ms later should SKIP (within failed backoff).
    TEST_ASSERT(peer_table_decide(TPK_A, 99, 3050) == PEER_DECISION_SKIP,
                "in-place update applied: same version, recent time → SKIP");
    // Different version → CONNECT.
    TEST_ASSERT(peer_table_decide(TPK_A, 100, 3050) == PEER_DECISION_CONNECT,
                "in-place update applied: changed version → CONNECT");
}

// ---------------------------------------------------------------------------
// test_peer_distinct_tpks_isolated — two peers with different prefixes
// don't interfere with each other.
// ---------------------------------------------------------------------------
void test_peer_distinct_tpks_isolated(void) {
    peer_table_init();
    peer_table_record_sync(TPK_A, 5, PEER_SYNC_RESULT_SUCCESS, 1000);
    peer_table_record_sync(TPK_B, 5, PEER_SYNC_RESULT_FAILED,  1000);

    // Same versions, very recent — both should SKIP, but for different reasons.
    TEST_ASSERT(peer_table_decide(TPK_A, 5, 1100) == PEER_DECISION_SKIP,
                "TPK_A: success-backoff SKIP");
    TEST_ASSERT(peer_table_decide(TPK_B, 5, 1100) == PEER_DECISION_SKIP,
                "TPK_B: failed-backoff SKIP");

    // After PEER_FAILED_BACKOFF_MS, TPK_B retries; TPK_A still skips.
    uint32_t now = 1000 + PEER_FAILED_BACKOFF_MS + 10;
    TEST_ASSERT(peer_table_decide(TPK_A, 5, now) == PEER_DECISION_SKIP,
                "long-backoff peer still SKIPs after short-backoff expired");
    TEST_ASSERT(peer_table_decide(TPK_B, 5, now) == PEER_DECISION_CONNECT,
                "short-backoff peer retries after its window expired");

    // TPK_C never seen → CONNECT regardless.
    TEST_ASSERT(peer_table_decide(TPK_C, 5, now) == PEER_DECISION_CONNECT,
                "third peer unaffected by either entry");
}

// ---------------------------------------------------------------------------
// test_peer_init_clears — a second init wipes the table.
// ---------------------------------------------------------------------------
void test_peer_init_clears(void) {
    peer_table_init();
    peer_table_record_sync(TPK_A, 1, PEER_SYNC_RESULT_SUCCESS, 1000);
    TEST_ASSERT(peer_table_size() == 1, "one entry after record");
    peer_table_init();
    TEST_ASSERT(peer_table_size() == 0, "init clears the table");
    TEST_ASSERT(peer_table_decide(TPK_A, 1, 1100) == PEER_DECISION_CONNECT,
                "previously-known peer is unknown after init");
}
