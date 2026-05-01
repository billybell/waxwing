#include "core/attestations.h"
#include "core/commands.h"
#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/encounters.h"
#include "core/ssid_scan.h"
#include "mock_filestore.h"
#include "stub_hal_crypto.h"
#include "stub_ssid_scan.h"
#include "test.h"

static const uint8_t NODE_PUB[32] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
};
static const uint8_t SEED[32] = {
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
};
static const uint8_t BSSID_A[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t BSSID_B[6] = {0x10, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

static size_t build_cmd(uint8_t *buf, const char *cmd) {
    uint8_t *p = buf;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, cmd, strlen(cmd));
    return (size_t)(p - buf);
}

static size_t build_encounters_get(uint8_t *buf, uint32_t since_ms, uint32_t offset) {
    uint8_t *p = buf;
    int fields = 1 + (since_ms ? 1 : 0) + (offset ? 1 : 0);
    p += cborencode_map_header(p, fields);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "encounters_get", 14);
    if (since_ms) {
        p += cborencode_text_str(p, "since_ms", 8);
        p += cborencode_uint(p, since_ms);
    }
    if (offset) {
        p += cborencode_text_str(p, "offset", 6);
        p += cborencode_uint(p, offset);
    }
    return (size_t)(p - buf);
}

void test_scan_get_no_scan_yet(void) {
    mock_fs_clear();
    mock_ssid_scan_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd(req, "scan_get");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "scan_get returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root) && root.type == CBOR_TYPE_MAP,
                "response parses as map");
    cbor_item_t err;
    bool has_err = cbor_map_find(root.data, out + n, root.arg, "error", &err);
    TEST_ASSERT(has_err, "no-scan path returns error key");
}

void test_scan_get_paginates_via_next_offset(void) {
    mock_fs_clear();
    ssid_scan_t s;
    ssid_scan_init(&s);
    // Eight observations with maxed-out SSIDs to force pagination at MTU 247.
    char long_ssid[33];
    memset(long_ssid, 'x', 32);
    long_ssid[32] = '\0';
    for (uint8_t i = 0; i < SSID_SCAN_MAX; i++) {
        uint8_t b[6] = {0x10, 0x00, 0x00, 0x00, 0x00, i};
        ssid_scan_add(&s, b, (const uint8_t*)long_ssid, 32, (int8_t)(-40 - i), 11);
    }
    s.scanned_ms = 999;
    mock_ssid_scan_set(&s);

    uint8_t out[512];

    // Page 1.
    uint8_t req1[64];
    uint8_t *p = req1;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "scan_get", 8);
    int n = commands_handle(req1, (size_t)(p - req1), out, sizeof(out));
    cbor_item_t root1, obs1;
    TEST_ASSERT(cbor_parse(out, out + n, &root1) && root1.type == CBOR_TYPE_MAP, "page1 parses");
    TEST_ASSERT(cbor_map_find(root1.data, out + n, root1.arg, "obs", &obs1) &&
                obs1.type == CBOR_TYPE_ARRAY && obs1.arg > 0 && obs1.arg < SSID_SCAN_MAX,
                "page1 returned a partial obs array");
    uint64_t next = 0;
    TEST_ASSERT(cbor_map_get_uint(root1.data, out + n, root1.arg, "next_offset", &next) &&
                next == obs1.arg,
                "page1 next_offset = obs count");

    // Page 2.
    uint8_t req2[64];
    p = req2;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "scan_get", 8);
    p += cborencode_text_str(p, "offset", 6);
    p += cborencode_uint(p, (uint32_t)next);
    n = commands_handle(req2, (size_t)(p - req2), out, sizeof(out));
    cbor_item_t root2, obs2;
    TEST_ASSERT(cbor_parse(out, out + n, &root2) && root2.type == CBOR_TYPE_MAP, "page2 parses");
    TEST_ASSERT(cbor_map_find(root2.data, out + n, root2.arg, "obs", &obs2) &&
                obs2.type == CBOR_TYPE_ARRAY && obs2.arg > 0,
                "page2 has observations");
    TEST_ASSERT(obs1.arg + obs2.arg <= SSID_SCAN_MAX, "two pages don't double-count");
}

void test_scan_get_returns_observations(void) {
    mock_fs_clear();
    ssid_scan_t s;
    ssid_scan_init(&s);
    ssid_scan_add(&s, BSSID_A, (const uint8_t*)"home", 4, -42, 6);
    ssid_scan_add(&s, BSSID_B, (const uint8_t*)"shop", 4, -65, 11);
    s.scanned_ms = 12345;
    mock_ssid_scan_set(&s);

    uint8_t req[64], out[512];
    size_t rlen = build_cmd(req, "scan_get");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "scan_get returns response");

    cbor_item_t root, ok, scanned, obs;
    TEST_ASSERT(cbor_parse(out, out + n, &root) && root.type == CBOR_TYPE_MAP,
                "parses as map");
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "ok", &ok) &&
                ok.type == CBOR_TYPE_BOOL && ok.arg == 1, "ok=true");
    uint64_t scanned_ms = 0;
    TEST_ASSERT(cbor_map_get_uint(root.data, out + n, root.arg, "scanned_ms", &scanned_ms) &&
                scanned_ms == 12345, "scanned_ms preserved");
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "obs", &obs) &&
                obs.type == CBOR_TYPE_ARRAY && obs.arg == 2, "obs array has 2 entries");

    // Walk the first observation and check bssid, rssi.
    cbor_item_t entry0;
    TEST_ASSERT(cbor_parse(obs.data, out + n, &entry0) && entry0.type == CBOR_TYPE_MAP,
                "first obs is map");
    const uint8_t *bs_data = NULL;
    size_t bs_len = 0;
    TEST_ASSERT(cbor_map_get_bytes(entry0.data, out + n, entry0.arg, "bssid", &bs_data, &bs_len) &&
                bs_len == 6 && memcmp(bs_data, BSSID_A, 6) == 0,
                "first bssid matches");
}

void test_encounters_get_empty(void) {
    mock_fs_clear();
    mock_hal_reset();
    encounters_reset();
    mock_ssid_scan_clear();

    uint8_t req[64], out[512];
    size_t rlen = build_encounters_get(req, 0, 0);
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "encounters_get with no records returns response");
    cbor_item_t root, recs;
    TEST_ASSERT(cbor_parse(out, out + n, &root) && root.type == CBOR_TYPE_MAP, "parses");
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "records", &recs) &&
                recs.type == CBOR_TYPE_ARRAY && recs.arg == 0, "empty records array");
}

void test_encounters_get_returns_records(void) {
    mock_fs_clear();
    mock_hal_reset();
    encounters_reset();

    ssid_scan_t s;
    ssid_scan_init(&s);
    ssid_scan_add(&s, BSSID_A, (const uint8_t*)"x", 1, -40, 1);
    encounters_record(&s, 100, NODE_PUB, SEED);

    uint8_t req[64], out[1024];
    size_t rlen = build_encounters_get(req, 0, 0);
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "encounters_get returns response");
    cbor_item_t root, recs;
    TEST_ASSERT(cbor_parse(out, out + n, &root) && root.type == CBOR_TYPE_MAP, "parses");
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "records", &recs) &&
                recs.type == CBOR_TYPE_ARRAY && recs.arg == 1,
                "records array has 1 entry");

    // Each record entry is a bstr containing the full signed CBOR record.
    cbor_item_t rec0;
    TEST_ASSERT(cbor_parse(recs.data, out + n, &rec0) && rec0.type == CBOR_TYPE_BSTR &&
                rec0.arg > 0,
                "record entry is non-empty bstr");
}

void test_encounters_get_since_filter(void) {
    mock_fs_clear();
    mock_hal_reset();
    encounters_reset();

    ssid_scan_t s;
    ssid_scan_init(&s);
    ssid_scan_add(&s, BSSID_A, (const uint8_t*)"x", 1, -40, 1);
    encounters_record(&s, 100, NODE_PUB, SEED);
    ssid_scan_init(&s);
    ssid_scan_add(&s, BSSID_B, (const uint8_t*)"y", 1, -50, 6);
    encounters_record(&s, 200, NODE_PUB, SEED);

    uint8_t req[64], out[1024];
    size_t rlen = build_encounters_get(req, 100, 0);
    int n = commands_handle(req, rlen, out, sizeof(out));
    cbor_item_t root, recs;
    cbor_parse(out, out + n, &root);
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "records", &recs) &&
                recs.type == CBOR_TYPE_ARRAY && recs.arg == 1,
                "since_ms=100 returns only the captured_at>100 record");
}

static size_t build_attestation_write(uint8_t *buf, const uint8_t *blob, size_t len) {
    uint8_t *p = buf;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "attestation_write", 17);
    p += cborencode_text_str(p, "blob", 4);
    p += cborencode_byte_str(p, blob, len);
    return (size_t)(p - buf);
}

void test_cmd_attestation_write_round_trip(void) {
    mock_fs_clear();
    attestations_reset();

    uint8_t blob[80];
    for (size_t i = 0; i < sizeof(blob); i++) blob[i] = (uint8_t)(i ^ 0x33);

    uint8_t req[256], out[256];
    size_t rlen = build_attestation_write(req, blob, sizeof(blob));
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0,                         "attestation_write returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root) && root.type == CBOR_TYPE_MAP,
                "response parses as map");
    cbor_item_t ok;
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "ok", &ok) &&
                ok.type == CBOR_TYPE_BOOL && ok.arg == 1,
                "ok=true");
    TEST_ASSERT(attestations_count() == 1, "stored exactly one attestation");

    // Request it back via attestations_get.
    uint8_t req2[64];
    uint8_t *p = req2;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "attestations_get", 16);
    n = commands_handle(req2, (size_t)(p - req2), out, sizeof(out));
    cbor_item_t root2, recs;
    cbor_parse(out, out + n, &root2);
    TEST_ASSERT(cbor_map_find(root2.data, out + n, root2.arg, "records", &recs) &&
                recs.type == CBOR_TYPE_ARRAY && recs.arg == 1,
                "round-tripped via attestations_get");
    cbor_item_t rec0;
    TEST_ASSERT(cbor_parse(recs.data, out + n, &rec0) &&
                rec0.type == CBOR_TYPE_BSTR &&
                rec0.arg == sizeof(blob) &&
                memcmp(rec0.data, blob, sizeof(blob)) == 0,
                "blob bytes preserved");
}

void test_cmd_attestation_write_missing_blob(void) {
    mock_fs_clear();
    attestations_reset();
    uint8_t req[64], out[256];
    uint8_t *p = req;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "attestation_write", 17);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    cbor_item_t root, err;
    cbor_parse(out, out + n, &root);
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "error", &err),
                "missing blob → error");
    TEST_ASSERT(attestations_count() == 0, "nothing persisted");
}

void test_encounters_get_paginates_via_next_offset(void) {
    mock_fs_clear();
    mock_hal_reset();
    encounters_reset();

    // Fill with several records — each ~200 bytes encoded; with stub MTU=247
    // the response budget is small enough that not all fit.
    ssid_scan_t s;
    for (int i = 0; i < 8; i++) {
        ssid_scan_init(&s);
        uint8_t b[6] = {0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)i};
        ssid_scan_add(&s, b, (const uint8_t*)"x", 1, -40, 1);
        ssid_scan_add(&s, BSSID_A, (const uint8_t*)"a", 1, -60, 6);
        encounters_record(&s, 1000 + (uint32_t)i, NODE_PUB, SEED);
    }

    uint8_t req[64], out[256];
    size_t rlen = build_encounters_get(req, 0, 0);
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "first page fits");
    cbor_item_t root, recs;
    cbor_parse(out, out + n, &root);
    TEST_ASSERT(cbor_map_find(root.data, out + n, root.arg, "records", &recs) &&
                recs.type == CBOR_TYPE_ARRAY,
                "first page has records key");
    uint64_t next_off = 0;
    bool has_next = cbor_map_get_uint(root.data, out + n, root.arg, "next_offset", &next_off);
    TEST_ASSERT(has_next && next_off == recs.arg, "next_offset = records returned");

    // Page 2: ask with offset=next_off.
    rlen = build_encounters_get(req, 0, (uint32_t)next_off);
    n = commands_handle(req, rlen, out, sizeof(out));
    cbor_item_t root2, recs2;
    cbor_parse(out, out + n, &root2);
    TEST_ASSERT(cbor_map_find(root2.data, out + n, root2.arg, "records", &recs2) &&
                recs2.type == CBOR_TYPE_ARRAY,
                "second page parses");
    TEST_ASSERT(recs.arg + recs2.arg <= 8, "two pages don't double-count");
    TEST_ASSERT(recs.arg + recs2.arg > 0, "two pages cover at least one record");
}
