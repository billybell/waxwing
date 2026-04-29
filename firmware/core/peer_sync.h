#ifndef WAXWING_PEER_SYNC_H
#define WAXWING_PEER_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Peer sync session state machine. Pulls every file the peer has into
 * the local /files/ that we don't already have, plus their .meta
 * sidecars. Built on the existing file-command CBOR vocabulary
 * (ls / read_start / read_chunk / read_meta) — no new wire format.
 *
 * Driven by the caller's run loop (the BLE client side in the hw
 * layer). Pure logic — no btstack, no clock, no RNG.
 *
 * Lifecycle
 * ---------
 *   peer_sync_start(...)                 — caller has just connected as
 *                                            Central; emits the FIRST
 *                                            command into out_buf
 *   peer_sync_handle_response(...)       — caller received a notification
 *                                            on the File Response
 *                                            characteristic; emits the
 *                                            next command, or DONE / ERROR
 *   peer_sync_end(...)                   — caller is disconnecting;
 *                                            aborts any in-flight chunked
 *                                            write so we never leak a
 *                                            half-written file
 *
 * Manifest counter
 * ----------------
 * After each successful fs_chunked_finish, peer_sync calls
 * manifest_counter_bump() exactly once. Files skipped because they
 * already exist locally do NOT bump. Pulls aborted by error or
 * disconnect do NOT bump.
 *
 * Rules of use
 * ------------
 *   - Only one session may be active at a time. peer_sync_start
 *     returns NULL if one is already in flight.
 *   - The caller MUST eventually call peer_sync_end, even on error.
 *     end is what aborts the chunked write if one is open.
 *   - Both step functions return a result enum; on PEER_SYNC_DONE or
 *     PEER_SYNC_ERROR, no further commands are emitted.
 */

typedef enum {
    PEER_SYNC_NEED_WRITE,    // out_buf contains the next command to send
    PEER_SYNC_DONE,          // session finished cleanly; caller disconnects
    PEER_SYNC_ERROR,         // session failed; caller disconnects
} peer_sync_step_t;

typedef struct peer_sync_session peer_sync_session_t;

/*
 * Start a session. `peer_tpk_prefix` is captured for record-keeping
 * when the session ends — peer_sync does not look at it itself; the
 * caller passes it through to peer_table on completion.
 *
 * On success the FIRST outgoing command is encoded into out_buf and
 * *out_len; the caller ships those bytes over BLE and waits for a
 * response. Returns the session handle (non-NULL) plus PEER_SYNC_NEED_WRITE
 * via *out_step. On failure (already-active session, output buffer too
 * small) returns NULL and *out_step = PEER_SYNC_ERROR.
 */
peer_sync_session_t *peer_sync_start(const uint8_t peer_tpk_prefix[8],
                                     uint8_t *out_buf, size_t out_max,
                                     size_t *out_len,
                                     peer_sync_step_t *out_step);

/*
 * Feed an incoming File Response notification into the FSM. On
 * NEED_WRITE, out_buf/out_len carry the next command to send.
 * On DONE/ERROR, out_len is set to 0.
 */
peer_sync_step_t peer_sync_handle_response(peer_sync_session_t *s,
                                           const uint8_t *resp,
                                           size_t resp_len,
                                           uint8_t *out_buf, size_t out_max,
                                           size_t *out_len);

/*
 * End the session. Aborts any in-flight chunked write (so no
 * half-written file is left in /files/). After this call the session
 * handle is no longer valid.
 *
 * Idempotent — calling twice is safe.
 */
void peer_sync_end(peer_sync_session_t *s);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_PEER_SYNC_H
