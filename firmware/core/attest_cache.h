#ifndef WAXWING_ATTEST_CACHE_H
#define WAXWING_ATTEST_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ATTEST_CACHE_CAP        64
#define ATTEST_CACHE_SLOT_BYTES 200

// Persisted at /system/attest_cache.bin. Same on-disk shape as
// attestations.{c,h} (header + 64 fixed-size slots), but stores
// attestations RECEIVED from peers over peer_sync rather than ones
// authored on this companion.
//
// Records are opaque signed CBOR — the firmware never validates them,
// only stores and serves them. Dedup is exact byte equality of the
// stored blob: re-receiving the same record from a different peer is
// a no-op (returns 0 from _write).

#define ATTEST_CACHE_MAGIC0 'W'
#define ATTEST_CACHE_MAGIC1 'X'
#define ATTEST_CACHE_MAGIC2 'A'
#define ATTEST_CACHE_MAGIC3 'C'
#define ATTEST_CACHE_VERSION 1

void attest_cache_init(void);
void attest_cache_reset(void);

// Insert `blob` (length `len`) into the ring. FIFO eviction at capacity.
// Returns:
//   1  inserted
//   0  duplicate (already present in the live ring; nothing changed)
//  -1  error (oversized, persist failure, or not yet initialized)
int attest_cache_write(const uint8_t *blob, size_t len);

int attest_cache_count(void);

// Iterate cached attestations from oldest to newest. Stop early by
// returning non-zero from `cb`. Returns the number of records visited.
typedef int (*attest_cache_iter_cb)(void *ctx, const uint8_t *blob, size_t len);
int attest_cache_for_each(void *ctx, attest_cache_iter_cb cb);

#endif
