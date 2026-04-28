#include "test.h"
#include "core/manifest_counter.h"
#include "core/filestore.h"
#include "mock_filestore.h"

// The real persistence path is fs_system_*. The mock filestore exposes
// mock_fs_add_system_entry to seed those, and the production code under
// test calls fs_system_read/write/delete which the mock implements. So
// these tests exercise the actual write-through path, not a stubbed one.

#define BLOB_NAME "manifest_version.bin"

static void clear_state(void) {
    mock_fs_clear();
    manifest_counter_init();    // re-load from (now empty) mock fs → 0
}

// ---------------------------------------------------------------------------
// test_counter_starts_at_zero — fresh device, no persisted blob.
// ---------------------------------------------------------------------------
void test_counter_starts_at_zero(void) {
    clear_state();
    TEST_ASSERT(manifest_counter_get() == 0,
                "fresh device: counter starts at 0");
}

// ---------------------------------------------------------------------------
// test_counter_persists — bump, reinit, value survives.
// ---------------------------------------------------------------------------
void test_counter_persists(void) {
    clear_state();
    manifest_counter_bump();
    manifest_counter_bump();
    manifest_counter_bump();
    TEST_ASSERT(manifest_counter_get() == 3, "three bumps land at 3");

    // Simulate a reboot: re-init reads the persisted byte back in.
    manifest_counter_init();
    TEST_ASSERT(manifest_counter_get() == 3,
                "counter survives re-init via /system/ blob");
}

// ---------------------------------------------------------------------------
// test_counter_wraps_at_256 — 256 bumps from 0 → back to 0.
// ---------------------------------------------------------------------------
void test_counter_wraps_at_256(void) {
    clear_state();
    for (int i = 0; i < 256; i++) {
        manifest_counter_bump();
    }
    TEST_ASSERT(manifest_counter_get() == 0,
                "256 bumps wrap back to 0 (uint8_t intentional)");

    manifest_counter_bump();
    TEST_ASSERT(manifest_counter_get() == 1,
                "wrap + 1 lands at 1, no off-by-one");
}

// ---------------------------------------------------------------------------
// test_counter_bump_writes_through — bump persists in the same call.
// ---------------------------------------------------------------------------
void test_counter_bump_writes_through(void) {
    clear_state();
    manifest_counter_bump();
    manifest_counter_bump();

    // Don't go through manifest_counter_init; read the persisted byte
    // directly to confirm the bump wrote through, not just that the
    // module's internal cache changed.
    uint8_t persisted = 0xFF;
    int n = fs_system_read(BLOB_NAME, &persisted, sizeof(persisted));
    TEST_ASSERT(n == 1, "bump persisted exactly 1 byte");
    TEST_ASSERT(persisted == 2, "persisted value matches latest bump");
}

// ---------------------------------------------------------------------------
// test_counter_load_corrupted_blob — wrong-size persisted blob falls
// back to 0 cleanly. A peer that sees a counter of 0 just treats it as a
// version it has never recorded and connects, which is the right behaviour.
// ---------------------------------------------------------------------------
void test_counter_load_corrupted_blob(void) {
    mock_fs_clear();

    // Seed an absurd 12-byte blob at the system path — must not crash
    // and must not be trusted.
    uint8_t junk[12] = { 'X','X','X','X','X','X','X','X','X','X','X','X' };
    mock_fs_add_system_entry(BLOB_NAME, junk, sizeof(junk));

    manifest_counter_init();
    TEST_ASSERT(manifest_counter_get() == 0,
                "corrupted blob → counter resets to 0");
}

// ---------------------------------------------------------------------------
// test_counter_init_idempotent — calling init twice in a row reads
// flash twice and arrives at the same value. Catches a regression where
// init mutated the persisted blob.
// ---------------------------------------------------------------------------
void test_counter_init_idempotent(void) {
    clear_state();
    manifest_counter_bump();
    manifest_counter_bump();
    manifest_counter_bump();    // counter now 3, persisted

    manifest_counter_init();
    manifest_counter_init();
    manifest_counter_init();
    TEST_ASSERT(manifest_counter_get() == 3,
                "repeated init reads same value, doesn't mutate flash");
}
