#include "test.h"
#include "core/meeting_count.h"
#include "core/filestore.h"
#include "mock_filestore.h"

// As with manifest_counter, the mock filestore implements fs_system_*
// directly so these tests exercise the real persistence path — no
// stubbing of meeting_count itself.

#define BLOB_NAME "meeting_count.bin"

static void clear_state(void) {
    mock_fs_clear();
    meeting_count_init();   // reload from now-empty mock → 0
}

void test_meeting_count_starts_at_zero(void) {
    clear_state();
    TEST_ASSERT(meeting_count_get() == 0,
                "fresh device: counter starts at 0");
}

void test_meeting_count_bump_returns_new_value(void) {
    clear_state();
    TEST_ASSERT(meeting_count_bump() == 1, "first bump returns 1");
    TEST_ASSERT(meeting_count_bump() == 2, "second bump returns 2");
    TEST_ASSERT(meeting_count_bump() == 3, "third bump returns 3");
    TEST_ASSERT(meeting_count_get()  == 3, "get matches last bump");
}

void test_meeting_count_persists_across_init(void) {
    clear_state();
    for (int i = 0; i < 17; i++) meeting_count_bump();
    TEST_ASSERT(meeting_count_get() == 17, "17 bumps land at 17");

    // Simulate reboot: zero RAM cache, re-init reads from /system/.
    meeting_count_reset();
    TEST_ASSERT(meeting_count_get() == 0, "reset clears in-RAM cache");
    meeting_count_init();
    TEST_ASSERT(meeting_count_get() == 17,
                "value survives reboot via /system/ blob");
}

void test_meeting_count_corrupted_blob_resets(void) {
    clear_state();
    // Write a malformed (wrong-sized) blob to the system store.
    uint8_t junk[3] = {0xDE, 0xAD, 0xBE};
    mock_fs_add_system_entry(BLOB_NAME, junk, sizeof(junk));
    meeting_count_init();
    TEST_ASSERT(meeting_count_get() == 0,
                "corrupted (wrong-sized) blob is treated as fresh");
}

void test_meeting_count_does_not_wrap_uint64_max(void) {
    clear_state();
    // Seed the on-disk state to UINT64_MAX so the next bump would
    // overflow. encode 0xFF * 8.
    uint8_t seed[8];
    for (int i = 0; i < 8; i++) seed[i] = 0xFF;
    mock_fs_add_system_entry(BLOB_NAME, seed, sizeof(seed));
    meeting_count_init();
    TEST_ASSERT(meeting_count_get() == UINT64_MAX,
                "loaded saturated value");

    uint64_t after = meeting_count_bump();
    TEST_ASSERT(after == UINT64_MAX,
                "bump at UINT64_MAX saturates instead of wrapping");
}

void test_meeting_count_persisted_bytes_are_le(void) {
    clear_state();
    // Bump to a value with distinct bytes per position (0x0807060504030201).
    // Easier: seed the cache, bump once, read the on-disk bytes back.
    // We seed via mock_fs and then init.
    uint8_t seed[8] = {0x00, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00};
    // value = 0x00 01 02 03 04 05 06 00 (LE) = 0x0001020304050600
    mock_fs_add_system_entry(BLOB_NAME, seed, sizeof(seed));
    meeting_count_init();
    TEST_ASSERT(meeting_count_get() == 0x0001020304050600ULL,
                "le decode matches expected");

    // Bump → write back. Verify the persisted bytes are LE of (value+1).
    uint64_t after = meeting_count_bump();

    uint8_t persisted[8];
    int n = fs_system_read(BLOB_NAME, persisted, sizeof(persisted));
    TEST_ASSERT(n == 8, "persisted blob is exactly 8 bytes");

    uint64_t reread = 0;
    for (int i = 0; i < 8; i++) {
        reread |= ((uint64_t)persisted[i]) << (i * 8);
    }
    TEST_ASSERT(reread == after,
                "persisted bytes round-trip via LE decode");
}
