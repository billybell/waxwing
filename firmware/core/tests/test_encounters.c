#include "core/encounters.h"
#include "core/cbor_decode.h"
#include "core/ssid_scan.h"
#include "mock_filestore.h"
#include "stub_hal_crypto.h"
#include "test.h"

static const uint8_t NODE_PUB[32] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
};
static const uint8_t SEED[32] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
};

static const uint8_t BSSID_A[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t BSSID_B[6] = {0x10, 0xAB, 0xCD, 0xEF, 0x01, 0x02};
static const uint8_t BSSID_C[6] = {0x20, 0x33, 0x44, 0x55, 0x66, 0x77};

static void make_scan(ssid_scan_t *s, const uint8_t **bssids, uint8_t n) {
    ssid_scan_init(s);
    for (uint8_t i = 0; i < n; i++) {
        ssid_scan_add(s, bssids[i], (const uint8_t*)"x", 1, (int8_t)(-40 - i), 6);
    }
}

static void encounters_test_setup(void) {
    mock_fs_clear();
    mock_hal_reset();
    encounters_reset();
}

void test_encounters_init_empty(void) {
    encounters_test_setup();
    TEST_ASSERT(encounters_count() == 0, "fresh init has no records");
}

void test_encounters_record_one(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list[] = {BSSID_A, BSSID_B};
    make_scan(&s, list, 2);
    int rc = encounters_record(&s, 1234, NODE_PUB, SEED);
    TEST_ASSERT(rc == 0, "record succeeds");
    TEST_ASSERT(encounters_count() == 1, "count incremented");
}

static int collect_cb(void *ctx, const uint8_t *cbor, size_t len) {
    int *count = (int*)ctx;
    (*count)++;
    (void)cbor; (void)len;
    return 0;
}

void test_encounters_iter_oldest_first(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list_a[] = {BSSID_A};
    const uint8_t *list_b[] = {BSSID_B};
    const uint8_t *list_c[] = {BSSID_C};
    make_scan(&s, list_a, 1); encounters_record(&s, 100, NODE_PUB, SEED);
    make_scan(&s, list_b, 1); encounters_record(&s, 200, NODE_PUB, SEED);
    make_scan(&s, list_c, 1); encounters_record(&s, 300, NODE_PUB, SEED);

    int count = 0;
    int seen = encounters_for_each(&count, collect_cb);
    TEST_ASSERT(seen == 3,  "iterated all three");
    TEST_ASSERT(count == 3, "callback fired three times");
}

typedef struct { uint32_t times[ENCOUNTERS_CAP]; int n; } time_collect_t;
static int time_cb(void *ctx, const uint8_t *cbor, size_t len) {
    time_collect_t *t = (time_collect_t*)ctx;
    cbor_item_t map;
    if (!cbor_parse(cbor, cbor + len, &map)) return 1;
    if (map.type != CBOR_TYPE_MAP) return 1;
    cbor_item_t v;
    char key[2] = {(char)0x03, 0};
    // Records use integer keys, not text; cbor_map_get_uint expects text keys.
    // Walk the map manually instead.
    const uint8_t *p = map.data;
    for (uint64_t i = 0; i < map.arg; i++) {
        cbor_item_t k;
        if (!cbor_parse(p, cbor + len, &k)) return 1;
        cbor_item_t val;
        if (!cbor_parse(k.next, cbor + len, &val)) return 1;
        if (k.type == CBOR_TYPE_UINT && k.arg == 3) {
            t->times[t->n++] = (uint32_t)val.arg;
        }
        p = val.next;
    }
    (void)key;
    (void)v;
    return 0;
}

void test_encounters_iter_order_preserved(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list_a[] = {BSSID_A};
    const uint8_t *list_b[] = {BSSID_B};
    const uint8_t *list_c[] = {BSSID_C};
    make_scan(&s, list_a, 1); encounters_record(&s, 100, NODE_PUB, SEED);
    make_scan(&s, list_b, 1); encounters_record(&s, 200, NODE_PUB, SEED);
    make_scan(&s, list_c, 1); encounters_record(&s, 300, NODE_PUB, SEED);

    time_collect_t collect = {0};
    encounters_for_each(&collect, time_cb);
    TEST_ASSERT(collect.n == 3,           "extracted 3 timestamps");
    TEST_ASSERT(collect.times[0] == 100,  "oldest first");
    TEST_ASSERT(collect.times[1] == 200,  "middle next");
    TEST_ASSERT(collect.times[2] == 300,  "newest last");
}

void test_encounters_ring_evicts_oldest(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list_a[] = {BSSID_A};
    for (uint32_t i = 0; i < ENCOUNTERS_CAP + 5; i++) {
        // Vary BSSID so dedup never fires (or just don't use should_record).
        uint8_t b[6] = {0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)i};
        const uint8_t *list[] = {b};
        make_scan(&s, list, 1);
        // Override slot 0 to BSSID_A so we can detect it didn't survive.
        if (i == 0) make_scan(&s, list_a, 1);
        encounters_record(&s, 1000 + i, NODE_PUB, SEED);
    }
    TEST_ASSERT(encounters_count() == ENCOUNTERS_CAP, "count capped at ring size");

    time_collect_t collect = {0};
    encounters_for_each(&collect, time_cb);
    TEST_ASSERT(collect.n == ENCOUNTERS_CAP, "iter returned full ring");
    TEST_ASSERT(collect.times[0] > 1000,     "oldest entry was evicted");
    TEST_ASSERT(collect.times[ENCOUNTERS_CAP - 1] == 1000 + ENCOUNTERS_CAP + 4,
                "newest entry preserved");
}

void test_encounters_persists_across_init(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list[] = {BSSID_A, BSSID_B};
    make_scan(&s, list, 2);
    encounters_record(&s, 555, NODE_PUB, SEED);

    // Simulate reboot: wipe the in-RAM cache, then re-init from /system/.
    encounters_init();
    TEST_ASSERT(encounters_count() == 1, "record survived reboot");

    time_collect_t collect = {0};
    encounters_for_each(&collect, time_cb);
    TEST_ASSERT(collect.n == 1,          "iter returned one record");
    TEST_ASSERT(collect.times[0] == 555, "captured_at_ms preserved");
}

void test_encounters_should_record_first(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list[] = {BSSID_A};
    make_scan(&s, list, 1);
    TEST_ASSERT(encounters_should_record(&s, 5000, 600000),
                "first scan always records");
}

void test_encounters_should_record_dedup_same_set(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list[] = {BSSID_A, BSSID_B};
    make_scan(&s, list, 2);
    encounters_record(&s, 1000, NODE_PUB, SEED);

    ssid_scan_t s2;
    make_scan(&s2, list, 2);
    TEST_ASSERT(!encounters_should_record(&s2, 1500, 600000),
                "same BSSID set within window: skip");
}

void test_encounters_should_record_dedup_diff_set(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list1[] = {BSSID_A};
    const uint8_t *list2[] = {BSSID_A, BSSID_C};
    make_scan(&s, list1, 1);
    encounters_record(&s, 1000, NODE_PUB, SEED);

    ssid_scan_t s2;
    make_scan(&s2, list2, 2);
    TEST_ASSERT(encounters_should_record(&s2, 1500, 600000),
                "different BSSID set: record");
}

void test_encounters_should_record_age_bypass(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list[] = {BSSID_A, BSSID_B};
    make_scan(&s, list, 2);
    encounters_record(&s, 1000, NODE_PUB, SEED);

    ssid_scan_t s2;
    make_scan(&s2, list, 2);
    TEST_ASSERT(!encounters_should_record(&s2, 1000 + 599999, 600000),
                "just under age threshold: skip");
    TEST_ASSERT(encounters_should_record(&s2, 1000 + 600000, 600000),
                "at age threshold: record");
}

void test_encounters_record_signs_with_seed(void) {
    encounters_test_setup();
    ssid_scan_t s;
    const uint8_t *list[] = {BSSID_A};
    make_scan(&s, list, 1);
    int before = mock_hal_random_call_count;
    int rc = encounters_record(&s, 100, NODE_PUB, SEED);
    TEST_ASSERT(rc == 0, "record signs and persists");
    TEST_ASSERT(mock_hal_random_call_count == before,
                "no RNG call (Ed25519 sign is deterministic from seed)");
}

void test_encounters_empty_scan_rejected(void) {
    encounters_test_setup();
    ssid_scan_t s;
    ssid_scan_init(&s); // count = 0
    int rc = encounters_record(&s, 100, NODE_PUB, SEED);
    TEST_ASSERT(rc == -1,                "empty scan rejected");
    TEST_ASSERT(encounters_count() == 0, "no record persisted");
}
