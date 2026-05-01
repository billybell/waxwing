#include "core/attest_query.h"

#include <string.h>

#include "core/attest_cache.h"
#include "core/attestations.h"
#include "core/cborencode.h"
#include "mock_filestore.h"
#include "test.h"

// Build a wire-format attestation blob matching what the companion writes:
// map(7) with int keys 1..7 — version, author, captured_at, bssids,
// geohash, source, signature.
static size_t build_blob(uint8_t *out,
                         const uint8_t *bssids, int bssid_count,
                         const char *geohash, const char *source) {
    uint8_t *p = out;
    uint8_t  author[32]; memset(author, 0xAA, 32);
    uint8_t  sig[64];    memset(sig,    0xBB, 64);

    p += cborencode_map_header(p, 7);
    p += cborencode_uint(p, 1); p += cborencode_uint(p, 1);
    p += cborencode_uint(p, 2); p += cborencode_byte_str(p, author, 32);
    p += cborencode_uint(p, 3); p += cborencode_uint(p, 1700000000u);
    p += cborencode_uint(p, 4); p += cborencode_array_header(p, bssid_count);
    for (int i = 0; i < bssid_count; i++) {
        p += cborencode_byte_str(p, bssids + i * 6, 6);
    }
    p += cborencode_uint(p, 5); p += cborencode_text_str(p, geohash, strlen(geohash));
    p += cborencode_uint(p, 6); p += cborencode_text_str(p, source, strlen(source));
    p += cborencode_uint(p, 7); p += cborencode_byte_str(p, sig, 64);
    return (size_t)(p - out);
}

static void setup_rings(void) {
    mock_fs_clear();
    attestations_reset();
    attest_cache_reset();
}

void test_attest_extract_bssids_basic(void) {
    uint8_t bssids[3][6] = {
        {0x00,0x01,0x02,0x03,0x04,0x05},
        {0x10,0x11,0x12,0x13,0x14,0x15},
        {0x20,0x21,0x22,0x23,0x24,0x25},
    };
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)bssids, 3, "9q8yyk7", "self");

    uint8_t out[8][6];
    int n = -1;
    bool ok = attest_extract_bssids(blob, len, out, 8, &n);
    TEST_ASSERT(ok,                                "well-formed blob parses");
    TEST_ASSERT(n == 3,                            "three bssids returned");
    TEST_ASSERT(memcmp(out[0], bssids[0], 6) == 0, "bssid 0 preserved");
    TEST_ASSERT(memcmp(out[2], bssids[2], 6) == 0, "bssid 2 preserved");
}

void test_attest_extract_bssids_caps_at_max(void) {
    uint8_t bssids[5][6];
    for (int i = 0; i < 5; i++) memset(bssids[i], (uint8_t)i, 6);
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)bssids, 5, "abc1234", "self");

    uint8_t out[3][6];
    int n = -1;
    bool ok = attest_extract_bssids(blob, len, out, 3, &n);
    TEST_ASSERT(ok,             "parse ok");
    TEST_ASSERT(n == 3,         "capped at max=3");
    TEST_ASSERT(out[0][0] == 0, "first BSSID is index 0");
    TEST_ASSERT(out[2][0] == 2, "third BSSID is index 2");
}

void test_attest_extract_bssids_skips_wrong_size(void) {
    // Hand-build a record where the bssid array contains mixed sizes.
    uint8_t blob[256], *p = blob;
    p += cborencode_map_header(p, 1);
    p += cborencode_uint(p, 4);
    p += cborencode_array_header(p, 4);
    p += cborencode_byte_str(p, (const uint8_t*)"\x01\x02\x03\x04\x05\x06", 6);
    p += cborencode_byte_str(p, (const uint8_t*)"\x99",                       1);
    p += cborencode_byte_str(p, (const uint8_t*)"\x07\x08\x09\x0A\x0B\x0C", 6);
    p += cborencode_byte_str(p, (const uint8_t*)"\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 7);

    uint8_t out[8][6];
    int n = -1;
    bool ok = attest_extract_bssids(blob, (size_t)(p - blob), out, 8, &n);
    TEST_ASSERT(ok,                "parse ok despite mixed sizes");
    TEST_ASSERT(n == 2,            "only 6-byte entries returned");
    TEST_ASSERT(out[0][0] == 0x01, "first valid entry preserved");
    TEST_ASSERT(out[1][0] == 0x07, "second valid entry preserved");
}

void test_attest_extract_bssids_missing_key4(void) {
    uint8_t blob[64], *p = blob;
    p += cborencode_map_header(p, 2);
    p += cborencode_uint(p, 1); p += cborencode_uint(p, 1);
    p += cborencode_uint(p, 5); p += cborencode_text_str(p, "abc", 3);

    uint8_t out[3][6];
    int n = -1;
    bool ok = attest_extract_bssids(blob, (size_t)(p - blob), out, 3, &n);
    TEST_ASSERT(!ok,    "missing key 4 returns false");
    TEST_ASSERT(n == 0, "out_count zeroed on failure");
}

void test_attest_extract_bssids_truncated_blob(void) {
    uint8_t bssids[2][6] = {{1,2,3,4,5,6},{7,8,9,10,11,12}};
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)bssids, 2, "9q8", "self");

    uint8_t out[3][6];
    int n = -1;
    bool ok = attest_extract_bssids(blob, 8, out, 3, &n); // truncated
    TEST_ASSERT(!ok,    "truncated blob refused");
    TEST_ASSERT(n == 0, "out_count zeroed on failure");
    (void)len;
}

void test_attest_match_any_bssid_hit(void) {
    uint8_t bssids[3][6] = {{1,1,1,1,1,1},{2,2,2,2,2,2},{3,3,3,3,3,3}};
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)bssids, 3, "9q8", "self");

    uint8_t query[2][6] = {{9,9,9,9,9,9},{2,2,2,2,2,2}};
    TEST_ASSERT(attest_match_any_bssid(blob, len, query, 2),
                "hit when any bssid intersects");
}

void test_attest_match_any_bssid_miss(void) {
    uint8_t bssids[3][6] = {{1,1,1,1,1,1},{2,2,2,2,2,2},{3,3,3,3,3,3}};
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)bssids, 3, "9q8", "self");

    uint8_t query[2][6] = {{9,9,9,9,9,9},{0,0,0,0,0,0}};
    TEST_ASSERT(!attest_match_any_bssid(blob, len, query, 2),
                "no hit when bssids disjoint");
}

typedef struct {
    int call_count;
    int stop_after;
    size_t lens[16];
} match_ctx_t;

static int match_collect(void *vc, const uint8_t *blob, size_t len) {
    match_ctx_t *c = (match_ctx_t*)vc;
    if (c->call_count < 16) c->lens[c->call_count] = len;
    c->call_count++;
    (void)blob;
    return (c->stop_after > 0 && c->call_count >= c->stop_after) ? 1 : 0;
}

void test_attest_query_for_each_finds_self_records(void) {
    setup_rings();
    uint8_t b[1][6] = {{0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}};
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)b, 1, "9q8", "self");
    attestations_write(blob, len);

    match_ctx_t c = {0};
    int matches = attest_query_for_each(b, 1, &c, match_collect);
    TEST_ASSERT(matches == 1,      "one match found from self ring");
    TEST_ASSERT(c.call_count == 1, "callback fired once");
}

void test_attest_query_for_each_finds_cache_records(void) {
    setup_rings();
    uint8_t b[1][6] = {{0x01,0x02,0x03,0x04,0x05,0x06}};
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)b, 1, "abc", "imported");
    attest_cache_write(blob, len);

    match_ctx_t c = {0};
    int matches = attest_query_for_each(b, 1, &c, match_collect);
    TEST_ASSERT(matches == 1,      "one match found from cache ring");
    TEST_ASSERT(c.call_count == 1, "callback fired once");
}

void test_attest_query_for_each_filters_non_matching(void) {
    setup_rings();
    uint8_t hit[1][6]    = {{0x10,0x20,0x30,0x40,0x50,0x60}};
    uint8_t miss_a[1][6] = {{0xAA,0xAA,0xAA,0xAA,0xAA,0xAA}};
    uint8_t miss_b[1][6] = {{0xBB,0xBB,0xBB,0xBB,0xBB,0xBB}};

    uint8_t blob[256];
    size_t  len;

    len = build_blob(blob, (uint8_t*)miss_a, 1, "abc", "self");
    attestations_write(blob, len);
    len = build_blob(blob, (uint8_t*)hit,    1, "def", "self");
    attestations_write(blob, len);
    len = build_blob(blob, (uint8_t*)miss_b, 1, "ghi", "imported");
    attest_cache_write(blob, len);

    match_ctx_t c = {0};
    int matches = attest_query_for_each(hit, 1, &c, match_collect);
    TEST_ASSERT(matches == 1,      "only matching record emitted");
    TEST_ASSERT(c.call_count == 1, "non-matching records skipped");
}

void test_attest_query_for_each_early_stops(void) {
    setup_rings();
    uint8_t bssids[3][6] = {{1,1,1,1,1,1},{2,2,2,2,2,2},{3,3,3,3,3,3}};
    uint8_t blob[256];
    for (int i = 0; i < 3; i++) {
        size_t len = build_blob(blob, bssids[i], 1, "abc", "self");
        attestations_write(blob, len);
    }

    match_ctx_t c = { .stop_after = 2 };
    int matches = attest_query_for_each(bssids, 3, &c, match_collect);
    TEST_ASSERT(c.call_count == 2, "callback fired twice before early stop");
    TEST_ASSERT(matches == 2,      "matches counter reflects emissions");
}

void test_attest_query_for_each_empty_query(void) {
    setup_rings();
    uint8_t b[1][6] = {{1,2,3,4,5,6}};
    uint8_t blob[256];
    size_t  len = build_blob(blob, (uint8_t*)b, 1, "abc", "self");
    attestations_write(blob, len);

    match_ctx_t c = {0};
    uint8_t dummy[1][6] = {{0,0,0,0,0,0}};
    int matches = attest_query_for_each(dummy, 0, &c, match_collect);
    TEST_ASSERT(matches == 0,      "empty query returns no matches");
    TEST_ASSERT(c.call_count == 0, "callback never fired");
}
