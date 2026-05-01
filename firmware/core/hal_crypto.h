#ifndef WAXWING_HAL_CRYPTO_H
#define WAXWING_HAL_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Tiny crypto/RNG HAL. Keeps core/ free of any hardware or third-party
// dependency: identity.c calls these, and hw/pico-w/ + tests/ provide
// their own implementations.
//
// Commit 1 keeps the original placeholder behavior on the device (so the
// wire format and observable behavior do not change in this commit). The
// real Ed25519 + hardware RNG implementation lands in commit 2.

#ifdef __cplusplus
extern "C" {
#endif

// Fill `out` with `n` random bytes. Returns true on success.
bool hal_random_bytes(uint8_t *out, size_t n);

// Derive an Ed25519 keypair from a 32-byte seed. `priv_out` receives the
// expanded private key (kept opaque here as 32 bytes — commit 2 may widen
// to the 64-byte form once mbedTLS lands; for now we mirror the existing
// 32+32 layout so identity persistence is unchanged).
//
// Returns true on success. Implementations must be deterministic: given
// the same seed, must always produce the same pub.
bool hal_ed25519_derive_pub(const uint8_t seed[32], uint8_t pub_out[32]);

// Sign `message` (length `message_len`) with the Ed25519 keypair derived
// from `seed`. Writes 64 bytes of signature into `sig_out`.
// Returns true on success. Deterministic per RFC 8032: same (seed, message)
// always produces the same signature.
bool hal_ed25519_sign(const uint8_t seed[32],
                      const uint8_t *message, size_t message_len,
                      uint8_t sig_out[64]);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_HAL_CRYPTO_H
