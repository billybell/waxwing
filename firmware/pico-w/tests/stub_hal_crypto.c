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

#include <string.h>

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
