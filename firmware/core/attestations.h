#ifndef WAXWING_ATTESTATIONS_H
#define WAXWING_ATTESTATIONS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ATTESTATIONS_CAP        64
#define ATTESTATIONS_SLOT_BYTES 200

// Persisted at /system/attest_self.bin. Layout:
//   header (16 bytes): magic 'WXAT', version 1, next_slot u8, used_count u8,
//                       reserved 9 bytes
//   slots[ATTESTATIONS_CAP]: each slot is u16 record_len followed by `record_len`
//                            bytes of opaque signed CBOR; padded to ATTESTATIONS_SLOT_BYTES.
//
// The firmware treats records as opaque — they are companion-authored,
// companion-signed, and parsed/validated entirely on iOS. The node is
// only a store-and-forward host; record schema lives in the companion app.

#define ATTESTATIONS_MAGIC0 'W'
#define ATTESTATIONS_MAGIC1 'X'
#define ATTESTATIONS_MAGIC2 'A'
#define ATTESTATIONS_MAGIC3 'T'
#define ATTESTATIONS_VERSION 1

void attestations_init(void);
void attestations_reset(void);

// Append `blob` (length `len`) to the ring. FIFO eviction at capacity.
// Returns 0 on success, -1 if the blob exceeds slot capacity or persist fails.
int attestations_write(const uint8_t *blob, size_t len);

int attestations_count(void);

// Iterate stored attestations from oldest to newest. Stop early by
// returning non-zero from `cb`. Returns the number of records visited.
typedef int (*attestations_iter_cb)(void *ctx, const uint8_t *blob, size_t len);
int attestations_for_each(void *ctx, attestations_iter_cb cb);

#endif
