#include "test.h"
#include "core/commands.h"
#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "mock_filestore.h"

#include <string.h>

// Build a minimal CBOR command map: { "cmd": <name> }. Sufficient to
// exercise the per-kind allowlist gate, which only inspects the "cmd"
// key before dispatching.
static size_t build_cmd_only(uint8_t *buf, const char *cmd) {
    uint8_t *p = buf;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, cmd, strlen(cmd));
    return (size_t)(p - buf);
}

// True if response is a map with "error" key whose text starts with the
// given expected prefix. The dispatcher trims to 24 chars in emit_error.
static bool response_has_error(const uint8_t *resp, size_t resp_len,
                               const char *expected_prefix) {
    cbor_item_t root;
    if (!cbor_parse(resp, resp + resp_len, &root)) return false;
    if (root.type != CBOR_TYPE_MAP) return false;
    cbor_item_t v;
    if (!cbor_map_find(root.data, resp + resp_len, root.arg, "error", &v)) return false;
    if (v.type != CBOR_TYPE_TSTR) return false;
    size_t exp = strlen(expected_prefix);
    if (v.arg < exp) return false;
    return memcmp(v.data, expected_prefix, exp) == 0;
}

static bool response_has_ok(const uint8_t *resp, size_t resp_len) {
    cbor_item_t root;
    if (!cbor_parse(resp, resp + resp_len, &root)) return false;
    if (root.type != CBOR_TYPE_MAP) return false;
    cbor_item_t v;
    if (!cbor_map_find(root.data, resp + resp_len, root.arg, "ok", &v)) return false;
    return v.type == CBOR_TYPE_BOOL && v.arg == 1;
}

void test_peer_gate_companion_only_write_rejected(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "write");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER write returns response");
    TEST_ASSERT(response_has_error(out, n, "companion only"),
                "write rejected on peer characteristic");
}

void test_peer_gate_companion_only_delete_rejected(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "delete");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER delete returns response");
    TEST_ASSERT(response_has_error(out, n, "companion only"),
                "delete rejected on peer characteristic");
}

void test_peer_gate_companion_only_scan_get_rejected(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "scan_get");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER scan_get returns response");
    TEST_ASSERT(response_has_error(out, n, "companion only"),
                "scan_get rejected on peer characteristic");
}

void test_peer_gate_companion_only_encounters_get_rejected(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "encounters_get");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER encounters_get returns response");
    TEST_ASSERT(response_has_error(out, n, "companion only"),
                "encounters_get rejected on peer characteristic");
}

void test_peer_gate_companion_only_attestation_write_rejected(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "attestation_write");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER attestation_write returns response");
    TEST_ASSERT(response_has_error(out, n, "companion only"),
                "attestation_write rejected on peer characteristic");
}

void test_peer_gate_ls_allowed(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "ls");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER ls returns response");
    // ls on an empty store yields {"files":[], "next_offset":0} — no error.
    TEST_ASSERT(!response_has_error(out, n, "companion"),
                "ls is allowed on peer characteristic");
}

void test_peer_gate_storage_info_allowed(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "storage_info");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER storage_info returns response");
    TEST_ASSERT(!response_has_error(out, n, "companion"),
                "storage_info is allowed on peer characteristic");
}

void test_peer_gate_unknown_cmd_still_unknown(void) {
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "nope");
    int n = commands_handle_session(COMMANDS_SESSION_PEER, req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "PEER unknown returns response");
    // Unknown command isn't on the peer allowlist either; the allowlist
    // catches it first with "companion only", which is acceptable —
    // either error tells a misbehaving peer to stop.
    TEST_ASSERT(response_has_error(out, n, "companion only") ||
                response_has_error(out, n, "unknown"),
                "unknown command yields a clear error");
}

void test_peer_gate_companion_mode_unchanged(void) {
    // commands_handle (companion shim) should still allow the full
    // surface — same path the iOS app exercises.
    mock_fs_clear();
    uint8_t req[64], out[256];
    size_t rlen = build_cmd_only(req, "scan_get");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "companion scan_get returns response");
    TEST_ASSERT(!response_has_error(out, n, "companion only"),
                "scan_get is still allowed on companion characteristic");
    (void)response_has_ok;     // not asserted here; scan_get may succeed or fail
}                              // depending on whether the SSID-scan stub has data.
