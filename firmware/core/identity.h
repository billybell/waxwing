#ifndef WAXWING_IDENTITY_H
#define WAXWING_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "core/constants.h"

#ifdef __cplusplus
extern "C" {
#endif

// Identity record on storage:
//   magic[4] = 'W','X','I','D'
//   version  = 1
//   seed[32]
//   pub[32]
// Total = 4 + 1 + 32 + 32 = 69 bytes.
#define IDENTITY_BLOB_SIZE 69
#define IDENTITY_MAGIC0 'W'
#define IDENTITY_MAGIC1 'X'
#define IDENTITY_MAGIC2 'I'
#define IDENTITY_MAGIC3 'D'
#define IDENTITY_VERSION 1

// Filename used for persistence under the existing fs_* root.
// Excludes leading slash — fs_* operates on bare names.
#define IDENTITY_BLOB_NAME "identity.bin"

typedef struct {
    uint8_t seed[32];        // 32-byte seed material (kept secret).
    uint8_t pub[32];         // Public key derived from seed.
    char    tpk_hex[65];     // 64-char lowercase hex of pub + NUL.
    char    fingerprint[9];  // 8-char lowercase hex prefix + NUL.
    char    node_name[16];   // "WX:AABBCCDD" + NUL.
} waxwing_identity_t;

// Load the persisted identity, or generate a fresh one and persist it.
// Returns true on success. On a corrupted/old/missing blob, regenerates.
bool waxwing_identity_load_or_generate(waxwing_identity_t *identity);

// Wipe the persisted identity (factory reset / testing).
void waxwing_identity_wipe(void);

// Encoding helpers (used by ble.c and tests).
void waxwing_identity_bytes_to_hex(const uint8_t *bytes, size_t len,
                                   char *hex_out);
void waxwing_identity_tpk_to_base64url(const uint8_t *pub_key,
                                       char *b64url_out);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_IDENTITY_H
