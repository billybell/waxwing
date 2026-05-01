#ifndef WAXWING_ENCOUNTERS_H
#define WAXWING_ENCOUNTERS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "core/ssid_scan.h"

#define ENCOUNTERS_CAP        32
#define ENCOUNTERS_SLOT_BYTES 256

// Persisted at /system/encounters.bin. Layout:
//   header (16 bytes): magic 'WXEN', version 1, next_slot u8, used_count u8,
//                       reserved 9 bytes
//   slots[ENCOUNTERS_CAP]: each slot is u16 record_len followed by `record_len`
//                          bytes of CBOR; padded to ENCOUNTERS_SLOT_BYTES total.
//
// Records contain a signed CBOR map:
//   { 1: 1, 2: bstr(32) node, 3: uint captured_at_ms, 4: array bssids, 5: bstr(64) sig }

#define ENCOUNTERS_MAGIC0 'W'
#define ENCOUNTERS_MAGIC1 'X'
#define ENCOUNTERS_MAGIC2 'E'
#define ENCOUNTERS_MAGIC3 'N'
#define ENCOUNTERS_VERSION 1

// Initialize the in-RAM cache from persisted state. Tolerates missing or
// corrupted file by treating as empty. Must be called once at boot.
void encounters_init(void);

// Reset to empty (for tests).
void encounters_reset(void);

// Decide whether `scan` is a meaningful new observation worth persisting.
// True if the BSSID set differs from the most recent encounter, OR no
// recent encounter exists, OR `now_ms - last.captured_at_ms` >= min_age_ms.
// `min_age_ms` is the dedup-bypass interval (caller picks; production = 600000).
bool encounters_should_record(const ssid_scan_t *scan,
                              uint32_t now_ms,
                              uint32_t min_age_ms);

// Build, sign, and persist an encounter record.
// `node_pub` is the signer's public key (32 bytes), included in the record.
// `seed` is the signer's Ed25519 seed (32 bytes), used only for signing.
// Returns 0 on success, -1 on signing or persistence failure.
int encounters_record(const ssid_scan_t *scan,
                      uint32_t captured_at_ms,
                      const uint8_t node_pub[32],
                      const uint8_t seed[32]);

// Number of stored encounters (0..ENCOUNTERS_CAP).
int encounters_count(void);

// Iterate stored encounters from oldest to newest. `cb` receives the raw
// signed CBOR bytes. Stop early by returning non-zero from `cb`.
typedef int (*encounters_iter_cb)(void *ctx, const uint8_t *cbor, size_t len);
int encounters_for_each(void *ctx, encounters_iter_cb cb);

// Extract `captured_at_ms` from a signed encounter record. Returns false on
// malformed records. Used by the BLE handler to filter by `since_ms` without
// re-decoding the whole map.
bool encounters_record_captured_at(const uint8_t *cbor, size_t len,
                                   uint32_t *captured_at_ms_out);

#endif
