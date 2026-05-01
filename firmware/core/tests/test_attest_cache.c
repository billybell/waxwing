#include "core/attest_cache.h"
#include "mock_filestore.h"
#include "test.h"

static void setup(void) {
    mock_fs_clear();
    attest_cache_reset();
}

static int collect_count_cb(void *ctx, const uint8_t *blob, size_t len) {
    int *count = (int*)ctx;
    (*count)++;
    (void)blob; (void)len;
    return 0;
}

typedef struct {
    const uint8_t *blobs[ATTEST_CACHE_CAP];
    size_t         lens[ATTEST_CACHE_CAP];
    int            n;
} blob_collect_t;

static int collect_cb(void *ctx, const uint8_t *blob, size_t len) {
    blob_collect_t *c = (blob_collect_t*)ctx;
    c->blobs[c->n] = blob;
    c->lens[c->n]  = len;
    c->n++;
    return 0;
}

void test_attest_cache_init_empty(void) {
    setup();
    TEST_ASSERT(attest_cache_count() == 0, "fresh init has no records");
}

void test_attest_cache_write_one(void) {
    setup();
    uint8_t blob[120];
    memset(blob, 0xAB, sizeof(blob));
    TEST_ASSERT(attest_cache_write(blob, sizeof(blob)) == 1, "write returns inserted");
    TEST_ASSERT(attest_cache_count() == 1,                   "count incremented");

    blob_collect_t c = {0};
    attest_cache_for_each(&c, collect_cb);
    TEST_ASSERT(c.n == 1,                            "iter returns one");
    TEST_ASSERT(c.lens[0] == sizeof(blob),           "length preserved");
    TEST_ASSERT(memcmp(c.blobs[0], blob, sizeof(blob)) == 0, "bytes preserved");
}

void test_attest_cache_dedup_returns_zero(void) {
    setup();
    uint8_t a[80], b[80];
    memset(a, 0x11, sizeof(a));
    memset(b, 0x22, sizeof(b));

    TEST_ASSERT(attest_cache_write(a, sizeof(a)) == 1, "first insert wins");
    TEST_ASSERT(attest_cache_write(a, sizeof(a)) == 0, "exact-dup rejected");
    TEST_ASSERT(attest_cache_count() == 1,             "count not bumped");

    TEST_ASSERT(attest_cache_write(b, sizeof(b)) == 1, "different blob accepted");
    TEST_ASSERT(attest_cache_count() == 2,             "count now 2");

    // Length differs — must not match the longer blob's prefix.
    uint8_t a_short[40];
    memset(a_short, 0x11, sizeof(a_short));
    TEST_ASSERT(attest_cache_write(a_short, sizeof(a_short)) == 1,
                "same-bytes-different-length is not a dup");
    TEST_ASSERT(attest_cache_count() == 3, "length-mismatched blob inserted");
}

void test_attest_cache_too_big_rejected(void) {
    setup();
    uint8_t huge[ATTEST_CACHE_SLOT_BYTES];
    memset(huge, 0xCC, sizeof(huge));
    TEST_ASSERT(attest_cache_write(huge, sizeof(huge)) == -1, "oversized rejected");
    TEST_ASSERT(attest_cache_count() == 0,                    "no record persisted");
}

void test_attest_cache_iter_oldest_first(void) {
    setup();
    for (int i = 0; i < 3; i++) {
        uint8_t blob[16];
        memset(blob, (uint8_t)('A' + i), sizeof(blob));
        attest_cache_write(blob, sizeof(blob));
    }
    blob_collect_t c = {0};
    attest_cache_for_each(&c, collect_cb);
    TEST_ASSERT(c.n == 3,             "iter visits 3 records");
    TEST_ASSERT(c.blobs[0][0] == 'A', "oldest first");
    TEST_ASSERT(c.blobs[2][0] == 'C', "newest last");
}

void test_attest_cache_ring_evicts_oldest(void) {
    setup();
    for (int i = 0; i < ATTEST_CACHE_CAP + 3; i++) {
        uint8_t blob[8];
        memset(blob, (uint8_t)i, sizeof(blob));
        attest_cache_write(blob, sizeof(blob));
    }
    TEST_ASSERT(attest_cache_count() == ATTEST_CACHE_CAP, "capped at ring size");

    blob_collect_t c = {0};
    attest_cache_for_each(&c, collect_cb);
    TEST_ASSERT(c.n == ATTEST_CACHE_CAP, "iter returns full ring");
    TEST_ASSERT(c.blobs[0][0] == (uint8_t)3,
                "oldest 3 entries evicted (record #3 is now oldest)");
    TEST_ASSERT(c.blobs[ATTEST_CACHE_CAP - 1][0] == (uint8_t)(ATTEST_CACHE_CAP + 2),
                "newest entry preserved");
}

void test_attest_cache_dedup_after_eviction(void) {
    setup();
    uint8_t target[8];
    memset(target, 0x55, sizeof(target));

    // Insert target, then push enough other blobs to evict it.
    TEST_ASSERT(attest_cache_write(target, sizeof(target)) == 1, "target inserted");
    for (int i = 0; i < ATTEST_CACHE_CAP; i++) {
        uint8_t blob[8];
        memset(blob, (uint8_t)(0x80 + i), sizeof(blob));
        attest_cache_write(blob, sizeof(blob));
    }
    // Target is no longer in the ring; re-presenting it should insert
    // (not dedup), because dedup only checks live slots.
    TEST_ASSERT(attest_cache_write(target, sizeof(target)) == 1,
                "evicted blob can be re-inserted");
}

void test_attest_cache_persist_across_init(void) {
    setup();
    uint8_t blob[24];
    memset(blob, 0xEE, sizeof(blob));
    attest_cache_write(blob, sizeof(blob));

    attest_cache_init();
    TEST_ASSERT(attest_cache_count() == 1, "record survived re-init");

    int count = 0;
    attest_cache_for_each(&count, collect_count_cb);
    TEST_ASSERT(count == 1, "iter returned one record");
}
