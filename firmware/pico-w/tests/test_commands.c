#include "test.h"
#include "core/commands.h"
#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/filestore.h"
#include "mock_filestore.h"

// ---------------------------------------------------------------------------
// Test helpers — build a CBOR map for a command request
// ---------------------------------------------------------------------------

static size_t build_cmd_request(uint8_t *buf, const char *cmd) {
    uint8_t *p = buf;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, cmd, strlen(cmd));
    return (size_t)(p - buf);
}

static size_t build_write_request(uint8_t *buf, const char *cmd,
                                   const char *name, const uint8_t *data, size_t len) {
    uint8_t *p = buf;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, cmd, strlen(cmd));
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    if (data && len > 0) {
        p += cborencode_text_str(p, "data", 4);
        p += cborencode_text_str(p, (const char *)data, len);
     }
    return (size_t)(p - buf);
}

// ---------------------------------------------------------------------------
// test_ls_empty — files array is empty when no files exist
// ---------------------------------------------------------------------------

void test_ls_empty(void) {
    mock_fs_clear();
    uint8_t req[64], out[512];
    size_t rlen = build_cmd_request(req, "ls");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "ls returns response");
    if (n <= 0) return;
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "ls parses as CBOR map");
    if (!cbor_parse(out, out + n, &root)) return;
    cbor_item_t files;
    bool ok = cbor_map_find(root.data, out + n,
                               (uint64_t)root.arg, "files", &files);
    TEST_ASSERT(ok, "ls has 'files' key");
    if (!ok) return;
    TEST_ASSERT(files.type == CBOR_TYPE_ARRAY && files.arg == 0,
                 "ls files array is empty when no files");
}

// ---------------------------------------------------------------------------
// test_ls_with_files — files list with correct shape: name, size, hash
// ---------------------------------------------------------------------------

void test_ls_with_files(void) {
    mock_fs_clear();
    const char *files[] = { "a.txt", "b.txt", "c.jpg" };
    const char *contents[] = { "alpha", "beta-bytes", "jpg-here" };
    for (int i = 0; i < 3; i++) {
        mock_fs_add_entry(files[i], (const uint8_t *)contents[i], strlen(contents[i]));
    }
    uint8_t req[64], out[512];
    size_t rlen = build_cmd_request(req, "ls");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "ls returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "ls parses");
    if (!cbor_parse(out, out + n, &root)) return;
    cbor_item_t files_arr;
    bool ok = cbor_map_find(root.data, out + n,
                              (uint64_t)root.arg, "files", &files_arr);
    TEST_ASSERT(ok && files_arr.type == CBOR_TYPE_ARRAY && files_arr.arg == 3,
                "ls returns 3 files");
    if (!ok || files_arr.arg != 3) return;

    // Walk all three entries; verify name + size + hash shape on each.
    const uint8_t *cursor = files_arr.data;
    for (int i = 0; i < 3; i++) {
        cbor_item_t entry;
        ok = cbor_parse(cursor, out + n, &entry) && entry.type == CBOR_TYPE_MAP;
        TEST_ASSERT(ok, "ls entry is a map");
        if (!ok) return;
        char name[FS_MAX_NAME_LEN];
        ok = cbor_map_get_text(entry.data, out + n, entry.arg, "name",
                               name, sizeof(name), NULL);
        TEST_ASSERT(ok && strcmp(name, files[i]) == 0,
                    "ls entry name matches expected");
        uint64_t size_v = 0;
        ok = cbor_map_get_uint(entry.data, out + n, entry.arg, "size", &size_v);
        TEST_ASSERT(ok && size_v == strlen(contents[i]),
                    "ls entry size matches content");
        const uint8_t *hash_ptr = NULL;
        size_t hash_len = 0;
        ok = cbor_map_get_bytes(entry.data, out + n, entry.arg, "hash",
                                &hash_ptr, &hash_len);
        TEST_ASSERT(ok && hash_len == 8, "ls entry hash is 8 bytes");
        cursor = entry.next;
    }
}

// ---------------------------------------------------------------------------
// test_ls_pagination — next_offset signals more entries; offset+limit math
// ---------------------------------------------------------------------------

void test_ls_pagination(void) {
    mock_fs_clear();
    // commands.c paginates ls in pages of 16; seed 20 to force a boundary.
    char names[20][FS_MAX_NAME_LEN];
    for (int i = 0; i < 20; i++) {
        snprintf(names[i], sizeof(names[i]), "f%02d.txt", i);
        mock_fs_add_entry(names[i], (const uint8_t *)"x", 1);
    }
    uint8_t req[64], out[1024];

    // First page: ls with no offset → 16 entries + next_offset=16
    size_t rlen = build_cmd_request(req, "ls");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "ls page 1 returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "ls page 1 parses");
    cbor_item_t files_arr;
    bool ok = cbor_map_find(root.data, out + n, root.arg, "files", &files_arr);
    TEST_ASSERT(ok && files_arr.type == CBOR_TYPE_ARRAY && files_arr.arg == 16,
                "ls page 1 has 16 entries");
    uint64_t next_off = 0;
    ok = cbor_map_get_uint(root.data, out + n, root.arg, "next_offset", &next_off);
    TEST_ASSERT(ok && next_off == 16, "ls page 1 next_offset is 16");

    // Second page: ls with offset=16 → 4 remaining, no next_offset.
    uint8_t *p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "ls", 2);
    p += cborencode_text_str(p, "offset", 6);
    p += cborencode_uint(p, 16);
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "ls page 2 returns response");
    TEST_ASSERT(cbor_parse(out, out + n, &root), "ls page 2 parses");
    ok = cbor_map_find(root.data, out + n, root.arg, "files", &files_arr);
    TEST_ASSERT(ok && files_arr.arg == 4, "ls page 2 has 4 entries");
    cbor_item_t no_more;
    bool has_next = cbor_map_find(root.data, out + n, root.arg,
                                   "next_offset", &no_more);
    TEST_ASSERT(!has_next, "ls page 2 omits next_offset (end of list)");
}

// ---------------------------------------------------------------------------
// test_storage_info — field names match iOS expectations
// ---------------------------------------------------------------------------

void test_storage_info(void) {
    mock_fs_clear();
    uint8_t req[64], out[512];
    size_t rlen = build_cmd_request(req, "storage_info");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "storage_info returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "storage_info parses");
    if (!cbor_parse(out, out + n, &root)) return;
      // Check for "info" key
    cbor_item_t info_val;
    bool ok = cbor_map_find(root.data, out + n,
                             (uint64_t)root.arg, "info", &info_val);
    TEST_ASSERT(ok && info_val.type == CBOR_TYPE_MAP, "storage_info has 'info' key");
    if (!ok || info_val.type != CBOR_TYPE_MAP) return;
      // Inside info map: file_count, free, reserve, used
    uint64_t val;
    cbor_map_get_uint(info_val.data, out + n,
                      info_val.arg, "file_count", &val);
    TEST_ASSERT(val == 0, "storage_info file_count is 0 for empty fs");
}

// ---------------------------------------------------------------------------
// test_read_existing_file — bytes round-trip
// ---------------------------------------------------------------------------

void test_read_existing_file(void) {
    mock_fs_clear();
    const char *test_content = "hello world";
    mock_fs_add_entry("readme.txt", (const uint8_t *)test_content, strlen(test_content));
    uint8_t req[128], out[512];
      // Build a read request: {"cmd":"read","name":"readme.txt"}
    uint8_t *p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read", 4);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "readme.txt", 10);
    size_t rlen = (size_t)(p - req);
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "read returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "read parses");
    if (!cbor_parse(out, out + n, &root)) return;
    cbor_item_t data_val;
    bool ok = cbor_map_find(root.data, out + n,
                             (uint64_t)root.arg, "data", &data_val);
    TEST_ASSERT(ok && data_val.type == CBOR_TYPE_TSTR, "read response has 'data' key");
    if (!ok || data_val.type != CBOR_TYPE_TSTR) return;
    TEST_ASSERT(data_val.arg == strlen(test_content),
                "read data matches content length");
    if (data_val.arg != strlen(test_content)) return;
    TEST_ASSERT(memcmp(data_val.data, test_content, data_val.arg) == 0,
                "read data matches content bytes");
}

// ---------------------------------------------------------------------------
// test_read_missing_file — returns error: not found
// ---------------------------------------------------------------------------

void test_read_missing_file(void) {
    mock_fs_clear();
    uint8_t req[128], out[512];
    uint8_t *p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read", 4);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "missing.txt", 11);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "read missing parses");
    if (!cbor_parse(out, out + n, &root)) return;
    cbor_item_t err_val;
    bool ok = cbor_map_find(root.data, out + n,
                             (uint64_t)root.arg, "error", &err_val);
    TEST_ASSERT(ok && err_val.type == CBOR_TYPE_TSTR, "read missing has 'error' key");
      // Just check it's a string error — iOS checks for error presence
}

// ---------------------------------------------------------------------------
// test_write_and_read_roundtrip — write then read back
// ---------------------------------------------------------------------------

void test_write_and_read_roundtrip(void) {
    mock_fs_clear();
    const char *content = "write round-trip test";
    uint8_t req[128], out[512];
      // Build write request: {"cmd":"write","name":"rt.txt","data":"..."}
    uint8_t *p = req;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write", 5);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "rt.txt", 6);
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_text_str(p, content, strlen(content));
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write returns response");
    cbor_item_t root;
    bool ok = cbor_parse(out, out + n, &root);
    TEST_ASSERT(ok, "write returns ok");
    if (!ok || !cbor_parse(out, out + n, &root)) return;
      // Now read it back
    p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read", 4);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "rt.txt", 6);
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    cbor_item_t r;
    TEST_ASSERT(cbor_parse(out, out + n, &r), "read parses");
    if (!cbor_parse(out, out + n, &r)) return;
    cbor_item_t data_val;
    ok = cbor_map_find(r.data, out + n, (uint64_t)r.arg,
                        "data", &data_val);
    TEST_ASSERT(ok && data_val.type == CBOR_TYPE_TSTR &&
                data_val.arg == strlen(content) &&
                memcmp(data_val.data, content, data_val.arg) == 0,
                "write+read round-trip");
}

// ---------------------------------------------------------------------------
// test_delete — file gone, subsequent read returns not-found
// ---------------------------------------------------------------------------

void test_delete(void) {
    mock_fs_clear();
    mock_fs_add_entry("delme.txt", (const uint8_t *)"delete me", 9);
      // Delete
    uint8_t req[128], out[512];
    bool ok;
    uint8_t *p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "delete", 6);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "delme.txt", 9);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "delete returns response");
    cbor_item_t root;
    ok = cbor_parse(out, out + n, &root);
    TEST_ASSERT(ok, "delete returns ok");
      // Read it back — should be not found
    p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read", 4);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "delme.txt", 9);
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    cbor_item_t r2;
    TEST_ASSERT(cbor_parse(out, out + n, &r2), "read after delete parses");
    if (!cbor_parse(out, out + n, &r2)) return;
    cbor_item_t err_val;
    ok = cbor_map_find(r2.data, out + n, (uint64_t)r2.arg,
                         "error", &err_val);
    TEST_ASSERT(ok && err_val.type == CBOR_TYPE_TSTR, "read after delete returns error");
}


// ---------------------------------------------------------------------------
// test_unknown_command — returns error: unknown cmd
// ---------------------------------------------------------------------------

void test_unknown_command(void) {
    mock_fs_clear();
    uint8_t req[128], out[512];
    uint8_t *p = req;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "foobar", 6);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "unknown cmd returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "unknown cmd parses");
    if (!cbor_parse(out, out + n, &root)) return;
    cbor_item_t err_val;
    bool ok = cbor_map_find(root.data, out + n,
                             (uint64_t)root.arg, "error", &err_val);
    TEST_ASSERT(ok && err_val.type == CBOR_TYPE_TSTR && err_val.arg >= 11 &&
                memcmp(err_val.data, "unknown cmd", 11) == 0,
                "unknown command returns 'error: unknown cmd'");
}

// ---------------------------------------------------------------------------
// test_missing_name_field — returns error when name is missing
// ---------------------------------------------------------------------------

void test_missing_name_field(void) {
    mock_fs_clear();
    uint8_t req[128], out[512];
    uint8_t *p = req;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read", 4);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "missing name returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "missing name parses");
    if (!cbor_parse(out, out + n, &root)) return;
    cbor_item_t err_val;
    bool ok = cbor_map_find(root.data, out + n,
                             (uint64_t)root.arg, "error", &err_val);
    TEST_ASSERT(ok && err_val.type == CBOR_TYPE_TSTR &&
                memcmp(err_val.data, "missing name", 12) == 0,
                "missing name returns 'error: missing name'");
}

// ---------------------------------------------------------------------------
// test_chunked_write_roundtrip — chunked upload + read back
// ---------------------------------------------------------------------------

void test_chunked_write_roundtrip(void) {
    mock_fs_clear();
    const char *name = "chunked.bin";
    const uint8_t payload[] = "chunked-data-0123456789"; // 23 bytes (no NUL)
    const size_t total = sizeof(payload) - 1;
    uint8_t req[512], out[512];

    // write_start
    uint8_t *p = req;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write_start", 11);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, (uint32_t)total);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write_start returns response");

    // write_chunk: send all bytes in one chunk. cmd_write_chunk requires bstr.
    p = req;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write_chunk", 11);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_byte_str(p, payload, total);
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write_chunk returns response");
    {
        cbor_item_t cr;
        TEST_ASSERT(cbor_parse(out, out + n, &cr), "write_chunk parses");
        cbor_item_t err;
        bool has_err = cbor_map_find(cr.data, out + n, cr.arg, "error", &err);
        TEST_ASSERT(!has_err, "write_chunk has no error");
    }

    // write_end
    p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write_end", 9);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write_end returns response");
    {
        cbor_item_t cr;
        TEST_ASSERT(cbor_parse(out, out + n, &cr), "write_end parses");
        cbor_item_t err;
        bool has_err = cbor_map_find(cr.data, out + n, cr.arg, "error", &err);
        TEST_ASSERT(!has_err, "write_end has no error on size match");
    }

    // read it back and verify bytes
    p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read", 4);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    cbor_item_t r;
    TEST_ASSERT(cbor_parse(out, out + n, &r), "read after chunked parses");
    cbor_item_t data_val;
    bool ok = cbor_map_find(r.data, out + n, r.arg, "data", &data_val);
    TEST_ASSERT(ok && data_val.type == CBOR_TYPE_TSTR, "chunked read has data");
    TEST_ASSERT(ok && data_val.arg == total &&
                memcmp(data_val.data, payload, total) == 0,
                "chunked round-trip bytes match");
}

// ---------------------------------------------------------------------------
// test_chunked_write_size_mismatch — size mismatch causes failure
// ---------------------------------------------------------------------------

void test_chunked_write_size_mismatch(void) {
    mock_fs_clear();
    const char *name = "bad-chunk.bin";
    uint8_t req[512], out[512];

    // write_start: declare 50 bytes
    uint8_t *p = req;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write_start", 11);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, 50);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write_start returns response");

    // write_chunk: send only 22 bytes
    p = req;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write_chunk", 11);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_byte_str(p, (const uint8_t *)"only-22-bytes-of-data!", 22);
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write_chunk returns response");

    // write_end: must fail with "size mismatch" error
    p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write_end", 9);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write_end returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "write_end parses");
    cbor_item_t err;
    bool has_err = cbor_map_find(root.data, out + n, root.arg, "error", &err);
    TEST_ASSERT(has_err && err.type == CBOR_TYPE_TSTR,
                "write_end on mismatch returns error");
    TEST_ASSERT(has_err && err.arg >= 13 &&
                memcmp(err.data, "size mismatch", 13) == 0,
                "write_end error is 'size mismatch'");
}

// ---------------------------------------------------------------------------
// test_read_start_chunk_read — size + offset/chunk math
// ---------------------------------------------------------------------------

void test_read_start_chunk_read(void) {
    mock_fs_clear();
    const char *name = "large.bin";
    uint8_t content[64];
    for (int i = 0; i < 64; i++) content[i] = (uint8_t)(i * 37);
    mock_fs_add_entry(name, content, sizeof(content));

    // read_start: returns size
    uint8_t req[128], out[512];
    uint8_t *p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_start", 10);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "read_start returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "read_start parses");
    if (!cbor_parse(out, out + n, &root)) return;
    uint64_t size_val = 0;
    cbor_map_get_uint(root.data, out + n,
                      root.arg, "size", &size_val);
    TEST_ASSERT(size_val == sizeof(content), "read_start returns correct size");

    // read_chunk at offset=16, size=24 — verify the exact window of bytes
    p = req;
    p += cborencode_map_header(p, 4);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_chunk", 10);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    p += cborencode_text_str(p, "offset", 6);
    p += cborencode_uint(p, 16);
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, 24);
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "read_chunk returns response");
    cbor_item_t r;
    TEST_ASSERT(cbor_parse(out, out + n, &r), "read_chunk parses");
    cbor_item_t data_val;
    bool ok = cbor_map_find(r.data, out + n, r.arg, "data", &data_val);
    TEST_ASSERT(ok && data_val.type == CBOR_TYPE_BSTR,
                "read_chunk has bstr data");
    TEST_ASSERT(ok && data_val.arg == 24 &&
                memcmp(data_val.data, content + 16, 24) == 0,
                "read_chunk window matches source bytes");
}

// ---------------------------------------------------------------------------
// test_write_meta_read_meta — bytes round-trip verbatim
// ---------------------------------------------------------------------------

void test_write_meta_read_meta(void) {
    mock_fs_clear();
    mock_fs_add_entry("meta-file.txt", (const uint8_t *)"file content", 12);
    uint8_t req[512], out[512];

    // Build a real CBOR map for meta: {"k":"v"}, then capture its raw bytes.
    uint8_t meta_buf[32];
    uint8_t *mp = meta_buf;
    mp += cborencode_map_header(mp, 1);
    mp += cborencode_text_str(mp, "k", 1);
    mp += cborencode_text_str(mp, "v", 1);
    size_t meta_len = (size_t)(mp - meta_buf);

    // write_meta: copy the meta map bytes verbatim into the request.
    uint8_t *p = req;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write_meta", 10);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "meta-file.txt", 13);
    p += cborencode_text_str(p, "meta", 4);
    memcpy(p, meta_buf, meta_len);
    p += meta_len;
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write_meta returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "write_meta parses");
    cbor_item_t err;
    bool has_err = cbor_map_find(root.data, out + n, root.arg, "error", &err);
    TEST_ASSERT(!has_err, "write_meta has no error");

    // read_meta — verify the raw map bytes are returned verbatim under "meta"
    p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_meta", 9);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "meta-file.txt", 13);
    n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "read_meta returns response");
    cbor_item_t r;
    TEST_ASSERT(cbor_parse(out, out + n, &r), "read_meta parses");
    cbor_item_t meta_val;
    bool ok = cbor_map_find(r.data, out + n, r.arg, "meta", &meta_val);
    TEST_ASSERT(ok && meta_val.type == CBOR_TYPE_MAP, "read_meta returns map");
    size_t got_len = (size_t)(meta_val.next - meta_val.start);
    TEST_ASSERT(ok && got_len == meta_len &&
                memcmp(meta_val.start, meta_buf, meta_len) == 0,
                "read_meta returns identical CBOR bytes");
}

// ---------------------------------------------------------------------------
// test_read_meta_missing_sidecar — empty meta map for missing sidecar
// ---------------------------------------------------------------------------

void test_read_meta_missing_sidecar(void) {
    mock_fs_clear();
    mock_fs_add_entry("no-meta.txt", (const uint8_t *)"hello", 5);
    uint8_t req[128], out[512];
    uint8_t *p = req;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_meta", 9);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "no-meta.txt", 11);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "read_meta on missing sidecar returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "read_meta on missing sidecar parses");
    cbor_item_t meta_val;
    bool ok = cbor_map_find(root.data, out + n, root.arg, "meta", &meta_val);
    TEST_ASSERT(ok && meta_val.type == CBOR_TYPE_MAP && meta_val.arg == 0,
                "missing sidecar returns empty meta map");
}

// ---------------------------------------------------------------------------
// test_storage_info_fields — every field iOS depends on is present + plausible
// ---------------------------------------------------------------------------

void test_storage_info_fields(void) {
    mock_fs_clear();
    mock_fs_add_entry("a.txt", (const uint8_t *)"AAAA", 4);
    mock_fs_add_entry("b.txt", (const uint8_t *)"BBBBBBB", 7);
    uint8_t req[64], out[512];
    size_t rlen = build_cmd_request(req, "storage_info");
    int n = commands_handle(req, rlen, out, sizeof(out));
    TEST_ASSERT(n > 0, "storage_info returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "storage_info parses");

    cbor_item_t info;
    bool ok = cbor_map_find(root.data, out + n, root.arg, "info", &info);
    TEST_ASSERT(ok && info.type == CBOR_TYPE_MAP, "info is a map");
    if (!ok) return;

    uint64_t v;
    TEST_ASSERT(cbor_map_get_uint(info.data, out + n, info.arg,
                                  "file_count", &v) && v == 2,
                "file_count is 2");
    TEST_ASSERT(cbor_map_get_uint(info.data, out + n, info.arg,
                                  "used", &v) && v == 11,
                "used is sum of file sizes");
    TEST_ASSERT(cbor_map_get_uint(info.data, out + n, info.arg,
                                  "free", &v),
                "free field is present and uint");
    TEST_ASSERT(cbor_map_get_uint(info.data, out + n, info.arg,
                                  "reserve", &v),
                "reserve field is present and uint");
}

// ---------------------------------------------------------------------------
// test_write_then_ls — written file shows up sorted in ls
// ---------------------------------------------------------------------------

void test_write_then_ls(void) {
    mock_fs_clear();
    mock_fs_add_entry("z.txt", (const uint8_t *)"zzz", 3);
    uint8_t req[128], out[512];

    // Write "a.txt" via the public command path (not the mock helper).
    uint8_t *p = req;
    p += cborencode_map_header(p, 3);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "write", 5);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "a.txt", 5);
    p += cborencode_text_str(p, "data", 4);
    p += cborencode_text_str(p, "aaa", 3);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "write returns response");

    // ls should now list a.txt before z.txt (alphabetical)
    size_t rlen = build_cmd_request(req, "ls");
    n = commands_handle(req, rlen, out, sizeof(out));
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "ls parses");
    cbor_item_t files_arr;
    bool ok = cbor_map_find(root.data, out + n, root.arg, "files", &files_arr);
    TEST_ASSERT(ok && files_arr.arg == 2, "ls has 2 entries after write");
    if (!ok || files_arr.arg != 2) return;

    cbor_item_t first;
    TEST_ASSERT(cbor_parse(files_arr.data, out + n, &first) &&
                first.type == CBOR_TYPE_MAP, "first entry is a map");
    char name[FS_MAX_NAME_LEN];
    ok = cbor_map_get_text(first.data, out + n, first.arg, "name",
                            name, sizeof(name), NULL);
    TEST_ASSERT(ok && strcmp(name, "a.txt") == 0,
                "first listed file is a.txt (sorted)");
}

// ---------------------------------------------------------------------------
// test_read_chunk_past_eof — offset >= file size returns error
// ---------------------------------------------------------------------------

void test_read_chunk_past_eof(void) {
    mock_fs_clear();
    mock_fs_add_entry("small.bin", (const uint8_t *)"abcd", 4);
    uint8_t req[128], out[512];
    uint8_t *p = req;
    p += cborencode_map_header(p, 4);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_chunk", 10);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, "small.bin", 9);
    p += cborencode_text_str(p, "offset", 6);
    p += cborencode_uint(p, 100);
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, 16);
    int n = commands_handle(req, (size_t)(p - req), out, sizeof(out));
    TEST_ASSERT(n > 0, "read_chunk past EOF returns response");
    cbor_item_t root;
    TEST_ASSERT(cbor_parse(out, out + n, &root), "read_chunk past EOF parses");
    cbor_item_t err;
    bool has_err = cbor_map_find(root.data, out + n, root.arg, "error", &err);
    TEST_ASSERT(has_err && err.type == CBOR_TYPE_TSTR,
                "read_chunk past EOF returns error");
}
