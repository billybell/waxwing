#include "identity.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

// For now, we'll use software implementations
// In a real implementation, we would use hardware RNG and crypto acceleration
#include <time.h>

// Simple software-based random number generator for demo
// In production, use hardware RNG from Pico SDK: get_random_bytes()
static void pseudo_random_bytes(uint8_t *buf, size_t len) {
    // Seed with current time if not already seeded
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)rand();
    }
}

// Simple SHA-256 implementation placeholder
// In production, use mbedTLS or hardware acceleration from Pico SDK
static void sha256_placeholder(const uint8_t *input, size_t len, uint8_t *output) {
    // This is just a placeholder - real implementation needed
    // For now, we'll copy input to output and zero pad/truncate
    memset(output, 0, 32);
    size_t copy_len = len < 32 ? len : 32;
    memcpy(output, input, copy_len);
}

// Convert bytes to hex string
void waxwing_identity_bytes_to_hex(const uint8_t *bytes, size_t len, char *hex_out) {
    const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex_out[i * 2]     = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex_out[len * 2] = '\0';
}

// Encode public key as base64url (RFC 4648)
void waxwing_identity_tpk_to_base64url(const uint8_t *pub_key, char *b64url_out) {
    static const char *b64url_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    int i = 0;
    int j = 0;
    uint32_t enc_buf[3];
    uint8_t dec_buf[3];

    while (i < 32) {
        dec_buf[0] = pub_key[i++];
        dec_buf[1] = (i < 32) ? pub_key[i++] : 0;
        dec_buf[2] = (i < 32) ? pub_key[i++] : 0;

        enc_buf[0] = (dec_buf[0] & 0xFC) >> 2;
        enc_buf[1] = ((dec_buf[0] & 0x03) << 4) | ((dec_buf[1] & 0xF0) >> 4);
        enc_buf[2] = ((dec_buf[1] & 0x0F) << 2) | ((dec_buf[2] & 0xC0) >> 6);
        enc_buf[3] = dec_buf[2] & 0x3F;

        for (int k = 0; k < (i < 32 ? 4 : i == 32 ? 3 : 2); k++) {
            b64url_out[j++] = b64url_chars[enc_buf[k]];
        }

        if (i >= 32) {
            // Add padding if needed (though base64url typically omits padding)
            switch (32 - (i - (i > 0 ? ((i-1)/3)*3 : 0))) {
                case 1: b64url_out[j++] = '='; break;
                case 2: b64url_out[j++] = '='; b64url_out[j++] = '='; break;
            }
        }
    }
    b64url_out[j] = '\0';
}

// Load identity from flash
bool waxwing_identity_load_from_flash(waxwing_identity_t *identity) {
    FILE *f = fopen(IDENTITY_FILE, "rb");
    if (!f) {
        return false;
    }

    size_t bytes_read = fread(identity->priv, 1, 32, f);
    if (bytes_read != 32) {
        fclose(f);
        return false;
    }

    bytes_read = fread(identity->pub, 1, 32, f);
    if (bytes_read != 32) {
        fclose(f);
        return false;
    }

    fclose(f);

    // Derive expected public key from private key (for validation)
    uint8_t expected_pub[32];
    sha256_placeholder(identity->priv, 32, expected_pub);

    // Verify that stored pub matches expected pub (derived from priv)
    if (memcmp(identity->pub, expected_pub, 32) != 0) {
        return false; // Identity corruption
    }

    // Convert to hex and other formats
    waxwing_identity_bytes_to_hex(identity->pub, 32, identity->tpk_hex);
    memcpy(identity->fingerprint, identity->tpk_hex, 8);
    identity->fingerprint[8] = '\0';

    // Construct node name: "WX:" + first 8 chars of tpk_hex (uppercase)
    strcpy(identity->node_name, "WX:");
    for (int i = 0; i < 8; i++) {
        identity->node_name[3 + i] = (char)toupper(identity->tpk_hex[i]);
    }
    identity->node_name[11] = '\0';

    return true;
}

// Save identity to flash (currently disabled - identity stored in RAM only)
bool waxwing_identity_save_to_flash(const waxwing_identity_t *identity) {
    // File system not available yet - identity will be regenerated on reboot
    // TODO: Add LittleFS/SPIFFS mount and use that for persistence
    (void)identity;
    return true;  // Return success to allow operation without flash storage
}

// Load or generate identity
bool waxwing_identity_load_or_generate(waxwing_identity_t *identity) {
    // Try to load existing identity
    if (waxwing_identity_load_from_flash(identity)) {
        printf("[identity] Loaded existing Transport Identity\n");
        return true;
    }

    // Generate new identity
    printf("[identity] Generating new Transport Identity\n");

    // Generate private key
    pseudo_random_bytes(identity->priv, 32);

    // Phase 1 placeholder: public key = SHA-256(private key)
    // In Phase 2, this would be replaced with real Ed25519
    sha256_placeholder(identity->priv, 32, identity->pub);

    // Save to flash
    if (!waxwing_identity_save_to_flash(identity)) {
        printf("[identity] Failed to save identity to flash\n");
        return false;
    }

    printf("[identity] Saved to %s\n", IDENTITY_FILE);

    // Convert to hex and other formats
    waxwing_identity_bytes_to_hex(identity->pub, 32, identity->tpk_hex);
    memcpy(identity->fingerprint, identity->tpk_hex, 8);
    identity->fingerprint[8] = '\0';

    // Construct node name: "WX:" + first 8 chars of tpk_hex (uppercase)
    strcpy(identity->node_name, "WX:");
    for (int i = 0; i < 8; i++) {
        identity->node_name[3 + i] = (char)toupper(identity->tpk_hex[i]);
    }
    identity->node_name[11] = '\0';

    return true;
}

// Wipe identity (for factory reset/testing)
void waxwing_identity_wipe(void) {
    remove(IDENTITY_FILE);
    printf("[identity] Identity wiped\n");
}