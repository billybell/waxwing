#include "core/ssid_scan.h"
#include "test.h"

static const uint8_t BSSID_A[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t BSSID_B[6] = {0x10, 0xAB, 0xCD, 0x01, 0x02, 0x03};
static const uint8_t BSSID_C[6] = {0x20, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB};
static const uint8_t BSSID_RANDOM[6]   = {0x06, 0x11, 0x22, 0x33, 0x44, 0x55}; // bit 1 set
static const uint8_t BSSID_LOCAL_HI[6] = {0xAA, 0x11, 0x22, 0x33, 0x44, 0x55}; // bits 1 and 7 set

void test_ssid_scan_init_empty(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    TEST_ASSERT(s.count == 0,      "fresh scan has no observations");
    TEST_ASSERT(s.scanned_ms == 0, "scanned_ms zero on init");
}

void test_ssid_scan_random_bssid_filtered(void) {
    TEST_ASSERT(ssid_scan_bssid_is_random(BSSID_RANDOM),   "0x06 detected as random");
    TEST_ASSERT(ssid_scan_bssid_is_random(BSSID_LOCAL_HI), "0xAA detected as random (bit 1)");
    TEST_ASSERT(!ssid_scan_bssid_is_random(BSSID_A),       "0x00 universally administered");
    TEST_ASSERT(!ssid_scan_bssid_is_random(BSSID_B),       "0x10 universally administered");

    ssid_scan_t s;
    ssid_scan_init(&s);
    bool added = ssid_scan_add(&s, BSSID_RANDOM, (const uint8_t*)"hotspot", 7, -50, 6);
    TEST_ASSERT(!added,        "randomized BSSID rejected");
    TEST_ASSERT(s.count == 0,  "table unchanged after rejected add");
}

void test_ssid_scan_add_basic(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    bool added = ssid_scan_add(&s, BSSID_A, (const uint8_t*)"home", 4, -42, 6);
    TEST_ASSERT(added,                       "first add succeeds");
    TEST_ASSERT(s.count == 1,                "count incremented");
    TEST_ASSERT(s.obs[0].rssi == -42,        "rssi stored");
    TEST_ASSERT(s.obs[0].channel == 6,       "channel stored");
    TEST_ASSERT(s.obs[0].ssid_len == 4,      "ssid_len stored");
    TEST_ASSERT(s.obs[0].ssid[4] == '\0',    "ssid NUL-terminated");
    TEST_ASSERT(memcmp(s.obs[0].ssid, "home", 4) == 0, "ssid bytes copied");
    TEST_ASSERT(memcmp(s.obs[0].bssid, BSSID_A, 6) == 0, "bssid bytes copied");
}

void test_ssid_scan_dedup_updates_in_place(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    ssid_scan_add(&s, BSSID_A, (const uint8_t*)"home", 4, -70, 6);
    ssid_scan_add(&s, BSSID_A, (const uint8_t*)"home", 4, -55, 11);
    TEST_ASSERT(s.count == 1,         "dedup keeps single entry");
    TEST_ASSERT(s.obs[0].rssi == -55, "rssi overwritten");
    TEST_ASSERT(s.obs[0].channel == 11, "channel overwritten");
}

void test_ssid_scan_full_replaces_weakest(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    for (uint8_t i = 0; i < SSID_SCAN_MAX; i++) {
        uint8_t b[6] = {0x00, 0x00, 0x00, 0x00, 0x00, i};
        int8_t rssi = (int8_t)(-30 - i * 10); // -30, -40, ... -100
        ssid_scan_add(&s, b, (const uint8_t*)"x", 1, rssi, 1);
    }
    TEST_ASSERT(s.count == SSID_SCAN_MAX, "table full");

    uint8_t bnew[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
    bool added = ssid_scan_add(&s, bnew, (const uint8_t*)"new", 3, -25, 1);
    TEST_ASSERT(added, "stronger entry replaces weakest");
    TEST_ASSERT(s.count == SSID_SCAN_MAX, "count stays at max");

    bool present = false;
    for (uint8_t i = 0; i < s.count; i++) {
        if (memcmp(s.obs[i].bssid, bnew, 6) == 0) present = true;
    }
    TEST_ASSERT(present, "new entry present in table");

    bool weakest_evicted = true;
    uint8_t bweak[6] = {0x00, 0x00, 0x00, 0x00, 0x00, SSID_SCAN_MAX - 1};
    for (uint8_t i = 0; i < s.count; i++) {
        if (memcmp(s.obs[i].bssid, bweak, 6) == 0) weakest_evicted = false;
    }
    TEST_ASSERT(weakest_evicted, "previous weakest entry evicted");
}

void test_ssid_scan_full_rejects_weaker(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    for (uint8_t i = 0; i < SSID_SCAN_MAX; i++) {
        uint8_t b[6] = {0x00, 0x00, 0x00, 0x00, 0x00, i};
        ssid_scan_add(&s, b, (const uint8_t*)"x", 1, -30, 1);
    }
    uint8_t bnew[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
    bool added = ssid_scan_add(&s, bnew, (const uint8_t*)"weak", 4, -85, 1);
    TEST_ASSERT(!added, "weaker entry rejected when full");
    TEST_ASSERT(s.count == SSID_SCAN_MAX, "count unchanged");
}

void test_ssid_scan_sort_by_rssi(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    ssid_scan_add(&s, BSSID_A, (const uint8_t*)"a", 1, -80, 1);
    ssid_scan_add(&s, BSSID_B, (const uint8_t*)"b", 1, -40, 6);
    ssid_scan_add(&s, BSSID_C, (const uint8_t*)"c", 1, -60, 11);
    ssid_scan_sort_by_rssi(&s);
    TEST_ASSERT(s.obs[0].rssi == -40, "strongest first");
    TEST_ASSERT(s.obs[1].rssi == -60, "middle next");
    TEST_ASSERT(s.obs[2].rssi == -80, "weakest last");
}

void test_ssid_scan_sort_stable_on_empty(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    ssid_scan_sort_by_rssi(&s); // must not crash
    TEST_ASSERT(s.count == 0, "empty sort no-op");
}

void test_ssid_scan_bssids_differ(void) {
    ssid_scan_t a, b;
    ssid_scan_init(&a);
    ssid_scan_init(&b);
    TEST_ASSERT(!ssid_scan_bssids_differ(&a, &b), "two empty scans equal");

    ssid_scan_add(&a, BSSID_A, NULL, 0, -40, 1);
    ssid_scan_add(&b, BSSID_A, NULL, 0, -90, 11);
    TEST_ASSERT(!ssid_scan_bssids_differ(&a, &b), "same BSSID set despite RSSI/channel diff");

    ssid_scan_add(&a, BSSID_B, NULL, 0, -50, 6);
    TEST_ASSERT(ssid_scan_bssids_differ(&a, &b), "extra BSSID in a flagged");

    ssid_scan_add(&b, BSSID_C, NULL, 0, -60, 6);
    TEST_ASSERT(ssid_scan_bssids_differ(&a, &b), "different second BSSID flagged");
}

void test_ssid_scan_long_ssid_truncated(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    uint8_t huge[64];
    memset(huge, 'x', sizeof(huge));
    ssid_scan_add(&s, BSSID_A, huge, 64, -40, 1);
    TEST_ASSERT(s.count == 1,                          "long ssid still added");
    TEST_ASSERT(s.obs[0].ssid_len == SSID_SCAN_SSID_MAX, "ssid_len clamped");
    TEST_ASSERT(s.obs[0].ssid[SSID_SCAN_SSID_MAX] == '\0', "NUL at clamp boundary");
}

void test_ssid_scan_hidden_ssid(void) {
    ssid_scan_t s;
    ssid_scan_init(&s);
    bool added = ssid_scan_add(&s, BSSID_A, NULL, 0, -55, 6);
    TEST_ASSERT(added,                  "hidden ssid (len=0) accepted");
    TEST_ASSERT(s.obs[0].ssid_len == 0, "ssid_len zero");
    TEST_ASSERT(s.obs[0].ssid[0] == '\0', "ssid empty NUL");
}
