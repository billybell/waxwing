#ifndef WAXWING_COMMANDS_H
#define WAXWING_COMMANDS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Distinguishes the two BLE characteristics that feed commands.c.
//
// COMPANION: connections from the iOS app over the existing companion
// characteristic. Full command surface; eventually gated by a separate
// "first-companion-pairing" flow that is not commands.c's concern.
//
// PEER: connections from another Waxwing node over the upcoming peer
// characteristic (M4 stage 6). Restricted to read-side and attestation-
// exchange commands. Mutating commands (write*, delete, attestation_write)
// and companion-private read commands (scan_get, encounters_get) return
// an "companion only" error.
typedef enum {
    COMMANDS_SESSION_COMPANION = 0,
    COMMANDS_SESSION_PEER      = 1,
} commands_session_kind_t;

/**
 * Dispatch a CBOR file command in a specific session context. Parses
 * the incoming CBOR map, applies the per-kind allowlist, runs the
 * filestore op, and encodes the response into out_buf.
 *
 * Returns bytes written to out_buf, or -1 on internal error.
 */
int commands_handle_session(commands_session_kind_t kind,
                            const uint8_t *cmd_data, size_t cmd_len,
                            uint8_t *out_buf, size_t out_max);

/**
 * Companion-mode shim, equivalent to
 * commands_handle_session(COMMANDS_SESSION_COMPANION, ...). Preserved so
 * existing callers (the inbound companion characteristic, host tests)
 * don't need to thread the session kind themselves.
 */
int commands_handle(const uint8_t *cmd_data, size_t cmd_len,
                    uint8_t *out_buf, size_t out_max);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_COMMANDS_H
