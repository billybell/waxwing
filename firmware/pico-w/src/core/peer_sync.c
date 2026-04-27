#include "core/peer_sync.h"
#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/filestore.h"
#include "core/manifest_counter.h"

#include <string.h>
#include <stdio.h>

// Conservative chunk size we ask the peer for in read_chunk requests.
// Stays under the responder's MAX_INLINE_DATA (220) and leaves headroom
// for the response envelope under any sane MTU.
#define PEER_SYNC_CHUNK_SIZE 200

// Max files we track from a single ls page response. Mirrors the
// responder's PAGE in cmd_ls but we don't strictly need that — any
// value >= 1 works because the FSM walks files one at a time. Keep it
// sized for a typical page so we don't lose entries.
#define PEER_SYNC_PAGE 16

typedef enum {
    PHASE_LS,                    // we just sent ls; expecting list page
    PHASE_READ_START,            // we just sent read_start; expecting size
    PHASE_READ_CHUNK,            // we just sent read_chunk; expecting bytes
    PHASE_READ_META,             // we just sent read_meta; expecting sidecar
    PHASE_DONE,
    PHASE_ERROR,
} phase_t;

struct peer_sync_session {
    phase_t  phase;
    int      in_use;             // guard for the single-slot allocator

    uint8_t  peer_tpk_prefix[8]; // captured for the caller; we don't read it

    // Pagination cursor.
    uint32_t ls_offset;          // next ls offset to request
    uint32_t ls_next_offset;     // server's reported next_offset (0 = done)

    // The current ls page contents, decoded from the latest ls response.
    char     names[PEER_SYNC_PAGE][32];     // 32 == FS_MAX_NAME_LEN
    uint32_t sizes[PEER_SYNC_PAGE];
    int      page_count;
    int      page_idx;                       // which file in page we're on

    // Per-file state.
    char     cur_name[32];
    uint32_t cur_total;          // bytes in this file (from read_start)
    uint32_t cur_received;       // bytes pulled so far
    int      chunked_open;       // fs_chunked_start succeeded; abort on error
    int      file_committed;     // fs_chunked_finish succeeded for cur file
};

static struct peer_sync_session g_session;

// ---------------------------------------------------------------------------
// Command encoders. Each writes a CBOR map into out_buf and returns
// bytes written; the size is bounded by the longest command we emit
// (read_chunk with a max-length filename ≈ 80 bytes), well inside any
// reasonable BLE MTU.
// ---------------------------------------------------------------------------

static size_t encode_ls(uint8_t *out, uint32_t offset) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "ls", 2);
    p += cborencode_text_str(p, "offset", 6);
    p += cborencode_uint(p, offset);
    return (size_t)(p - out);
}

static size_t encode_read_start(uint8_t *out, const char *name) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_start", 10);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    return (size_t)(p - out);
}

static size_t encode_read_chunk(uint8_t *out, const char *name,
                                uint32_t offset, uint32_t size) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 4);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_chunk", 10);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    p += cborencode_text_str(p, "offset", 6);
    p += cborencode_uint(p, offset);
    p += cborencode_text_str(p, "size", 4);
    p += cborencode_uint(p, size);
    return (size_t)(p - out);
}

static size_t encode_read_meta(uint8_t *out, const char *name) {
    uint8_t *p = out;
    p += cborencode_map_header(p, 2);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "read_meta", 9);
    p += cborencode_text_str(p, "name", 4);
    p += cborencode_text_str(p, name, strlen(name));
    return (size_t)(p - out);
}

// ---------------------------------------------------------------------------
// Response parsers. All operate on the outer CBOR map of a notification.
// Returns -1 on parse failure; caller treats as ERROR.
// ---------------------------------------------------------------------------

static int parse_outer_map(const uint8_t *resp, size_t resp_len,
                           uint64_t *out_pc, const uint8_t **out_body,
                           const uint8_t **out_end) {
    cbor_item_t root;
    const uint8_t *end = resp + resp_len;
    if (!cbor_parse(resp, end, &root) || root.type != CBOR_TYPE_MAP) return -1;
    *out_pc = root.arg;
    *out_body = root.data;
    *out_end = end;
    return 0;
}

// True if the response contains an "error" key.
static int response_is_error(const uint8_t *body, const uint8_t *end,
                             uint64_t pc) {
    cbor_item_t v;
    return cbor_map_find(body, end, pc, "error", &v) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Filestore helpers, isolating the side-effects so they're easy to find.
// ---------------------------------------------------------------------------

static void abort_inflight_write(struct peer_sync_session *s) {
    if (s->chunked_open) {
        fs_chunked_abort(s->cur_name);
        s->chunked_open = 0;
    }
}

static void start_next_file_or_finish(struct peer_sync_session *s,
                                      uint8_t *out, size_t out_max,
                                      size_t *out_len,
                                      peer_sync_step_t *out_step);

// Skip cur file because it's a duplicate of one already on disk. Bump
// page_idx and proceed.
static void skip_current_file(struct peer_sync_session *s,
                              uint8_t *out, size_t out_max,
                              size_t *out_len,
                              peer_sync_step_t *out_step) {
    s->page_idx++;
    start_next_file_or_finish(s, out, out_max, out_len, out_step);
}

// Move to whichever file comes next: another in this page, or the next
// page, or DONE.
static void start_next_file_or_finish(struct peer_sync_session *s,
                                      uint8_t *out, size_t out_max,
                                      size_t *out_len,
                                      peer_sync_step_t *out_step) {
    while (s->page_idx < s->page_count) {
        const char *name = s->names[s->page_idx];
        uint32_t    sz   = s->sizes[s->page_idx];

        // Dedup: if a file with this name already exists locally we
        // skip — name-based for v0, hash-based later.
        if (fs_file_size(name) >= 0) {
            s->page_idx++;
            continue;
        }

        // Out-of-space guard: skip any file whose size exceeds free
        // space. We never delete to make room.
        uint32_t free_b = 0, used_b = 0, reserve_b = 0, count_b = 0;
        fs_storage_info(&free_b, &used_b, &reserve_b, &count_b);
        if (sz > free_b) {
            printf("[peer_sync] skipping %s: %u bytes > %u free\r\n",
                   name, (unsigned)sz, (unsigned)free_b);
            s->page_idx++;
            continue;
        }

        // Lock in this file and ask the peer for its size.
        strncpy(s->cur_name, name, sizeof(s->cur_name) - 1);
        s->cur_name[sizeof(s->cur_name) - 1] = '\0';
        s->cur_total       = sz;
        s->cur_received    = 0;
        s->chunked_open    = 0;
        s->file_committed  = 0;

        if (out_max < 64) {
            s->phase    = PHASE_ERROR;
            *out_len    = 0;
            *out_step   = PEER_SYNC_ERROR;
            return;
        }
        *out_len  = encode_read_start(out, s->cur_name);
        s->phase  = PHASE_READ_START;
        *out_step = PEER_SYNC_NEED_WRITE;
        return;
    }

    // Page exhausted. If the server promised more, ask for the next page.
    if (s->ls_next_offset > 0) {
        s->ls_offset    = s->ls_next_offset;
        s->page_count   = 0;
        s->page_idx     = 0;
        *out_len  = encode_ls(out, s->ls_offset);
        s->phase  = PHASE_LS;
        *out_step = PEER_SYNC_NEED_WRITE;
        return;
    }

    // No more files, no more pages.
    s->phase  = PHASE_DONE;
    *out_len  = 0;
    *out_step = PEER_SYNC_DONE;
}

// ---------------------------------------------------------------------------
// Per-phase response handlers.
// ---------------------------------------------------------------------------

static void handle_ls_response(struct peer_sync_session *s,
                               const uint8_t *resp, size_t resp_len,
                               uint8_t *out, size_t out_max,
                               size_t *out_len,
                               peer_sync_step_t *out_step) {
    uint64_t pc = 0;
    const uint8_t *body = NULL, *end = NULL;
    if (parse_outer_map(resp, resp_len, &pc, &body, &end) < 0 ||
        response_is_error(body, end, pc)) {
        s->phase    = PHASE_ERROR;
        *out_len    = 0;
        *out_step   = PEER_SYNC_ERROR;
        return;
    }

    cbor_item_t files;
    if (!cbor_map_find(body, end, pc, "files", &files) ||
        files.type != CBOR_TYPE_ARRAY) {
        s->phase    = PHASE_ERROR;
        *out_len    = 0;
        *out_step   = PEER_SYNC_ERROR;
        return;
    }

    // Iterate the array. Each entry is a map with "name" and "size".
    s->page_count = 0;
    s->page_idx   = 0;
    const uint8_t *p = files.data;
    uint64_t arr_n = files.arg;
    for (uint64_t i = 0; i < arr_n && s->page_count < PEER_SYNC_PAGE; i++) {
        cbor_item_t entry;
        if (!cbor_parse(p, end, &entry) || entry.type != CBOR_TYPE_MAP) break;

        size_t name_len = 0;
        if (!cbor_map_get_text(entry.data, end, entry.arg, "name",
                               s->names[s->page_count],
                               sizeof(s->names[0]), &name_len)) {
            // Skip malformed entry but keep going — peer might just have
            // weird files we don't care about.
            p = entry.next;
            continue;
        }
        uint64_t sz = 0;
        if (!cbor_map_get_uint(entry.data, end, entry.arg, "size", &sz)) {
            p = entry.next;
            continue;
        }
        s->sizes[s->page_count] = (uint32_t)sz;
        s->page_count++;
        p = entry.next;
    }

    // Pick up next_offset (0 means peer has no more pages).
    uint64_t next = 0;
    cbor_map_get_uint(body, end, pc, "next_offset", &next);
    s->ls_next_offset = (uint32_t)next;

    start_next_file_or_finish(s, out, out_max, out_len, out_step);
}

static void handle_read_start_response(struct peer_sync_session *s,
                                       const uint8_t *resp, size_t resp_len,
                                       uint8_t *out, size_t out_max,
                                       size_t *out_len,
                                       peer_sync_step_t *out_step) {
    uint64_t pc = 0;
    const uint8_t *body = NULL, *end = NULL;
    if (parse_outer_map(resp, resp_len, &pc, &body, &end) < 0 ||
        response_is_error(body, end, pc)) {
        // Peer returned not-found (or otherwise refused). Skip this
        // file and move on to the next.
        s->page_idx++;
        start_next_file_or_finish(s, out, out_max, out_len, out_step);
        return;
    }

    uint64_t sz = 0;
    cbor_map_get_uint(body, end, pc, "size", &sz);
    if (sz != s->cur_total) {
        // ls advertised a size that doesn't match read_start. Trust
        // read_start and update our budget.
        s->cur_total = (uint32_t)sz;
    }

    // Zero-byte file: skip. fs_chunked_start rejects total_size==0
    // and we can't represent an empty file via the chunked path; the
    // single-shot fs_write would work but we don't use it on the
    // receive side. Empty files are a rare-enough edge case that
    // skipping them is fine for v0; a future encounter that produces
    // the same name with non-zero content will fall through dedup
    // (no local entry exists) and pull normally.
    if (s->cur_total == 0) {
        s->page_idx++;
        start_next_file_or_finish(s, out, out_max, out_len, out_step);
        return;
    }

    if (fs_chunked_start(s->cur_name, s->cur_total) != 0) {
        // Local filestore refused — out of space, name too long, or
        // another chunked write somehow still active. Skip this file.
        s->page_idx++;
        start_next_file_or_finish(s, out, out_max, out_len, out_step);
        return;
    }
    s->chunked_open = 1;

    *out_len  = encode_read_chunk(out, s->cur_name, 0, PEER_SYNC_CHUNK_SIZE);
    s->phase  = PHASE_READ_CHUNK;
    *out_step = PEER_SYNC_NEED_WRITE;
}

static void handle_read_chunk_response(struct peer_sync_session *s,
                                       const uint8_t *resp, size_t resp_len,
                                       uint8_t *out, size_t out_max,
                                       size_t *out_len,
                                       peer_sync_step_t *out_step) {
    uint64_t pc = 0;
    const uint8_t *body = NULL, *end = NULL;
    if (parse_outer_map(resp, resp_len, &pc, &body, &end) < 0 ||
        response_is_error(body, end, pc)) {
        // Mid-pull error: abort the chunked write so we don't leave a
        // half-written file behind, and bail the whole session. We
        // could try to skip just this file and continue, but treating
        // a mid-pull error as fatal keeps the receiver state simple
        // and the next encounter will retry under the FAILED backoff.
        abort_inflight_write(s);
        s->phase    = PHASE_ERROR;
        *out_len    = 0;
        *out_step   = PEER_SYNC_ERROR;
        return;
    }

    const uint8_t *data = NULL;
    size_t data_len = 0;
    if (!cbor_map_get_bytes(body, end, pc, "data", &data, &data_len)) {
        abort_inflight_write(s);
        s->phase    = PHASE_ERROR;
        *out_len    = 0;
        *out_step   = PEER_SYNC_ERROR;
        return;
    }

    if (fs_chunked_append(data, data_len) < 0) {
        abort_inflight_write(s);
        s->phase    = PHASE_ERROR;
        *out_len    = 0;
        *out_step   = PEER_SYNC_ERROR;
        return;
    }
    s->cur_received += (uint32_t)data_len;

    if (s->cur_received < s->cur_total) {
        // More to fetch.
        uint32_t remaining = s->cur_total - s->cur_received;
        uint32_t want = remaining < PEER_SYNC_CHUNK_SIZE
                        ? remaining : PEER_SYNC_CHUNK_SIZE;
        *out_len  = encode_read_chunk(out, s->cur_name,
                                      s->cur_received, want);
        s->phase  = PHASE_READ_CHUNK;
        *out_step = PEER_SYNC_NEED_WRITE;
        return;
    }

    // File body fully received. Finalise and bump the manifest counter.
    if (fs_chunked_finish(s->cur_name) < 0) {
        abort_inflight_write(s);     // also resets chunked_open
        s->phase    = PHASE_ERROR;
        *out_len    = 0;
        *out_step   = PEER_SYNC_ERROR;
        return;
    }
    s->chunked_open    = 0;
    s->file_committed  = 1;
    manifest_counter_bump();

    // Now ask for the .meta sidecar.
    *out_len  = encode_read_meta(out, s->cur_name);
    s->phase  = PHASE_READ_META;
    *out_step = PEER_SYNC_NEED_WRITE;
}

static void handle_read_meta_response(struct peer_sync_session *s,
                                      const uint8_t *resp, size_t resp_len,
                                      uint8_t *out, size_t out_max,
                                      size_t *out_len,
                                      peer_sync_step_t *out_step) {
    uint64_t pc = 0;
    const uint8_t *body = NULL, *end = NULL;
    if (parse_outer_map(resp, resp_len, &pc, &body, &end) == 0 &&
        !response_is_error(body, end, pc)) {
        cbor_item_t meta;
        if (cbor_map_find(body, end, pc, "meta", &meta) &&
            meta.type == CBOR_TYPE_MAP) {
            // Persist the raw meta-map bytes verbatim. Empty maps
            // (the responder's "no sidecar" sentinel) are also
            // persisted — harmless and keeps round-trips consistent.
            size_t raw_len = (size_t)(meta.next - meta.start);
            (void)fs_write_meta(s->cur_name, meta.start, raw_len);
        }
    }
    // Meta is best-effort. Errors here don't undo the file we just
    // wrote and don't abort the session.

    s->page_idx++;
    start_next_file_or_finish(s, out, out_max, out_len, out_step);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

peer_sync_session_t *peer_sync_start(const uint8_t peer_tpk_prefix[8],
                                     uint8_t *out_buf, size_t out_max,
                                     size_t *out_len,
                                     peer_sync_step_t *out_step) {
    if (g_session.in_use || !out_buf || !out_len || !out_step ||
        out_max < 64) {
        if (out_step) *out_step = PEER_SYNC_ERROR;
        if (out_len)  *out_len  = 0;
        return NULL;
    }

    memset(&g_session, 0, sizeof(g_session));
    g_session.in_use = 1;
    if (peer_tpk_prefix) {
        memcpy(g_session.peer_tpk_prefix, peer_tpk_prefix, 8);
    }
    g_session.phase = PHASE_LS;
    g_session.ls_offset = 0;

    *out_len  = encode_ls(out_buf, 0);
    *out_step = PEER_SYNC_NEED_WRITE;
    return &g_session;
}

peer_sync_step_t peer_sync_handle_response(peer_sync_session_t *s,
                                           const uint8_t *resp,
                                           size_t resp_len,
                                           uint8_t *out_buf, size_t out_max,
                                           size_t *out_len) {
    peer_sync_step_t step = PEER_SYNC_ERROR;
    if (!s || !s->in_use || !resp || !out_buf || !out_len || out_max < 64) {
        if (out_len) *out_len = 0;
        return PEER_SYNC_ERROR;
    }

    *out_len = 0;
    switch (s->phase) {
        case PHASE_LS:
            handle_ls_response(s, resp, resp_len, out_buf, out_max,
                               out_len, &step);
            break;
        case PHASE_READ_START:
            handle_read_start_response(s, resp, resp_len, out_buf, out_max,
                                       out_len, &step);
            break;
        case PHASE_READ_CHUNK:
            handle_read_chunk_response(s, resp, resp_len, out_buf, out_max,
                                       out_len, &step);
            break;
        case PHASE_READ_META:
            handle_read_meta_response(s, resp, resp_len, out_buf, out_max,
                                      out_len, &step);
            break;
        case PHASE_DONE:
            step = PEER_SYNC_DONE;
            break;
        case PHASE_ERROR:
            step = PEER_SYNC_ERROR;
            break;
    }
    return step;
}

void peer_sync_end(peer_sync_session_t *s) {
    if (!s || !s->in_use) return;
    abort_inflight_write(s);
    s->in_use = 0;
}
