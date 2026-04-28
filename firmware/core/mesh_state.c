#include "core/mesh_state.h"
#include "core/constants.h"

#include <stddef.h>

// FSM state. The dwell deadline is the absolute clock value at which
// the current ADVERTISING/SCANNING phase expires and we flip role.
static struct {
    mesh_phase_t    phase;
    uint32_t        dwell_deadline_ms;     // valid in ADVERTISING / SCANNING
    uint32_t        grace_deadline_ms;     // valid in GRACE
    mesh_rng_u32_fn rng;
} g;

// Draw a dwell time uniformly in [MESH_DWELL_MIN_MS, MESH_DWELL_MAX_MS]
// with ±MESH_DWELL_JITTER_PCT applied on top. Both layers of randomness
// matter: the uniform draw varies the *centre* across cycles, the
// jitter breaks fine-grained lockstep when two devices happen to draw
// the same uniform value.
static uint32_t draw_dwell_ms(void) {
    uint32_t r1 = g.rng();
    uint32_t span = (uint32_t)MESH_DWELL_MAX_MS - (uint32_t)MESH_DWELL_MIN_MS;
    uint32_t base = (uint32_t)MESH_DWELL_MIN_MS + (r1 % (span + 1));

    // Apply ±JITTER_PCT. Map a second random uint32 into [-pct, +pct].
    uint32_t r2 = g.rng();
    int32_t  jitter_range = (int32_t)((base * MESH_DWELL_JITTER_PCT) / 100);
    int32_t  jitter = (int32_t)(r2 % (uint32_t)(2 * jitter_range + 1)) - jitter_range;

    int32_t result = (int32_t)base + jitter;
    if (result < (int32_t)MESH_DWELL_MIN_MS / 2) {
        // Defensive floor — never let jitter swing us below half the
        // minimum dwell. The arithmetic above can't actually go that
        // low with sane constants, but explicit is better.
        result = MESH_DWELL_MIN_MS / 2;
    }
    return (uint32_t)result;
}

// Choose the next role and arm the dwell deadline. Used both at the
// end of GRACE and on every dwell expiry.
static mesh_action_t flip_role(uint32_t now_ms) {
    // 50/50 between advertising and scanning. Using the LSB of an
    // RNG draw rather than alternating deterministically — same
    // reasoning as the dwell draw, defeats lockstep.
    uint32_t r = g.rng();
    g.dwell_deadline_ms = now_ms + draw_dwell_ms();
    if (r & 1u) {
        g.phase = MESH_ADVERTISING;
        return MESH_ACTION_START_ADVERTISING;
    } else {
        g.phase = MESH_SCANNING;
        return MESH_ACTION_START_SCANNING;
    }
}

void mesh_state_init(uint32_t now_ms, mesh_rng_u32_fn rng) {
    g.rng = rng;
    g.phase = MESH_GRACE;
    g.grace_deadline_ms = now_ms + (uint32_t)MESH_GRACE_MS;
    g.dwell_deadline_ms = 0;
}

mesh_action_t mesh_state_tick(uint32_t now_ms) {
    switch (g.phase) {
        case MESH_GRACE:
            if ((int32_t)(now_ms - g.grace_deadline_ms) >= 0) {
                return flip_role(now_ms);
            }
            return MESH_ACTION_NONE;

        case MESH_ADVERTISING:
        case MESH_SCANNING:
            if ((int32_t)(now_ms - g.dwell_deadline_ms) >= 0) {
                return flip_role(now_ms);
            }
            return MESH_ACTION_NONE;

        case MESH_CONNECTED:
            // Timer frozen. Caller drives us back to mesh alternation
            // via mesh_state_on_disconnected().
            return MESH_ACTION_NONE;
    }
    return MESH_ACTION_NONE;
}

void mesh_state_on_connected(uint32_t now_ms) {
    (void)now_ms;
    g.phase = MESH_CONNECTED;
}

void mesh_state_on_disconnected(uint32_t now_ms) {
    // We never go back to GRACE — the boot grace window is a one-shot
    // pairing affordance, not a recurring "settle down" period. Drop
    // out of CONNECTED into a placeholder ADVERTISING state with an
    // already-expired dwell deadline so the very next tick fires
    // flip_role and emits a real START_ADVERTISING / START_SCANNING
    // action. The placeholder phase is never observable through
    // mesh_state_phase() in practice because tick() runs immediately
    // after.
    g.phase = MESH_ADVERTISING;
    g.dwell_deadline_ms = now_ms;     // expired; next tick flips role
}

mesh_phase_t mesh_state_phase(void) {
    return g.phase;
}
