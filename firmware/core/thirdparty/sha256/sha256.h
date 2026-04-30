// Public-domain SHA-256 (FIPS 180-4). Streaming API.
//
// Used by every firmware target and the host test build to hash file
// contents the same way iOS does (CryptoKit SHA-256). The 32-byte digest
// can be truncated to whatever prefix length the manifest uses.

#ifndef WAXWING_SHA256_H
#define WAXWING_SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buffer[64];
    size_t   buffer_len;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);

// One-shot convenience for callers that already have the full buffer.
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_SHA256_H
