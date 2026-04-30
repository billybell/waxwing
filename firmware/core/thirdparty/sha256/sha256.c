// Public-domain SHA-256 (FIPS 180-4). Plain C99, no dependencies beyond
// <stdint.h>/<string.h>, fits cleanly on Pico W (RP2040), ESP32-S3, and
// any host toolchain. Streaming and one-shot APIs share the same core.

#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ror32((x),  2) ^ ror32((x), 13) ^ ror32((x), 22))
#define BSIG1(x) (ror32((x),  6) ^ ror32((x), 11) ^ ror32((x), 25))
#define SSIG0(x) (ror32((x),  7) ^ ror32((x), 18) ^ ((x) >>  3))
#define SSIG1(x) (ror32((x), 17) ^ ror32((x), 19) ^ ((x) >> 10))

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[4*i + 0] << 24) |
               ((uint32_t)block[4*i + 1] << 16) |
               ((uint32_t)block[4*i + 2] <<  8) |
               ((uint32_t)block[4*i + 3]);
    }
    for (int i = 16; i < 64; i++) {
        W[i] = SSIG1(W[i-2]) + W[i-7] + SSIG0(W[i-15]) + W[i-16];
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t T1 = h + BSIG1(e) + CH(e, f, g) + K[i] + W[i];
        uint32_t T2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bit_count  = 0;
    ctx->buffer_len = 0;
}

void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
    ctx->bit_count += (uint64_t)len * 8;

    // Top-up an in-flight buffer so the streaming compressor only ever
    // sees full 64-byte blocks.
    if (ctx->buffer_len > 0) {
        size_t need = 64 - ctx->buffer_len;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buffer + ctx->buffer_len, data, take);
        ctx->buffer_len += take;
        data += take;
        len  -= take;
        if (ctx->buffer_len == 64) {
            sha256_compress(ctx->state, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }

    while (len >= 64) {
        sha256_compress(ctx->state, data);
        data += 64;
        len  -= 64;
    }

    if (len > 0) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t out[32]) {
    uint64_t bits = ctx->bit_count;

    // Append the 0x80 separator, then enough zeros to leave 8 bytes for
    // the big-endian bit length at the end of the final block.
    ctx->buffer[ctx->buffer_len++] = 0x80;
    if (ctx->buffer_len > 56) {
        memset(ctx->buffer + ctx->buffer_len, 0, 64 - ctx->buffer_len);
        sha256_compress(ctx->state, ctx->buffer);
        ctx->buffer_len = 0;
    }
    memset(ctx->buffer + ctx->buffer_len, 0, 56 - ctx->buffer_len);

    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_compress(ctx->state, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        out[4*i + 0] = (uint8_t)(ctx->state[i] >> 24);
        out[4*i + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[4*i + 2] = (uint8_t)(ctx->state[i] >>  8);
        out[4*i + 3] = (uint8_t)(ctx->state[i]);
    }

    memset(ctx->buffer, 0, sizeof(ctx->buffer));
    ctx->buffer_len = 0;
    ctx->bit_count  = 0;
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
