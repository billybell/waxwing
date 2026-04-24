#ifndef WAXWING_IDENTITY_H
#define WAXWING_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "constants.h"

// Identity record size: 32 bytes private key + 32 bytes public key
#define IDENTITY_RECORD_SIZE 64

// Identity structure
typedef struct {
    uint8_t priv[32];        // Private key (keep secret)
    uint8_t pub[32];         // Public key / Transport Public Key
    char tpk_hex[65];        // 64-char lowercase hex string of pub + null terminator
    char fingerprint[9];     // 8-char lowercase hex of pub[0:4] + null terminator
    char node_name[16];      // e.g. "WX:AABBCCDD" + null terminator
} waxwing_identity_t;

// Function prototypes
bool waxwing_identity_load_or_generate(waxwing_identity_t *identity);
void waxwing_identity_wipe(void);

// Utility functions
void waxwing_identity_bytes_to_hex(const uint8_t *bytes, size_t len, char *hex_out);
void waxwing_identity_tpk_to_base64url(const uint8_t *pub_key, char *b64url_out);

#endif // WAXWING_IDENTITY_H