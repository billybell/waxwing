#ifndef WAXWING_MESH_STATE_H
#define WAXWING_MESH_STATE_H

#include <stdint.h>

/*
 * Mesh-mode state machine: drives the advertise / scan alternation.
 *
 * Lifecycle (see PEER_SYNC_PLAN.md §1):
 *
 *   boot → GRACE for MESH_GRACE_MS (peripheral only; gives the user a
 *           window to pair their companion app without a peer-scan race)
 *
 *   GRACE expires → ADVERTISING for a random dwell in [MIN, MAX] ±jitter
 *       ↕ each dwell expiry flips role
 *   SCANNING for a random dwell in [MIN, MAX] ±jitter
 *
 *   any connection (companion or peer) → CONNECTED, timer frozen
 *   disconnect                          → resume mesh alternation with a
 *                                          fresh dwell draw (we do NOT
 *                                          go back to GRACE)
 *
 * Pure logic. The caller owns the clock and the RNG: at init time it
 * supplies a function that returns a uint32 of randomness, and on
 * every tick it supplies the current millisecond clock. This makes the
 * schedule fully reproducible in tests and keeps hal_random_bytes out
 * of the core layer.
 */

typedef enum {
    MESH_GRACE,
    MESH_ADVERTISING,
    MESH_SCANNING,
    MESH_CONNECTED,
} mesh_phase_t;

/*
 * Action the run loop should take this tick. Most ticks return NONE.
 *
 *   START_ADVERTISING : caller starts BLE adverts, stops scanning
 *   START_SCANNING    : caller stops BLE adverts, starts scanning
 *   STOP_BOTH         : caller stops both (we just entered CONNECTED;
 *                       in practice ble.c also stops adverts on connect,
 *                       so this is mostly a hint for the scanner side)
 */
typedef enum {
    MESH_ACTION_NONE,
    MESH_ACTION_START_ADVERTISING,
    MESH_ACTION_START_SCANNING,
    MESH_ACTION_STOP_BOTH,
} mesh_action_t;

typedef uint32_t (*mesh_rng_u32_fn)(void);

/*
 * Initialise. `now_ms` is the current monotonic millisecond clock;
 * `rng` is a function returning random uint32s, used to draw dwell
 * times. Phase starts at GRACE; the first MESH_GRACE_MS will return
 * NONE on every tick. After the grace window the FSM emits its first
 * START_ADVERTISING or START_SCANNING (the caller likely already has
 * advertising running from boot, but the action is still safe to
 * apply — it idempotently sets the desired role).
 */
void mesh_state_init(uint32_t now_ms, mesh_rng_u32_fn rng);

/* Advance the FSM. Caller polls this every run-loop iteration. */
mesh_action_t mesh_state_tick(uint32_t now_ms);

/*
 * Notify the FSM that a connection just came up (companion or peer).
 * Freezes the dwell timer. Subsequent ticks return NONE until the
 * matching disconnected event arrives.
 */
void mesh_state_on_connected(uint32_t now_ms);

/*
 * Notify the FSM that the connection ended. The next tick will emit
 * a fresh START_ADVERTISING or START_SCANNING with a new dwell draw.
 */
void mesh_state_on_disconnected(uint32_t now_ms);

mesh_phase_t mesh_state_phase(void);

#endif // WAXWING_MESH_STATE_H
