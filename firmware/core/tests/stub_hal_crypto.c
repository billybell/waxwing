// Deterministic test stub for the crypto HAL.
//
// hal_random_bytes returns a counter sequence so tests can assert on
// exact bytes produced. Tests that need to detect "did identity.c call
// the RNG?" can read mock_hal_random_call_count.
//
// hal_ed25519_derive_pub is a deterministic transform of the seed. It is
// not Ed25519 and is not cryptographically meaningful — its only job is
// to give identity.c a stable seed→pub mapping so the verify-on-load
// branch can be exercised.

#include "core/hal_crypto.h"
#include "stub_hal_crypto.h"
#include "core/thirdparty/sha256/sha256.h"

#include <string.h>
#include <stdlib.h>

struct hal_sha256_ctx {
    sha256_ctx ctx;
};

hal_sha256_ctx_t* hal_sha256_init(void) {
    hal_sha256_ctx_t *s = malloc(sizeof(struct hal_sha256_ctx));
    if (s) sha256_init(&s->ctx);
    return s;
}

void hal_sha256_update(hal_sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (ctx) sha256_update(&ctx->ctx, data, len);
}

void hal_sha256_final(hal_sha256_ctx_t *ctx, uint8_t out[32]) {
    if (ctx) sha256_final(&ctx->ctx, out);
}

void hal_sha256_free(hal_sha256_ctx_t *ctx) {
    free(ctx);
}

bool hal_sha256_blob(const uint8_t *data, size_t len, uint8_t out[32]) {
    if (!data || !out) return false;
    sha256(data, len, out);
    return true;
}

unsigned mock_hal_random_call_count = 0;
static uint8_t random_counter = 0;

void mock_hal_reset(void) {
    mock_hal_random_call_count = 0;
    random_counter = 0;
}

bool hal_random_bytes(uint8_t *out, size_t n) {
    mock_hal_random_call_count++;
    for (size_t i = 0; i < n; i++) {
        out[i] = random_counter++;
    }
    return true;
}

// Deterministic transform: pub[i] = seed[i] XOR 0x5A. Bijective, so
// distinct seeds produce distinct pubs (good enough for the load/verify
// path to exercise mismatches).
bool hal_ed25519_derive_pub(const uint8_t seed[32], uint8_t pub_out[32]) {
    for (int i = 0; i < 32; i++) {
        pub_out[i] = seed[i] ^ 0x5A;
    }
    return true;
}

// Deterministic fake signature: first 32 bytes = seed XOR 0xA5, second 32
// bytes = byte-by-byte sum of seed and message rotated. Tests only care
// that (seed, message) → sig is reproducible and sensitive to the inputs.
bool hal_ed25519_sign(const uint8_t seed[32],
                      const uint8_t *message, size_t message_len,
                      uint8_t sig_out[64]) {
    for (int i = 0; i < 32; i++) sig_out[i] = seed[i] ^ 0xA5;
    uint8_t acc = 0;
    for (size_t i = 0; i < message_len; i++) acc += message[i];
    for (int i = 0; i < 32; i++) {
        sig_out[32 + i] = (uint8_t)(seed[i] + acc + (uint8_t)i);
    }
    return true;
}

// Stub verifier. The stub's `derive_pub` is a bijection (`pub = seed XOR
// 0x5A`), so we can recover the seed from the pub, recompute what `sign`
// would have produced, and compare. This makes the stub round-trip
// consistent without any real crypto.
bool hal_ed25519_verify(const uint8_t pub[32],
                        const uint8_t *message, size_t message_len,
                        const uint8_t sig[64]) {
    uint8_t recovered_seed[32];
    for (int i = 0; i < 32; i++) recovered_seed[i] = pub[i] ^ 0x5A;
    uint8_t expected[64];
    if (!hal_ed25519_sign(recovered_seed, message, message_len, expected)) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        if (sig[i] != expected[i]) return false;
    }
    return true;
}
