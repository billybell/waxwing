// Pico W HAL crypto: real Ed25519 (RFC 8032 / curve25519 + SHA-512) via
// monocypher, RNG via the RP2040 hardware RNG (`get_rand_32`, ROSC-based).
//
// Standard Ed25519 — interoperable with iOS CryptoKit Curve25519.Signing.

#include "core/hal_crypto.h"
#include "core/thirdparty/sha256/sha256.h"

#include <string.h>
#include <stdlib.h>

#include "pico/rand.h"
#include "core/thirdparty/monocypher/monocypher-ed25519.h"

// ---------------------------------------------------------------------------
// SHA-256 wrappers (bridge to vendored implementation)
// ---------------------------------------------------------------------------

struct hal_sha256_ctx {
    sha256_ctx ctx;
};

hal_sha256_ctx_t* hal_sha256_init(void) {
    hal_sha256_ctx_t *s = malloc(sizeof(hal_sha256_ctx_t));
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

// ---------------------------------------------------------------------------
// RNG + Ed25519
// ---------------------------------------------------------------------------

bool hal_random_bytes(uint8_t *out, size_t n) {
    while (n >= 4) {
        uint32_t r = get_rand_32();
        memcpy(out, &r, 4);
        out += 4;
        n -= 4;
    }
    if (n > 0) {
        uint32_t r = get_rand_32();
        memcpy(out, &r, n);
    }
    return true;
}

bool hal_ed25519_derive_pub(const uint8_t seed[32], uint8_t pub_out[32]) {
    // crypto_ed25519_key_pair wipes its `seed` argument after use, so
    // copy first. The full secret key (seed||pub) is also returned but
    // we don't need it for derive — wipe and discard.
    uint8_t seed_copy[32];
    uint8_t secret_key[64];
    memcpy(seed_copy, seed, 32);
    crypto_ed25519_key_pair(secret_key, pub_out, seed_copy);
    crypto_wipe(secret_key, sizeof(secret_key));
    return true;
}

bool hal_ed25519_sign(const uint8_t seed[32],
                      const uint8_t *message, size_t message_len,
                      uint8_t sig_out[64]) {
    uint8_t seed_copy[32];
    uint8_t secret_key[64];
    uint8_t pub[32];
    memcpy(seed_copy, seed, 32);
    crypto_ed25519_key_pair(secret_key, pub, seed_copy);
    crypto_ed25519_sign(sig_out, secret_key, message, message_len);
    crypto_wipe(secret_key, sizeof(secret_key));
    return true;
}

bool hal_ed25519_verify(const uint8_t pub[32],
                        const uint8_t *message, size_t message_len,
                        const uint8_t sig[64]) {
    return crypto_ed25519_check(sig, pub, message, message_len) == 0;
}
