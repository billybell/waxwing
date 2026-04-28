#include "test.h"
#include "core/peer_sync.h"
#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/filestore.h"
#include "core/manifest_counter.h"
#include "mock_filestore.h"

#include <string.h>

// All responses peer_sync receives are canned CBOR maps built by these
// helpers. The local mock_filestore models the *receiver* — the side
// peer_sync writes pulled files into. The "peer" side is just bytes we
// inject as responses.

static const uint8_t TPK_PREFIX[8] = { 1,2,3,4,5,6,7,8 };

static uint8_t  tx_buf[512];        // command we just emitted
static size_t   tx_len;
static uint8_t  resp_buf[512];      // canned response bytes
static size_t   resp_len;

// ---------------------------------------------------------------------------
// CBOR builders for canned peer responses.
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    uint32_t    size;
} ls_entry_t;

static size_t build_ls_response(uint8_t *out, const ls_entry_t *entries,
                                int n, uint32_t next_offset) {
    uint8_t *p = out;
    int n_fields = next_offset > 0 ? 2 : 1;
    p += cborencode_map_header(p, (uint32_t)n_fields);
    p += cborencode_text_str(p, "files", 5);
    p += cborencode_array_header(p, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        p += cborencode_map_header(p, 3);
        p += cborencode_text_str(p, "name", 4);
        p += cborencode_text_str(p, entries[i].name, strlen(entries[i].name));
        p += cborencode_text_str(p, "size", 4);
        p += cborencode_uint(p, entries[i].size);
        p += cborencode_text_str(p, "hash", 4);
        uint8_t hash8[8] = { 0 };    // peer_sync doesn't look at hashes yet
        p += cborencode_byte_str(p, hash8, 8);
    }
    if (next_offset > 0) {
        p += cborencode_text_str(p, "next_offset", 11);
        p += cborencode_uint(p, next_offset);
    }
    return (size_t)(p - out);
}

static size_t build_read_start_ok(uint8_t *out, uint32_t size) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, size);
    return (size_t)(p - out);
}

static size_t build_read_chunk_ok(uint8_t *out, const uint8_t *data,
                                  size_t len) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_byte_str(p, data, len);
    return (size_t)(p - out);
}

static size_t build_read_meta_empty(uint8_t *out) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "meta", 4);
    p += cborencode_map_header(p, 0);
    return (size_t)(p - out);
}

static size_t build_error(uint8_t *out, const char *msg) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "error", 5);
    p += cborencode_text_str(p, msg, strlen(msg));
    return (size_t)(p - out);
}

// ---------------------------------------------------------------------------
// Inspect what peer_sync just emitted, so tests can assert on the
// command name without re-encoding.
// ---------------------------------------------------------------------------

static int tx_is_cmd(const char *expected) {
    cbor_item_t root;
    if (!cbor_parse(tx_buf, tx_buf + tx_len, &root) ||
        root.type != CBOR_TYPE_MAP) return 0;
    char cmd[16];
    if (!cbor_map_get_text(root.data, tx_buf + tx_len, root.arg, "cmd",
                           cmd, sizeof(cmd), NULL)) return 0;
    return strcmp(cmd, expected) == 0;
}

static int tx_get_name(char *out, size_t out_size) {
    cbor_item_t root;
    if (!cbor_parse(tx_buf, tx_buf + tx_len, &root) ||
        root.type != CBOR_TYPE_MAP) return 0;
    return cbor_map_get_text(root.data, tx_buf + tx_len, root.arg, "name",
                             out, out_size, NULL);
}

static uint64_t tx_get_uint(const char *key) {
    cbor_item_t root;
    if (!cbor_parse(tx_buf, tx_buf + tx_len, &root) ||
        root.type != CBOR_TYPE_MAP) return UINT64_MAX;
    uint64_t v = UINT64_MAX;
    cbor_map_get_uint(root.data, tx_buf + tx_len, root.arg, key, &v);
    return v;
}

// ---------------------------------------------------------------------------
// Receiver-side mock state helpers. Reads files out of mock_filestore.
// ---------------------------------------------------------------------------

static int local_has_file(const char *name) {
    return fs_file_size(name) >= 0;
}

static int local_file_bytes_match(const char *name,
                                  const uint8_t *expected, size_t len) {
    uint8_t buf[2048];
    int n = fs_read(name, buf, sizeof(buf));
    if (n < 0 || (size_t)n != len) return 0;
    return memcmp(buf, expected, len) == 0;
}

static void clear_state(void) {
    mock_fs_clear();
    manifest_counter_init();
}

// ---------------------------------------------------------------------------
// test_peer_sync_empty_peer — peer's ls returns zero files. One round
// trip and the session is done. No counter bump, no read_start issued.
// ---------------------------------------------------------------------------
void test_peer_sync_empty_peer(void) {
    clear_state();
    uint8_t v0 = manifest_counter_get();

    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len,
                                              &step);
    TEST_ASSERT(s != NULL, "session started");
    TEST_ASSERT(step == PEER_SYNC_NEED_WRITE, "first step is NEED_WRITE");
    TEST_ASSERT(tx_is_cmd("ls"), "first command is ls");

    resp_len = build_ls_response(resp_buf, NULL, 0, 0);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "empty ls → DONE on first response");

    TEST_ASSERT(manifest_counter_get() == v0,
                "no counter bump when peer has no files");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_single_file_full_pull — one small file end-to-end.
// ---------------------------------------------------------------------------
void test_peer_sync_single_file_full_pull(void) {
    clear_state();
    uint8_t v0 = manifest_counter_get();

    const uint8_t body[] = "hello waxwing";
    const size_t  body_len = sizeof(body) - 1;
    ls_entry_t one[] = { { "hello.txt", (uint32_t)body_len } };

    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);
    TEST_ASSERT(s != NULL && step == PEER_SYNC_NEED_WRITE, "started");

    // ls → response with one file
    resp_len = build_ls_response(resp_buf, one, 1, 0);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_NEED_WRITE && tx_is_cmd("read_start"),
                "after ls → emit read_start");
    char nm[32]; tx_get_name(nm, sizeof(nm));
    TEST_ASSERT(strcmp(nm, "hello.txt") == 0, "read_start targets right file");

    // read_start → size response
    resp_len = build_read_start_ok(resp_buf, (uint32_t)body_len);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_NEED_WRITE && tx_is_cmd("read_chunk"),
                "after read_start → emit read_chunk");
    TEST_ASSERT(tx_get_uint("offset") == 0, "first chunk offset is 0");

    // read_chunk → all bytes in one shot
    resp_len = build_read_chunk_ok(resp_buf, body, body_len);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_NEED_WRITE && tx_is_cmd("read_meta"),
                "after final chunk → emit read_meta");

    // counter must have bumped EXACTLY here, after fs_chunked_finish.
    TEST_ASSERT(manifest_counter_get() == (uint8_t)(v0 + 1),
                "counter bumped exactly once on body finalisation");

    // read_meta → empty meta
    resp_len = build_read_meta_empty(resp_buf);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "after read_meta → DONE");

    TEST_ASSERT(local_has_file("hello.txt"), "file present in local mock");
    TEST_ASSERT(local_file_bytes_match("hello.txt", body, body_len),
                "local bytes match peer bytes");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_multi_chunk_pull — file larger than PEER_SYNC_CHUNK_SIZE
// (200) requires multiple read_chunk round trips, each accepted in
// order, byte-perfect on completion.
// ---------------------------------------------------------------------------
void test_peer_sync_multi_chunk_pull(void) {
    clear_state();

    uint8_t big[450];
    for (int i = 0; i < (int)sizeof(big); i++) big[i] = (uint8_t)(i ^ 0x5A);
    ls_entry_t one[] = { { "big.bin", (uint32_t)sizeof(big) } };

    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    resp_len = build_ls_response(resp_buf, one, 1, 0);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_start_ok(resp_buf, (uint32_t)sizeof(big));
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);

    // Drive chunks until peer_sync moves on to read_meta.
    uint32_t served = 0;
    int chunk_count = 0;
    while (tx_is_cmd("read_chunk")) {
        uint64_t want_offset = tx_get_uint("offset");
        uint64_t want_size   = tx_get_uint("size");
        TEST_ASSERT(want_offset == served,
                    "chunk request offset matches running cursor");
        size_t actual = (size_t)want_size;
        if (served + actual > sizeof(big)) actual = sizeof(big) - served;
        resp_len = build_read_chunk_ok(resp_buf, big + served, actual);
        served += (uint32_t)actual;
        chunk_count++;
        step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                         sizeof(tx_buf), &tx_len);
        TEST_ASSERT(step == PEER_SYNC_NEED_WRITE,
                    "still NEED_WRITE while pulling/finishing");
    }
    TEST_ASSERT(chunk_count >= 3,
                "multi-chunk pull issued at least 3 read_chunk requests");
    TEST_ASSERT(tx_is_cmd("read_meta"),
                "after all chunks → read_meta");

    resp_len = build_read_meta_empty(resp_buf);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "session DONE after meta");

    TEST_ASSERT(local_file_bytes_match("big.bin", big, sizeof(big)),
                "multi-chunk bytes assembled correctly");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_pagination — peer has more files than fit in one
// page; sync follows next_offset.
// ---------------------------------------------------------------------------
void test_peer_sync_pagination(void) {
    clear_state();

    uint8_t b1[] = "alpha";
    uint8_t b2[] = "beta";

    ls_entry_t page1[] = { { "a.txt", (uint32_t)(sizeof(b1) - 1) } };
    ls_entry_t page2[] = { { "b.txt", (uint32_t)(sizeof(b2) - 1) } };

    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    // First ls → page1 + next_offset=1
    resp_len = build_ls_response(resp_buf, page1, 1, /*next=*/1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    // pull a.txt
    resp_len = build_read_start_ok(resp_buf, sizeof(b1) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_chunk_ok(resp_buf, b1, sizeof(b1) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_meta_empty(resp_buf);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    // After the meta of page1's only file, next emission is the second ls.
    TEST_ASSERT(step == PEER_SYNC_NEED_WRITE && tx_is_cmd("ls"),
                "page exhausted with next_offset>0 → fetch next page");
    TEST_ASSERT(tx_get_uint("offset") == 1,
                "second ls uses peer's reported next_offset");

    // Second ls → page2, no more pages
    resp_len = build_ls_response(resp_buf, page2, 1, 0);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_start_ok(resp_buf, sizeof(b2) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_chunk_ok(resp_buf, b2, sizeof(b2) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_meta_empty(resp_buf);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "DONE after both pages drained");

    TEST_ASSERT(local_has_file("a.txt") && local_has_file("b.txt"),
                "both files landed locally");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_dedup_by_name — a file already present locally is
// skipped: no read_start issued, no counter bump.
// ---------------------------------------------------------------------------
void test_peer_sync_dedup_by_name(void) {
    clear_state();
    mock_fs_add_entry("dup.txt", (const uint8_t *)"local", 5);
    uint8_t v0 = manifest_counter_get();

    ls_entry_t one[] = { { "dup.txt", 5 } };
    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    resp_len = build_ls_response(resp_buf, one, 1, 0);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE,
                "single file present locally → DONE without read_start");
    TEST_ASSERT(manifest_counter_get() == v0,
                "dedup skip does NOT bump the manifest counter");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_dedup_skips_through_to_unique — page contains a mix of
// known and unknown files; only the unknown ones get pulled.
// ---------------------------------------------------------------------------
void test_peer_sync_dedup_skips_through_to_unique(void) {
    clear_state();
    mock_fs_add_entry("known.txt", (const uint8_t *)"X", 1);

    const uint8_t body[] = "fresh";
    ls_entry_t two[] = {
        { "known.txt", 1 },
        { "fresh.txt", (uint32_t)(sizeof(body) - 1) },
    };

    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    resp_len = build_ls_response(resp_buf, two, 2, 0);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_NEED_WRITE && tx_is_cmd("read_start"),
                "known file skipped, unknown triggers read_start");
    char nm[32]; tx_get_name(nm, sizeof(nm));
    TEST_ASSERT(strcmp(nm, "fresh.txt") == 0,
                "read_start targets the unknown file");

    resp_len = build_read_start_ok(resp_buf, sizeof(body) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_chunk_ok(resp_buf, body, sizeof(body) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_meta_empty(resp_buf);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "session done after unique pulled");
    TEST_ASSERT(local_has_file("fresh.txt"), "fresh file landed");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_peer_error_mid_chunk — peer returns error during a
// chunked pull. peer_sync aborts the chunked write and ends the
// session. The half-written file MUST NOT remain.
// ---------------------------------------------------------------------------
void test_peer_sync_peer_error_mid_chunk(void) {
    clear_state();
    uint8_t v0 = manifest_counter_get();

    ls_entry_t one[] = { { "midfail.bin", 500 } };
    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    resp_len = build_ls_response(resp_buf, one, 1, 0);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_start_ok(resp_buf, 500);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);

    // First chunk arrives fine.
    uint8_t partial[200] = { 0 };
    resp_len = build_read_chunk_ok(resp_buf, partial, sizeof(partial));
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);

    // Second chunk: peer reports error.
    resp_len = build_error(resp_buf, "read_chunk failed");
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_ERROR,
                "mid-chunk error → session ERROR");
    TEST_ASSERT(!local_has_file("midfail.bin"),
                "no half-written file remains after mid-chunk abort");
    TEST_ASSERT(manifest_counter_get() == v0,
                "no counter bump on aborted pull");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_read_start_not_found_skips — peer returns "not found"
// to read_start. We skip the file and continue to the next, NOT abort.
// ---------------------------------------------------------------------------
void test_peer_sync_read_start_not_found_skips(void) {
    clear_state();

    const uint8_t body[] = "ok";
    ls_entry_t two[] = {
        { "missing.txt", 99 },          // peer will deny this one
        { "ok.txt",      (uint32_t)(sizeof(body) - 1) },
    };
    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    resp_len = build_ls_response(resp_buf, two, 2, 0);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    // peer_sync just sent read_start for missing.txt. Reply with error.
    TEST_ASSERT(tx_is_cmd("read_start"), "first command is read_start");
    resp_len = build_error(resp_buf, "not found");
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_NEED_WRITE,
                "read_start error → continue, not abort");
    TEST_ASSERT(tx_is_cmd("read_start"),
                "next command is read_start for the next file");
    char nm[32]; tx_get_name(nm, sizeof(nm));
    TEST_ASSERT(strcmp(nm, "ok.txt") == 0,
                "next read_start targets the second file");

    resp_len = build_read_start_ok(resp_buf, sizeof(body) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_chunk_ok(resp_buf, body, sizeof(body) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_meta_empty(resp_buf);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "session done");
    TEST_ASSERT(local_has_file("ok.txt"), "ok.txt landed");
    TEST_ASSERT(!local_has_file("missing.txt"),
                "denied file did not land");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_meta_error_is_best_effort — peer returns "not found"
// for read_meta. The body is already committed; meta failure must not
// abort or undo.
// ---------------------------------------------------------------------------
void test_peer_sync_meta_error_is_best_effort(void) {
    clear_state();
    uint8_t v0 = manifest_counter_get();

    const uint8_t body[] = "no meta ok";
    ls_entry_t one[] = { { "nometa.txt", (uint32_t)(sizeof(body) - 1) } };
    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    resp_len = build_ls_response(resp_buf, one, 1, 0);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_start_ok(resp_buf, sizeof(body) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_chunk_ok(resp_buf, body, sizeof(body) - 1);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);

    // Meta error.
    resp_len = build_error(resp_buf, "not found");
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "meta error → still DONE");
    TEST_ASSERT(local_has_file("nometa.txt"),
                "body committed despite meta failure");
    TEST_ASSERT(manifest_counter_get() == (uint8_t)(v0 + 1),
                "counter bumped once for the committed body");
    peer_sync_end(s);
}

// ---------------------------------------------------------------------------
// test_peer_sync_end_aborts_in_flight — peer_sync_end called while a
// chunked write is open MUST abort it. Asserts no orphan file remains.
// ---------------------------------------------------------------------------
void test_peer_sync_end_aborts_in_flight(void) {
    clear_state();

    ls_entry_t one[] = { { "orphan.bin", 500 } };
    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len, &step);

    resp_len = build_ls_response(resp_buf, one, 1, 0);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    resp_len = build_read_start_ok(resp_buf, 500);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    // One partial chunk lands.
    uint8_t buf[100] = { 0 };
    resp_len = build_read_chunk_ok(resp_buf, buf, sizeof(buf));
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);

    // Disconnect mid-pull.
    peer_sync_end(s);
    TEST_ASSERT(!local_has_file("orphan.bin"),
                "peer_sync_end aborts in-flight chunked write");
}

// ---------------------------------------------------------------------------
// test_peer_sync_only_one_session_at_a_time — second start while one is
// active returns NULL and ERROR.
// ---------------------------------------------------------------------------
void test_peer_sync_only_one_session_at_a_time(void) {
    clear_state();
    peer_sync_step_t step1, step2;
    peer_sync_session_t *a = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len,
                                              &step1);
    TEST_ASSERT(a != NULL, "first session started");

    uint8_t buf2[256];
    size_t  len2;
    peer_sync_session_t *b = peer_sync_start(TPK_PREFIX, buf2, sizeof(buf2),
                                              &len2, &step2);
    TEST_ASSERT(b == NULL && step2 == PEER_SYNC_ERROR,
                "second concurrent start refused");
    peer_sync_end(a);

    // After end, a fresh start works.
    peer_sync_session_t *c = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len,
                                              &step1);
    TEST_ASSERT(c != NULL, "session can start again after end");
    peer_sync_end(c);
}

// ---------------------------------------------------------------------------
// test_peer_sync_envelope_fits_mtu — every command peer_sync emits in
// a typical session fits inside (mtu - 3) for mtu = 247 (the common
// negotiated MTU).
// ---------------------------------------------------------------------------
void test_peer_sync_envelope_fits_mtu(void) {
    clear_state();

    // A maximum-length filename close to FS_MAX_NAME_LEN, exercising
    // the bound on read_chunk in particular.
    const char *long_name = "abcdefghijklmnopqrstuvwxyz01234";   // 31 chars
    ls_entry_t one[] = { { long_name, 1000 } };

    uint8_t  body[1000];
    for (int i = 0; i < (int)sizeof(body); i++) body[i] = (uint8_t)i;

    peer_sync_step_t step;
    peer_sync_session_t *s = peer_sync_start(TPK_PREFIX, tx_buf,
                                              sizeof(tx_buf), &tx_len,
                                              &step);
    TEST_ASSERT(tx_len <= 247 - 3, "ls command fits in MTU=247 envelope");

    resp_len = build_ls_response(resp_buf, one, 1, 0);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    TEST_ASSERT(tx_len <= 247 - 3,
                "read_start with max-length name fits MTU=247");

    resp_len = build_read_start_ok(resp_buf, 1000);
    peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                              sizeof(tx_buf), &tx_len);
    TEST_ASSERT(tx_len <= 247 - 3,
                "read_chunk with max-length name fits MTU=247");

    // Drain the rest, asserting on each emitted command.
    uint32_t served = 0;
    while (tx_is_cmd("read_chunk")) {
        TEST_ASSERT(tx_len <= 247 - 3,
                    "every read_chunk fits MTU=247");
        uint64_t off = tx_get_uint("offset");
        uint64_t want = tx_get_uint("size");
        size_t actual = (size_t)want;
        if (off + actual > sizeof(body)) actual = sizeof(body) - off;
        resp_len = build_read_chunk_ok(resp_buf, body + off, actual);
        served += (uint32_t)actual;
        peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                  sizeof(tx_buf), &tx_len);
    }
    TEST_ASSERT(tx_is_cmd("read_meta") && tx_len <= 247 - 3,
                "read_meta with max-length name fits MTU=247");
    TEST_ASSERT(served == 1000, "all bytes served");

    resp_len = build_read_meta_empty(resp_buf);
    step = peer_sync_handle_response(s, resp_buf, resp_len, tx_buf,
                                     sizeof(tx_buf), &tx_len);
    TEST_ASSERT(step == PEER_SYNC_DONE, "session done");
    peer_sync_end(s);
}
