#include "core/peer_sync.h"
#include "core/cborencode.h"
#include "core/cbor_decode.h"
#include "core/filestore.h"
#include "core/manifest_counter.h"
#include "core/encounter_record.h"
#include "core/attest_cache.h"
#include "core/attest_query.h"

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

// Attestation-fetch phase tunables. We gather BSSIDs from local
// encounter records that we don't yet have attestations for, then ask
// the peer for them in batches. The total cap bounds session memory;
// the batch cap matches the responder's ATTEST_QUERY_MAX_BSSIDS so we
// always fit in a single command.
#define PEER_SYNC_BSSID_TOTAL_MAX 64
#define PEER_SYNC_BSSID_BATCH_MAX 12
#define PEER_SYNC_ENC_NAME_PREFIX     "enc_"
#define PEER_SYNC_ENC_NAME_PREFIX_LEN 4

typedef enum {
    PHASE_LS,                    // we just sent ls; expecting list page
    PHASE_READ_START,            // we just sent read_start; expecting size
    PHASE_READ_CHUNK,            // we just sent read_chunk; expecting bytes
    PHASE_READ_META,             // we just sent read_meta; expecting sidecar
    PHASE_ATTEST_QUERY,          // we just sent attest_query; expecting blobs
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
    uint8_t  hashes[PEER_SYNC_PAGE][8];
    int      page_count;
    int      page_idx;                       // which file in page we're on

    // Per-file state.
    char     cur_name[32];
    uint32_t cur_total;          // bytes in this file (from read_start)
    uint32_t cur_received;       // bytes pulled so far
    int      chunked_open;       // fs_chunked_start succeeded; abort on error
    int      file_committed;     // fs_chunked_finish succeeded for cur file

    // Attestation fetch state. After the file-pull phase ends we walk
    // local /files/enc_*.cbor records, gather every BSSID that doesn't
    // already match a stored attestation, and ask the peer for them in
    // batches. Re-entered for each pagination page within a batch.
    int      attest_phase_started;             // gather has run
    uint8_t  pending_bssids[PEER_SYNC_BSSID_TOTAL_MAX][6];
    int      pending_count;
    int      pending_next;                     // start index of next batch
    int      pending_batch_size;               // size of in-flight batch
    uint32_t attest_query_offset;              // pagination within batch
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

static size_t encode_attest_query(uint8_t *out, const uint8_t (*bssids)[6],
                                  int count, uint32_t offset) {
    uint8_t *p = out;
    int n_keys = (offset > 0) ? 3 : 2;
    p += cborencode_map_header(p, (uint32_t)n_keys);
    p += cborencode_text_str(p, "cmd", 3);
    p += cborencode_text_str(p, "attest_query", 12);
    p += cborencode_text_str(p, "bssids", 6);
    p += cborencode_array_header(p, (uint32_t)count);
    for (int i = 0; i < count; i++) {
        p += cborencode_byte_str(p, bssids[i], 6);
    }
    if (offset > 0) {
        p += cborencode_text_str(p, "offset", 6);
        p += cborencode_uint(p, offset);
    }
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

// Check if any local file has this truncated hash. An all-zero hash is
// treated as "unknown" (the peer didn't supply one, or it was malformed)
// and never matches — otherwise a peer with no hash would dedup against
// any local file whose own fs_get_hash failed and wrote zeros.
static bool fs_hash_exists(const uint8_t hash[8]) {
    static const uint8_t zero[8] = {0};
    if (memcmp(hash, zero, 8) == 0) return false;

    char names[16][FS_MAX_NAME_LEN];
    uint32_t sizes[16];
    uint8_t hashes[16][8];
    int next = 0;
    int offset = 0;

    while (1) {
        int avail = fs_list(names, sizes, hashes, 16, offset, 16, &next);
        if (avail <= 0) break;
        for (int i = 0; i < avail; i++) {
            if (memcmp(hashes[i], hash, 8) == 0) return true;
        }
        if (next <= offset) break;
        offset = next;
    }
    return false;
}

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

static void gather_unknown_encounter_bssids(struct peer_sync_session *s);

static void enter_attest_query_phase(struct peer_sync_session *s,
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
        uint8_t     *hash = s->hashes[s->page_idx];

        // Dedup: if a file with this name already exists locally, or
        // any file has the same hash, we skip.
        if (fs_file_size(name) >= 0 || fs_hash_exists(hash)) {
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

    // File-pull phase exhausted. Hand off to the attestation-fetch
    // phase: gather BSSIDs once, then page through attest_query batches.
    if (!s->attest_phase_started) {
        s->attest_phase_started = 1;
        gather_unknown_encounter_bssids(s);
    }
    enter_attest_query_phase(s, out, out_max, out_len, out_step);
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

        uint8_t h[8];
        const uint8_t *hash_ptr = NULL;
        size_t hash_len = 0;
        if (!cbor_map_get_bytes(entry.data, end, entry.arg, "hash", &hash_ptr, &hash_len) || hash_len != 8) {
            memset(s->hashes[s->page_count], 0, 8);
        } else {
            memcpy(s->hashes[s->page_count], hash_ptr, 8);
        }

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
// Attestation-fetch phase. Runs after the file-pull phase completes.
// Walks local /files/enc_*.cbor records, gathers every BSSID we don't
// already have an attestation for, and asks the peer for them via
// attest_query in batches of PEER_SYNC_BSSID_BATCH_MAX (= 12 = the
// responder's ATTEST_QUERY_MAX_BSSIDS). Responses are ingested into
// attest_cache via attest_cache_write. Hardened against peers that
// don't support attest_query: any error response folds the phase to
// PHASE_DONE rather than retrying.
// ---------------------------------------------------------------------------

typedef struct { bool found; } any_match_ctx_t;

static int any_match_cb(void *vctx, const uint8_t *blob, size_t len) {
    (void)blob; (void)len;
    ((any_match_ctx_t*)vctx)->found = true;
    return 1;        // stop after first match
}

// True if the local self-ring or peer cache-ring already has an
// attestation that lists this BSSID.
static bool local_has_attestation_for(const uint8_t bssid[6]) {
    uint8_t q[1][6];
    memcpy(q[0], bssid, 6);
    any_match_ctx_t c = { .found = false };
    attest_query_for_each(q, 1, &c, any_match_cb);
    return c.found;
}

static int bssid_already_pending(const uint8_t needle[6],
                                 const uint8_t (*list)[6], int count) {
    for (int i = 0; i < count; i++) {
        if (memcmp(needle, list[i], 6) == 0) return 1;
    }
    return 0;
}

// Scan all local /files/enc_*.cbor records, decode each, and enqueue
// every BSSID (from either bssids_a or bssids_b) for which we don't
// yet have an attestation. Capped at PEER_SYNC_BSSID_TOTAL_MAX — the
// next sync covers what didn't fit. Idempotent: re-running on a fully-
// resolved set produces zero pending entries.
static void gather_unknown_encounter_bssids(struct peer_sync_session *s) {
    s->pending_count       = 0;
    s->pending_next        = 0;
    s->pending_batch_size  = 0;
    s->attest_query_offset = 0;

    char     names[PEER_SYNC_PAGE][FS_MAX_NAME_LEN];
    uint32_t sizes[PEER_SYNC_PAGE];
    uint8_t  hashes[PEER_SYNC_PAGE][8];
    int offset = 0, next = 0;

    while (s->pending_count < PEER_SYNC_BSSID_TOTAL_MAX) {
        int avail = fs_list(names, sizes, hashes, PEER_SYNC_PAGE,
                            offset, PEER_SYNC_PAGE, &next);
        if (avail <= 0) break;

        for (int i = 0; i < avail &&
                        s->pending_count < PEER_SYNC_BSSID_TOTAL_MAX; i++) {
            // Only encounter records carry BSSIDs we'd want to resolve.
            if (strncmp(names[i], PEER_SYNC_ENC_NAME_PREFIX,
                        PEER_SYNC_ENC_NAME_PREFIX_LEN) != 0) continue;
            if (sizes[i] == 0 || sizes[i] > ENCOUNTER_RECORD_MAX_BYTES) continue;

            uint8_t buf[ENCOUNTER_RECORD_MAX_BYTES];
            int n = fs_read(names[i], buf, sizeof(buf));
            if (n <= 0) continue;

            encounter_record_t rec;
            if (!encounter_record_decode(buf, (size_t)n, &rec)) continue;

            for (int side = 0; side < 2; side++) {
                int      bcount  = (side == 0) ? rec.bssids_a_count
                                               : rec.bssids_b_count;
                uint8_t (*bs)[6] = (side == 0) ? rec.bssids_a : rec.bssids_b;
                for (int b = 0; b < bcount; b++) {
                    if (s->pending_count >= PEER_SYNC_BSSID_TOTAL_MAX) break;
                    if (bssid_already_pending(bs[b], s->pending_bssids,
                                              s->pending_count)) continue;
                    if (local_has_attestation_for(bs[b])) continue;
                    memcpy(s->pending_bssids[s->pending_count], bs[b], 6);
                    s->pending_count++;
                }
            }
        }

        if (next <= offset) break;
        offset = next;
    }
}

static void enter_attest_query_phase(struct peer_sync_session *s,
                                     uint8_t *out, size_t out_max,
                                     size_t *out_len,
                                     peer_sync_step_t *out_step) {
    if (s->pending_next >= s->pending_count) {
        s->phase  = PHASE_DONE;
        *out_len  = 0;
        *out_step = PEER_SYNC_DONE;
        return;
    }

    int batch = s->pending_count - s->pending_next;
    if (batch > PEER_SYNC_BSSID_BATCH_MAX) batch = PEER_SYNC_BSSID_BATCH_MAX;
    s->pending_batch_size = batch;

    // Worst-case wire size: map(3) + cmd + "attest_query" + "bssids" +
    // array(12) + 12*bstr(6) + "offset" + uint ≈ 130 bytes. The 64-byte
    // floor that peer_sync_handle_response already enforces is enough
    // for ls/read_meta but tight here, so require at least 200.
    if (out_max < 200) {
        s->phase  = PHASE_ERROR;
        *out_len  = 0;
        *out_step = PEER_SYNC_ERROR;
        return;
    }

    *out_len  = encode_attest_query(out,
                                    (const uint8_t (*)[6])
                                        &s->pending_bssids[s->pending_next],
                                    batch, s->attest_query_offset);
    s->phase  = PHASE_ATTEST_QUERY;
    *out_step = PEER_SYNC_NEED_WRITE;
}

static void handle_attest_query_response(struct peer_sync_session *s,
                                         const uint8_t *resp, size_t resp_len,
                                         uint8_t *out, size_t out_max,
                                         size_t *out_len,
                                         peer_sync_step_t *out_step) {
    uint64_t pc = 0;
    const uint8_t *body = NULL, *end = NULL;
    if (parse_outer_map(resp, resp_len, &pc, &body, &end) < 0 ||
        response_is_error(body, end, pc)) {
        // Peer doesn't support attest_query, or returned some other
        // failure. Don't keep hammering — finish the session cleanly;
        // the file-pull side already succeeded.
        s->phase  = PHASE_DONE;
        *out_len  = 0;
        *out_step = PEER_SYNC_DONE;
        return;
    }

    cbor_item_t blobs;
    if (cbor_map_find(body, end, pc, "blobs", &blobs) &&
        blobs.type == CBOR_TYPE_ARRAY) {
        const uint8_t *p = blobs.data;
        for (uint64_t i = 0; i < blobs.arg; i++) {
            cbor_item_t item;
            if (!cbor_parse(p, end, &item)) break;
            p = item.next;
            if (item.type != CBOR_TYPE_BSTR || item.arg == 0) continue;
            // Best-effort: dedup is exact-bytes inside attest_cache_write.
            (void)attest_cache_write(item.data, (size_t)item.arg);
        }
    }

    // If the peer reported next_offset, keep paginating the same batch.
    uint64_t next_off = 0;
    if (cbor_map_get_uint(body, end, pc, "next_offset", &next_off) &&
        next_off > 0) {
        s->attest_query_offset = (uint32_t)next_off;
        enter_attest_query_phase(s, out, out_max, out_len, out_step);
        return;
    }

    // Batch fully drained. Move to the next batch (or finish).
    s->pending_next       += s->pending_batch_size;
    s->pending_batch_size  = 0;
    s->attest_query_offset = 0;
    enter_attest_query_phase(s, out, out_max, out_len, out_step);
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
        case PHASE_ATTEST_QUERY:
            handle_attest_query_response(s, resp, resp_len, out_buf, out_max,
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
