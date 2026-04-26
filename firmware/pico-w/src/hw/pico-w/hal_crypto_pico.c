// Pico W HAL crypto implementation.
//
// Commit 1: keeps the original placeholder behavior bit-for-bit so that
// the wire-format tpk emitted in the BLE identity blob is unchanged.
// Commit 2 will replace these with real Ed25519 + RP2040 hardware RNG.

#include "core/hal_crypto.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

bool hal_random_bytes(uint8_t *out, size_t n) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)rand();
    }
    return true;
}

// Historical placeholder: pub = seed (zero-pad/truncate to 32 bytes).
// This matches what the previous sha256_placeholder() produced for a
// 32-byte input. Replaced with mbedTLS Ed25519 derivation in commit 2.
bool hal_ed25519_derive_pub(const uint8_t seed[32], uint8_t pub_out[32]) {
    memcpy(pub_out, seed, 32);
    return true;
}
