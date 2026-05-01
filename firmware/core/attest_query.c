#include "core/attest_query.h"

#include <string.h>

#include "core/attest_cache.h"
#include "core/attestations.h"
#include "core/cbor_decode.h"

#define BSSID_KEY        4
#define BSSID_BYTES      6
// Top-3 is what the companion writes today. Allow a little headroom in
// case that limit grows later, but keep it bounded — this array lives
// on the stack inside attest_match_any_bssid.
#define MAX_EXTRACTED    8

bool attest_extract_bssids(const uint8_t *blob, size_t blob_len,
                           uint8_t (*out_bssids)[6], int max,
                           int *out_count) {
    if (out_count) *out_count = 0;
    if (!blob || blob_len == 0 || !out_bssids || max <= 0 || !out_count) {
        return false;
    }

    const uint8_t *end = blob + blob_len;
    cbor_item_t root;
    if (!cbor_parse(blob, end, &root) || root.type != CBOR_TYPE_MAP) {
        return false;
    }

    const uint8_t *p = root.data;
    for (uint64_t i = 0; i < root.arg; i++) {
        cbor_item_t key, val;
        if (!cbor_parse(p, end, &key))         return false;
        if (!cbor_parse(key.next, end, &val))  return false;
        p = val.next;

        if (key.type != CBOR_TYPE_UINT || key.arg != BSSID_KEY) continue;
        if (val.type != CBOR_TYPE_ARRAY) return false;

        const uint8_t *ap = val.data;
        int n = 0;
        for (uint64_t j = 0; j < val.arg; j++) {
            cbor_item_t it;
            if (!cbor_parse(ap, end, &it)) return false;
            ap = it.next;
            if (it.type != CBOR_TYPE_BSTR || it.arg != BSSID_BYTES) continue;
            if (n >= max) break;
            memcpy(out_bssids[n], it.data, BSSID_BYTES);
            n++;
        }
        *out_count = n;
        return true;
    }
    return false;
}

bool attest_match_any_bssid(const uint8_t *blob, size_t blob_len,
                            const uint8_t (*query)[6], int query_count) {
    if (!query || query_count <= 0) return false;
    uint8_t bssids[MAX_EXTRACTED][BSSID_BYTES];
    int n = 0;
    if (!attest_extract_bssids(blob, blob_len, bssids, MAX_EXTRACTED, &n)) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < query_count; j++) {
            if (memcmp(bssids[i], query[j], BSSID_BYTES) == 0) return true;
        }
    }
    return false;
}

typedef struct {
    const uint8_t   (*query)[6];
    int              query_count;
    void            *user_ctx;
    attest_query_cb  user_cb;
    int              matches;
    bool             stopped;
} walk_t;

static int walk_cb(void *vw, const uint8_t *blob, size_t len) {
    walk_t *w = (walk_t*)vw;
    if (w->stopped) return 1;
    if (!attest_match_any_bssid(blob, len, w->query, w->query_count)) return 0;
    w->matches++;
    int rc = w->user_cb(w->user_ctx, blob, len);
    if (rc != 0) { w->stopped = true; return 1; }
    return 0;
}

int attest_query_for_each(const uint8_t (*query)[6], int query_count,
                          void *ctx, attest_query_cb cb) {
    if (!query || query_count <= 0 || !cb) return 0;
    walk_t w = {0};
    w.query       = query;
    w.query_count = query_count;
    w.user_ctx    = ctx;
    w.user_cb     = cb;

    attestations_for_each(&w, walk_cb);
    if (!w.stopped) {
        attest_cache_for_each(&w, walk_cb);
    }
    return w.matches;
}
