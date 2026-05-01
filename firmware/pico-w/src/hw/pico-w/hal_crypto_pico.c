// Pico W HAL crypto: real Ed25519 (RFC 8032 / curve25519 + SHA-512) via
// monocypher, RNG via the RP2040 hardware RNG (`get_rand_32`, ROSC-based).
//
// Standard Ed25519 — interoperable with iOS CryptoKit Curve25519.Signing.

#include "core/hal_crypto.h"

#include <string.h>

#include "pico/rand.h"
#include "core/thirdparty/monocypher/monocypher-ed25519.h"

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
