#include "test.h"
#include "core/identity.h"
#include "core/filestore.h"
#include "core/hal_crypto.h"
#include "mock_filestore.h"
#include "stub_hal_crypto.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Helpers — write a synthetic identity blob into the mock fs
// ---------------------------------------------------------------------------

static void make_blob(uint8_t *blob, uint8_t magic0, uint8_t magic1,
                      uint8_t magic2, uint8_t magic3, uint8_t version,
                      const uint8_t seed[32], const uint8_t pub[32]) {
    blob[0] = magic0;
    blob[1] = magic1;
    blob[2] = magic2;
    blob[3] = magic3;
    blob[4] = version;
    if (seed) memcpy(&blob[5], seed, 32);
    else      memset(&blob[5], 0xAA, 32);
    if (pub)  memcpy(&blob[5 + 32], pub, 32);
    else      memset(&blob[5 + 32], 0xBB, 32);
}

// Mirror of identity.c's stub-derived pub: pub[i] = seed[i] XOR 0x5A.
static void stub_derive_pub(const uint8_t seed[32], uint8_t pub[32]) {
    for (int i = 0; i < 32; i++) pub[i] = seed[i] ^ 0x5A;
}

static void clear_state(void) {
    mock_fs_clear();
    mock_hal_reset();
}

// ---------------------------------------------------------------------------
// test_identity_generate_fresh — first boot, no blob → fresh keypair persisted
// ---------------------------------------------------------------------------

void test_identity_generate_fresh(void) {
    clear_state();
    waxwing_identity_t id;
    bool ok = waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(ok, "load_or_generate succeeds when no blob exists");
    TEST_ASSERT(mock_hal_random_call_count == 1,
                "fresh generate calls hal_random_bytes exactly once");

    // Derived fields populated.
    TEST_ASSERT(strlen(id.tpk_hex) == 64, "tpk_hex is 64 chars");
    TEST_ASSERT(strlen(id.fingerprint) == 8, "fingerprint is 8 chars");
    TEST_ASSERT(strncmp(id.node_name, "WX:", 3) == 0,
                "node_name starts with WX:");
    TEST_ASSERT(strlen(id.node_name) == 11, "node_name is 11 chars");

    // pub matches the stub derivation of seed.
    uint8_t expected_pub[32];
    stub_derive_pub(id.seed, expected_pub);
    TEST_ASSERT(memcmp(expected_pub, id.pub, 32) == 0,
                "pub == stub_derive(seed) for fresh identity");
}

// ---------------------------------------------------------------------------
// test_identity_persists_across_load — second call reads back same identity
// ---------------------------------------------------------------------------

void test_identity_persists_across_load(void) {
    clear_state();
    waxwing_identity_t a, b;
    waxwing_identity_load_or_generate(&a);
    unsigned rng_after_first = mock_hal_random_call_count;

    waxwing_identity_load_or_generate(&b);
    TEST_ASSERT(mock_hal_random_call_count == rng_after_first,
                "second load does not call the RNG");
    TEST_ASSERT(memcmp(a.seed, b.seed, 32) == 0, "seed survives reload");
    TEST_ASSERT(memcmp(a.pub,  b.pub,  32) == 0, "pub survives reload");
    TEST_ASSERT(strcmp(a.node_name, b.node_name) == 0,
                "node_name survives reload");
}

// ---------------------------------------------------------------------------
// test_identity_load_existing — pre-seeded valid blob is used as-is
// ---------------------------------------------------------------------------

void test_identity_load_existing(void) {
    clear_state();
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i + 1);
    uint8_t pub[32];
    stub_derive_pub(seed, pub);

    uint8_t blob[IDENTITY_BLOB_SIZE];
    make_blob(blob, IDENTITY_MAGIC0, IDENTITY_MAGIC1, IDENTITY_MAGIC2,
              IDENTITY_MAGIC3, IDENTITY_VERSION, seed, pub);
    mock_fs_add_system_entry(IDENTITY_BLOB_NAME, blob, sizeof(blob));

    waxwing_identity_t id;
    bool ok = waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(ok, "load existing succeeds");
    TEST_ASSERT(mock_hal_random_call_count == 0,
                "load existing does not call hal_random_bytes");
    TEST_ASSERT(memcmp(id.seed, seed, 32) == 0, "loaded seed matches");
    TEST_ASSERT(memcmp(id.pub, pub, 32) == 0, "loaded pub matches");
}

// ---------------------------------------------------------------------------
// test_identity_corrupted_pub_regenerates — pub != derive(seed) → regen
// ---------------------------------------------------------------------------

void test_identity_corrupted_pub_regenerates(void) {
    clear_state();
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i + 1);
    uint8_t bad_pub[32];
    memset(bad_pub, 0xFF, 32); // intentionally not derive(seed)

    uint8_t blob[IDENTITY_BLOB_SIZE];
    make_blob(blob, IDENTITY_MAGIC0, IDENTITY_MAGIC1, IDENTITY_MAGIC2,
              IDENTITY_MAGIC3, IDENTITY_VERSION, seed, bad_pub);
    mock_fs_add_system_entry(IDENTITY_BLOB_NAME, blob, sizeof(blob));

    waxwing_identity_t id;
    bool ok = waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(ok, "load_or_generate succeeds after pub mismatch");
    TEST_ASSERT(mock_hal_random_call_count == 1,
                "corrupted pub triggers regenerate (1 RNG call)");
    TEST_ASSERT(memcmp(id.pub, bad_pub, 32) != 0,
                "regenerated pub is not the corrupt value");
}

// ---------------------------------------------------------------------------
// test_identity_wrong_magic_regenerates — bad magic bytes → regen
// ---------------------------------------------------------------------------

void test_identity_wrong_magic_regenerates(void) {
    clear_state();
    uint8_t blob[IDENTITY_BLOB_SIZE];
    make_blob(blob, 'B', 'A', 'D', '!', IDENTITY_VERSION, NULL, NULL);
    mock_fs_add_system_entry(IDENTITY_BLOB_NAME, blob, sizeof(blob));

    waxwing_identity_t id;
    bool ok = waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(ok, "load_or_generate succeeds with bad magic");
    TEST_ASSERT(mock_hal_random_call_count == 1,
                "bad magic triggers regenerate");
}

// ---------------------------------------------------------------------------
// test_identity_wrong_version_regenerates — version != current → regen
// ---------------------------------------------------------------------------

void test_identity_wrong_version_regenerates(void) {
    clear_state();
    uint8_t blob[IDENTITY_BLOB_SIZE];
    make_blob(blob, IDENTITY_MAGIC0, IDENTITY_MAGIC1, IDENTITY_MAGIC2,
              IDENTITY_MAGIC3, (uint8_t)(IDENTITY_VERSION + 7),
              NULL, NULL);
    mock_fs_add_system_entry(IDENTITY_BLOB_NAME, blob, sizeof(blob));

    waxwing_identity_t id;
    bool ok = waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(ok, "load_or_generate succeeds with bad version");
    TEST_ASSERT(mock_hal_random_call_count == 1,
                "bad version triggers regenerate (no migration)");
}

// ---------------------------------------------------------------------------
// test_identity_short_blob_regenerates — truncated blob on disk → regen
// ---------------------------------------------------------------------------

void test_identity_short_blob_regenerates(void) {
    clear_state();
    uint8_t short_blob[10] = { 'W', 'X', 'I', 'D', IDENTITY_VERSION,
                                0, 0, 0, 0, 0 };
    mock_fs_add_system_entry(IDENTITY_BLOB_NAME, short_blob, sizeof(short_blob));

    waxwing_identity_t id;
    bool ok = waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(ok, "load_or_generate succeeds on truncated blob");
    TEST_ASSERT(mock_hal_random_call_count == 1,
                "truncated blob triggers regenerate");
}

// ---------------------------------------------------------------------------
// test_identity_node_name_format — exactly "WX:" + 8 uppercase hex chars
// ---------------------------------------------------------------------------

void test_identity_node_name_format(void) {
    clear_state();
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = 0x10;        // → pub[i] = 0x4A
    uint8_t pub[32];
    stub_derive_pub(seed, pub);

    uint8_t blob[IDENTITY_BLOB_SIZE];
    make_blob(blob, IDENTITY_MAGIC0, IDENTITY_MAGIC1, IDENTITY_MAGIC2,
              IDENTITY_MAGIC3, IDENTITY_VERSION, seed, pub);
    mock_fs_add_system_entry(IDENTITY_BLOB_NAME, blob, sizeof(blob));

    waxwing_identity_t id;
    waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(strcmp(id.node_name, "WX:4A4A4A4A") == 0,
                "node_name is WX: + uppercase hex of pub[0..3]");
    TEST_ASSERT(strcmp(id.fingerprint, "4a4a4a4a") == 0,
                "fingerprint is lowercase hex of pub[0..3]");
}

// ---------------------------------------------------------------------------
// test_identity_not_in_user_files — identity must be invisible to the BLE
// file surface. fs_list, fs_read, fs_file_size on the user namespace must
// not see the identity blob.
// ---------------------------------------------------------------------------

void test_identity_not_in_user_files(void) {
    clear_state();

    waxwing_identity_t id;
    bool ok = waxwing_identity_load_or_generate(&id);
    TEST_ASSERT(ok, "fresh identity generated");

    char names[8][FS_MAX_NAME_LEN];
    uint32_t sizes[8];
    uint8_t hashes[8][8];
    int next_off = 0;
    int n = fs_list(names, sizes, hashes, 8, 0, 8, &next_off);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT(strcmp(names[i], IDENTITY_BLOB_NAME) != 0,
                    "fs_list does not enumerate identity.bin");
    }

    uint8_t buf[128];
    TEST_ASSERT(fs_read(IDENTITY_BLOB_NAME, buf, sizeof(buf)) < 0,
                "fs_read by name cannot return identity bytes");
    TEST_ASSERT(fs_file_size(IDENTITY_BLOB_NAME) < 0,
                "fs_file_size by name cannot reveal identity");
}

// ---------------------------------------------------------------------------
// test_identity_hex_encoding — bytes_to_hex matches a known vector
// ---------------------------------------------------------------------------

void test_identity_hex_encoding(void) {
    uint8_t bytes[] = { 0x00, 0xff, 0x10, 0xa5, 0x5a, 0x01 };
    char hex[13];
    waxwing_identity_bytes_to_hex(bytes, sizeof(bytes), hex);
    TEST_ASSERT(strcmp(hex, "00ff10a55a01") == 0,
                "bytes_to_hex round-trips a known vector");
}

// ---------------------------------------------------------------------------
// test_identity_b64url_encoding — 32 zero bytes → 43 chars of "AA…"
// ---------------------------------------------------------------------------

void test_identity_b64url_encoding(void) {
    uint8_t pub[32];
    memset(pub, 0, sizeof(pub));
    char b64[44];
    waxwing_identity_tpk_to_base64url(pub, b64);
    TEST_ASSERT(strlen(b64) == 43,
                "32-byte pub encodes to 43 base64url chars (no padding)");
    for (size_t i = 0; i < 43; i++) {
        TEST_ASSERT(b64[i] == 'A',
                    "all-zero pub produces all-A base64url output");
    }
}
