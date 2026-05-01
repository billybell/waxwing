#ifndef WAXWING_ATTEST_QUERY_H
#define WAXWING_ATTEST_QUERY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Pure CBOR walker — pulls the BSSID array (int-key 4) out of an
// opaque signed attestation blob authored by the companion. The
// firmware never validates the signature or any other field; it only
// trusts the wire-format invariant that key 4 is an array of 6-byte
// byte-strings. Entries that are not exactly 6 bytes are skipped
// defensively rather than failing the parse.
//
// Returns false only when the blob is not a valid CBOR map, is
// truncated, or doesn't contain key 4. *out_count is set to 0 on
// failure so callers can ignore it.
bool attest_extract_bssids(const uint8_t *blob, size_t blob_len,
                           uint8_t (*out_bssids)[6], int max,
                           int *out_count);

// True iff `blob` claims at least one BSSID that also appears in
// `query`. Convenience wrapper around attest_extract_bssids.
bool attest_match_any_bssid(const uint8_t *blob, size_t blob_len,
                            const uint8_t (*query)[6], int query_count);

// Iterate matching attestations across BOTH the local self-ring
// (attestations) AND the peer cache-ring (attest_cache). For every
// stored blob whose claimed BSSID set intersects `query`, invoke `cb`
// once. Caller can stop iteration early by returning non-zero from
// `cb`. Returns the number of matches emitted. Self-ring is visited
// before the cache ring; both are oldest-first within their ring.
typedef int (*attest_query_cb)(void *ctx, const uint8_t *blob, size_t len);
int attest_query_for_each(const uint8_t (*query)[6], int query_count,
                          void *ctx, attest_query_cb cb);

#endif
