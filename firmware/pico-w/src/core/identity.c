// Transport identity: load from persisted blob or generate fresh.
//
// Persistence routes through the same fs_* API as everything else, so
// commit 1 stays platform-independent (testable on host, no FatFS or
// FILE* coupling). Crypto goes through hal_crypto — commit 1 keeps the
// historical placeholder behavior; commit 2 swaps in real Ed25519.

#include "core/identity.h"
#include "core/filestore.h"
#include "core/hal_crypto.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

void waxwing_identity_bytes_to_hex(const uint8_t *bytes, size_t len,
                                   char *hex_out) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex_out[i * 2]     = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex_out[len * 2] = '\0';
}

// Base64URL (RFC 4648 §5), no padding. 32 input bytes → 43 output chars
// + NUL. The output buffer must be at least 44 bytes.
void waxwing_identity_tpk_to_base64url(const uint8_t *pub_key,
                                       char *b64url_out) {
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t in = 0, out = 0;
    while (in + 3 <= 32) {
        uint32_t v = ((uint32_t)pub_key[in]     << 16) |
                     ((uint32_t)pub_key[in + 1] << 8)  |
                     ((uint32_t)pub_key[in + 2]);
        b64url_out[out++] = alphabet[(v >> 18) & 0x3F];
        b64url_out[out++] = alphabet[(v >> 12) & 0x3F];
        b64url_out[out++] = alphabet[(v >> 6)  & 0x3F];
        b64url_out[out++] = alphabet[v         & 0x3F];
        in += 3;
    }
    // 32 bytes leaves 32 - 30 = 2 trailing input bytes → 3 output chars.
    if (in < 32) {
        uint32_t v = (uint32_t)pub_key[in] << 16;
        if (in + 1 < 32) v |= (uint32_t)pub_key[in + 1] << 8;
        b64url_out[out++] = alphabet[(v >> 18) & 0x3F];
        b64url_out[out++] = alphabet[(v >> 12) & 0x3F];
        if (in + 1 < 32) {
            b64url_out[out++] = alphabet[(v >> 6) & 0x3F];
        }
    }
    b64url_out[out] = '\0';
}

// ---------------------------------------------------------------------------
// Derived fields (hex / fingerprint / node name)
// ---------------------------------------------------------------------------

static void populate_derived_fields(waxwing_identity_t *identity) {
    waxwing_identity_bytes_to_hex(identity->pub, 32, identity->tpk_hex);
    memcpy(identity->fingerprint, identity->tpk_hex, 8);
    identity->fingerprint[8] = '\0';

    // node_name = "WX:" + uppercase 8-char hex prefix.
    identity->node_name[0] = 'W';
    identity->node_name[1] = 'X';
    identity->node_name[2] = ':';
    for (int i = 0; i < 8; i++) {
        identity->node_name[3 + i] = (char)toupper((unsigned char)identity->tpk_hex[i]);
    }
    identity->node_name[11] = '\0';
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static bool blob_has_valid_header(const uint8_t *blob) {
    return blob[0] == IDENTITY_MAGIC0 &&
           blob[1] == IDENTITY_MAGIC1 &&
           blob[2] == IDENTITY_MAGIC2 &&
           blob[3] == IDENTITY_MAGIC3 &&
           blob[4] == IDENTITY_VERSION;
}

static bool load_blob(uint8_t *blob_out) {
    int n = fs_read(IDENTITY_BLOB_NAME, blob_out, IDENTITY_BLOB_SIZE);
    return n == IDENTITY_BLOB_SIZE;
}

static bool save_blob(const uint8_t *blob) {
    return fs_write(IDENTITY_BLOB_NAME, blob, IDENTITY_BLOB_SIZE) == 0;
}

static void serialize_blob(const waxwing_identity_t *identity, uint8_t *blob) {
    blob[0] = IDENTITY_MAGIC0;
    blob[1] = IDENTITY_MAGIC1;
    blob[2] = IDENTITY_MAGIC2;
    blob[3] = IDENTITY_MAGIC3;
    blob[4] = IDENTITY_VERSION;
    memcpy(&blob[5],         identity->seed, 32);
    memcpy(&blob[5 + 32],    identity->pub,  32);
}

// ---------------------------------------------------------------------------
// Load or generate
// ---------------------------------------------------------------------------

static bool generate_fresh(waxwing_identity_t *identity) {
    if (!hal_random_bytes(identity->seed, 32)) {
        printf("[identity] hal_random_bytes failed\r\n");
        return false;
    }
    if (!hal_ed25519_derive_pub(identity->seed, identity->pub)) {
        printf("[identity] hal_ed25519_derive_pub failed\r\n");
        return false;
    }

    uint8_t blob[IDENTITY_BLOB_SIZE];
    serialize_blob(identity, blob);
    if (!save_blob(blob)) {
        // Persistence failure isn't fatal for the running session — the
        // node will work until reboot — but it means the next reboot
        // generates a *different* identity (and a different node_name),
        // which is almost always a setup bug (e.g. fs_init not called
        // first). Log loudly so we notice in serial output.
        printf("[identity] WARN: save_blob failed — identity will not "
               "survive reboot. Check fs_init() ran before "
               "waxwing_identity_load_or_generate().\r\n");
    }
    populate_derived_fields(identity);
    printf("[identity] Generated new Transport Identity\r\n");
    return true;
}

bool waxwing_identity_load_or_generate(waxwing_identity_t *identity) {
    uint8_t blob[IDENTITY_BLOB_SIZE];

    if (load_blob(blob) && blob_has_valid_header(blob)) {
        memcpy(identity->seed, &blob[5],      32);
        memcpy(identity->pub,  &blob[5 + 32], 32);

        // Re-derive pub from seed and verify it matches what we stored.
        // If the derive function changes (commit 2 swaps placeholder for
        // real Ed25519) this check forces a regenerate, which is the
        // right thing — the old pub is no longer valid.
        uint8_t expected_pub[32];
        if (hal_ed25519_derive_pub(identity->seed, expected_pub) &&
            memcmp(expected_pub, identity->pub, 32) == 0) {
            populate_derived_fields(identity);
            printf("[identity] Loaded existing Transport Identity\r\n");
            return true;
        }
        printf("[identity] Stored identity failed verification; regenerating\r\n");
    }

    return generate_fresh(identity);
}

void waxwing_identity_wipe(void) {
    fs_delete(IDENTITY_BLOB_NAME);
    printf("[identity] Identity wiped\r\n");
}
