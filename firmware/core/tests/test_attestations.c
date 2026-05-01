#include "core/attestations.h"
#include "mock_filestore.h"
#include "test.h"

static void setup(void) {
    mock_fs_clear();
    attestations_reset();
}

static int collect_count_cb(void *ctx, const uint8_t *blob, size_t len) {
    int *count = (int*)ctx;
    (*count)++;
    (void)blob; (void)len;
    return 0;
}

typedef struct {
    const uint8_t *blobs[ATTESTATIONS_CAP];
    size_t         lens[ATTESTATIONS_CAP];
    int            n;
} blob_collect_t;

static int collect_cb(void *ctx, const uint8_t *blob, size_t len) {
    blob_collect_t *c = (blob_collect_t*)ctx;
    c->blobs[c->n] = blob;
    c->lens[c->n]  = len;
    c->n++;
    return 0;
}

void test_attestations_init_empty(void) {
    setup();
    TEST_ASSERT(attestations_count() == 0, "fresh init has no records");
}

void test_attestations_write_one(void) {
    setup();
    uint8_t blob[120];
    memset(blob, 0xAB, sizeof(blob));
    TEST_ASSERT(attestations_write(blob, sizeof(blob)) == 0, "write succeeds");
    TEST_ASSERT(attestations_count() == 1, "count incremented");

    blob_collect_t c = {0};
    attestations_for_each(&c, collect_cb);
    TEST_ASSERT(c.n == 1,                            "iter returns one");
    TEST_ASSERT(c.lens[0] == sizeof(blob),           "length preserved");
    TEST_ASSERT(memcmp(c.blobs[0], blob, sizeof(blob)) == 0, "bytes preserved");
}

void test_attestations_too_big_rejected(void) {
    setup();
    uint8_t huge[ATTESTATIONS_SLOT_BYTES];
    memset(huge, 0xCC, sizeof(huge));
    TEST_ASSERT(attestations_write(huge, sizeof(huge)) == -1, "oversized rejected");
    TEST_ASSERT(attestations_count() == 0,                    "no record persisted");
}

void test_attestations_iter_oldest_first(void) {
    setup();
    for (int i = 0; i < 3; i++) {
        uint8_t blob[16];
        memset(blob, (uint8_t)('A' + i), sizeof(blob));
        attestations_write(blob, sizeof(blob));
    }
    blob_collect_t c = {0};
    attestations_for_each(&c, collect_cb);
    TEST_ASSERT(c.n == 3,            "iter visits 3 records");
    TEST_ASSERT(c.blobs[0][0] == 'A', "oldest first");
    TEST_ASSERT(c.blobs[2][0] == 'C', "newest last");
}

void test_attestations_ring_evicts_oldest(void) {
    setup();
    for (int i = 0; i < ATTESTATIONS_CAP + 3; i++) {
        uint8_t blob[8];
        memset(blob, (uint8_t)i, sizeof(blob));
        attestations_write(blob, sizeof(blob));
    }
    TEST_ASSERT(attestations_count() == ATTESTATIONS_CAP, "capped at ring size");

    blob_collect_t c = {0};
    attestations_for_each(&c, collect_cb);
    TEST_ASSERT(c.n == ATTESTATIONS_CAP, "iter returns full ring");
    TEST_ASSERT(c.blobs[0][0] == (uint8_t)3,
                "oldest 3 entries evicted (record #3 is now oldest)");
    TEST_ASSERT(c.blobs[ATTESTATIONS_CAP - 1][0] == (uint8_t)(ATTESTATIONS_CAP + 2),
                "newest entry preserved");
}

void test_attestations_persist_across_init(void) {
    setup();
    uint8_t blob[24];
    memset(blob, 0xEE, sizeof(blob));
    attestations_write(blob, sizeof(blob));

    attestations_init();
    TEST_ASSERT(attestations_count() == 1, "record survived re-init");

    int count = 0;
    attestations_for_each(&count, collect_count_cb);
    TEST_ASSERT(count == 1, "iter returned one record");
}
