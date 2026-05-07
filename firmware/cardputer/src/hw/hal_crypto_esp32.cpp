// CardPuter HAL crypto: real Ed25519 (RFC 8032) via monocypher,
// RNG via the ESP32-S3 hardware RNG (esp_random — true RNG once
// the BT controller is up, which it is by the time identity.c
// calls hal_random_bytes).
//
// Standard Ed25519 — interoperable with the Pico W port and with
// iOS CryptoKit Curve25519.Signing.

extern "C" {
#include "core/hal_crypto.h"
#include "core/thirdparty/sha256/sha256.h"
#include "core/thirdparty/monocypher/monocypher-ed25519.h"
}

#include <esp_random.h>
#include <cstring>
#include <cstdlib>

struct hal_sha256_ctx {
    sha256_ctx ctx;
};

extern "C" hal_sha256_ctx_t* hal_sha256_init(void) {
    hal_sha256_ctx_t *s = (hal_sha256_ctx_t*)std::malloc(sizeof(hal_sha256_ctx));
    if (s) sha256_init(&s->ctx);
    return s;
}

extern "C" void hal_sha256_update(hal_sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (ctx) sha256_update(&ctx->ctx, data, len);
}

extern "C" void hal_sha256_final(hal_sha256_ctx_t *ctx, uint8_t out[32]) {
    if (ctx) sha256_final(&ctx->ctx, out);
}

extern "C" void hal_sha256_free(hal_sha256_ctx_t *ctx) {
    std::free(ctx);
}

extern "C" bool hal_sha256_blob(const uint8_t *data, size_t len, uint8_t out[32]) {
    if (!data || !out) return false;
    sha256(data, len, out);
    return true;
}

extern "C" bool hal_random_bytes(uint8_t *out, size_t n) {
    while (n >= 4) {
        uint32_t r = esp_random();
        std::memcpy(out, &r, 4);
        out += 4;
        n  -= 4;
    }
    if (n > 0) {
        uint32_t r = esp_random();
        std::memcpy(out, &r, n);
    }
    return true;
}

extern "C" bool hal_ed25519_derive_pub(const uint8_t seed[32],
                                       uint8_t pub_out[32]) {
    // crypto_ed25519_key_pair wipes its `seed` argument after use, so
    // copy first. The full secret key (seed||pub) is also returned but
    // we don't need it for derive — wipe and discard.
    uint8_t seed_copy[32];
    uint8_t secret_key[64];
    std::memcpy(seed_copy, seed, 32);
    crypto_ed25519_key_pair(secret_key, pub_out, seed_copy);
    crypto_wipe(secret_key, sizeof(secret_key));
    return true;
}

extern "C" bool hal_ed25519_sign(const uint8_t seed[32],
                                 const uint8_t *message, size_t message_len,
                                 uint8_t sig_out[64]) {
    uint8_t seed_copy[32];
    uint8_t secret_key[64];
    uint8_t pub[32];
    std::memcpy(seed_copy, seed, 32);
    crypto_ed25519_key_pair(secret_key, pub, seed_copy);
    crypto_ed25519_sign(sig_out, secret_key, message, message_len);
    crypto_wipe(secret_key, sizeof(secret_key));
    return true;
}

extern "C" bool hal_ed25519_verify(const uint8_t pub[32],
                                   const uint8_t *message, size_t message_len,
                                   const uint8_t sig[64]) {
    return crypto_ed25519_check(sig, pub, message, message_len) == 0;
}
