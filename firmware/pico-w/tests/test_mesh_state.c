#include "test.h"
#include "core/mesh_state.h"
#include "core/constants.h"

// ---------------------------------------------------------------------------
// Deterministic RNG: feed a fixed sequence of uint32s. Each test that
// cares about specific actions seeds the sequence and lets the FSM
// consume it as it draws dwells and role flips.
//
// flip_role() consumes 3 RNGs per transition: one for the role bit
// (LSB), one for the uniform dwell base, one for the jitter. So a 30-
// element sequence covers 10 role flips, plenty for our purposes.
// ---------------------------------------------------------------------------

#define RNG_BUF_LEN 256
static uint32_t g_rng_buf[RNG_BUF_LEN];
static int      g_rng_idx;
static int      g_rng_consumed;

static uint32_t test_rng(void) {
    uint32_t v = g_rng_buf[g_rng_idx % RNG_BUF_LEN];
    g_rng_idx++;
    g_rng_consumed++;
    return v;
}

static void seed_rng(const uint32_t *vals, int n) {
    for (int i = 0; i < n && i < RNG_BUF_LEN; i++) g_rng_buf[i] = vals[i];
    // Fill the rest with a benign repeat to avoid reading uninitialised
    // memory if a test consumes more entropy than it seeded.
    uint32_t fill = (n > 0) ? vals[n - 1] : 0xCAFEBABE;
    for (int i = n; i < RNG_BUF_LEN; i++) g_rng_buf[i] = fill;
    g_rng_idx = 0;
    g_rng_consumed = 0;
}

// ---------------------------------------------------------------------------
// test_mesh_starts_in_grace — fresh init, phase == GRACE, ticks at
// times before the grace deadline return NONE.
// ---------------------------------------------------------------------------
void test_mesh_starts_in_grace(void) {
    uint32_t r[] = { 1u, 2u, 3u };
    seed_rng(r, 3);
    mesh_state_init(/*now=*/0, test_rng);

    TEST_ASSERT(mesh_state_phase() == MESH_GRACE,
                "post-init phase is GRACE");
    TEST_ASSERT(mesh_state_tick(0)                == MESH_ACTION_NONE,
                "tick at t=0 returns NONE inside grace");
    TEST_ASSERT(mesh_state_tick(MESH_GRACE_MS - 1) == MESH_ACTION_NONE,
                "tick just before grace expiry returns NONE");
    TEST_ASSERT(mesh_state_phase() == MESH_GRACE,
                "phase still GRACE before deadline");
}

// ---------------------------------------------------------------------------
// test_mesh_grace_expires_to_advertising — when the grace deadline is
// reached, the next tick emits a real action and phase leaves GRACE.
// We seed the RNG so the first role flip lands on ADVERTISING.
// ---------------------------------------------------------------------------
void test_mesh_grace_expires_to_advertising(void) {
    // role-bit RNG: LSB==1 → ADVERTISING. Then dwell + jitter draws.
    uint32_t r[] = { 0x00000001u,    // role -> advertising
                     0x00000000u,    // dwell base -> MIN
                     0x00000000u };  // jitter -> 0
    seed_rng(r, 3);
    mesh_state_init(0, test_rng);

    mesh_action_t a = mesh_state_tick(MESH_GRACE_MS);
    TEST_ASSERT(a == MESH_ACTION_START_ADVERTISING,
                "first tick after grace emits START_ADVERTISING");
    TEST_ASSERT(mesh_state_phase() == MESH_ADVERTISING,
                "phase is ADVERTISING");
}

// ---------------------------------------------------------------------------
// test_mesh_grace_expires_to_scanning — same setup but role-bit LSB == 0.
// ---------------------------------------------------------------------------
void test_mesh_grace_expires_to_scanning(void) {
    uint32_t r[] = { 0x00000000u,    // role -> scanning
                     0x00000000u,    // dwell base -> MIN
                     0x00000000u };  // jitter -> 0
    seed_rng(r, 3);
    mesh_state_init(0, test_rng);

    mesh_action_t a = mesh_state_tick(MESH_GRACE_MS);
    TEST_ASSERT(a == MESH_ACTION_START_SCANNING,
                "role-bit even → START_SCANNING after grace");
    TEST_ASSERT(mesh_state_phase() == MESH_SCANNING,
                "phase is SCANNING");
}

// ---------------------------------------------------------------------------
// test_mesh_dwell_within_bounds — across many flips, every dwell the
// FSM commits to is within the documented [MIN/2, MAX*1.2] envelope
// (the inner factor is the defensive floor, the outer factor is
// MIN+SPAN multiplied by the +JITTER_PCT cap).
// ---------------------------------------------------------------------------
void test_mesh_dwell_within_bounds(void) {
    // Use varied randomness so we hit a range of dwell draws.
    uint32_t r[3 * 200];
    for (int i = 0; i < (int)(sizeof(r)/sizeof(r[0])); i++) {
        r[i] = (uint32_t)(i * 0x9E3779B1u);  // golden-ratio LCG-ish
    }
    seed_rng(r, sizeof(r) / sizeof(r[0]));
    mesh_state_init(0, test_rng);

    uint32_t now = MESH_GRACE_MS;
    mesh_state_tick(now);              // grace -> first role
    uint32_t prev = now;

    for (int i = 0; i < 100; i++) {
        // The FSM stores its deadline internally; we recover the
        // implied dwell by stepping to "deadline minus 1" (no flip)
        // then to "deadline" (flip). That requires knowing the
        // deadline, which we don't — instead we just run for a
        // generous interval and assert that whichever tick triggers a
        // flip is at a delta within the documented range.
        for (uint32_t t = prev + 1; t <= prev + MESH_DWELL_MAX_MS * 2; t++) {
            mesh_action_t a = mesh_state_tick(t);
            if (a == MESH_ACTION_START_ADVERTISING ||
                a == MESH_ACTION_START_SCANNING) {
                uint32_t dwell = t - prev;
                // Floor and ceiling per the implementation comments.
                TEST_ASSERT(dwell >= (uint32_t)(MESH_DWELL_MIN_MS / 2),
                            "dwell respects defensive floor");
                TEST_ASSERT(dwell <= (uint32_t)(MESH_DWELL_MAX_MS *
                                                (100 + MESH_DWELL_JITTER_PCT)
                                                / 100),
                            "dwell respects MAX + JITTER ceiling");
                prev = t;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// test_mesh_alternates_or_repeats — over many flips against varied
// randomness, both ADVERTISING and SCANNING phases should occur. (We
// don't require strict alternation — the role bit is random — but if
// 100 flips all land on the same role we have a bug.)
// ---------------------------------------------------------------------------
void test_mesh_alternates_or_repeats(void) {
    uint32_t r[3 * 200];
    for (int i = 0; i < (int)(sizeof(r)/sizeof(r[0])); i++) {
        // Vary the role bit deliberately by toggling the LSB in slot 0,
        // 3, 6, ... (those slots feed flip_role's role-bit draw).
        r[i] = (i % 3 == 0) ? (uint32_t)(i / 3) : 0xDEADBEEFu + (uint32_t)i;
    }
    seed_rng(r, sizeof(r) / sizeof(r[0]));
    mesh_state_init(0, test_rng);

    int n_adv = 0, n_scan = 0;
    uint32_t now = MESH_GRACE_MS;
    for (int i = 0; i < 100; i++) {
        // Generous fast-forward — way past any plausible dwell.
        now += MESH_DWELL_MAX_MS * 4;
        mesh_action_t a = mesh_state_tick(now);
        if (a == MESH_ACTION_START_ADVERTISING) n_adv++;
        if (a == MESH_ACTION_START_SCANNING)    n_scan++;
    }
    TEST_ASSERT(n_adv > 0, "at least one advertising flip in 100 cycles");
    TEST_ASSERT(n_scan > 0, "at least one scanning flip in 100 cycles");
}

// ---------------------------------------------------------------------------
// test_mesh_companion_freezes_schedule — connection events freeze the
// timer; ticks during CONNECTED return NONE; disconnect resumes mesh
// alternation (NOT grace).
// ---------------------------------------------------------------------------
void test_mesh_companion_freezes_schedule(void) {
    uint32_t r[3 * 20] = { 0 };
    // Make the first post-grace flip ADVERTISING, the post-disconnect
    // flip SCANNING.
    r[0] = 1u;        // role-bit -> ADVERTISING
    r[1] = 0u;        // dwell base
    r[2] = 0u;        // jitter
    r[3] = 0u;        // role-bit -> SCANNING (post-disconnect flip)
    r[4] = 0u;
    r[5] = 0u;
    seed_rng(r, sizeof(r)/sizeof(r[0]));
    mesh_state_init(0, test_rng);

    // Step out of grace → ADVERTISING.
    mesh_action_t a = mesh_state_tick(MESH_GRACE_MS);
    TEST_ASSERT(a == MESH_ACTION_START_ADVERTISING,
                "post-grace lands in ADVERTISING");

    // Companion connects mid-cycle.
    mesh_state_on_connected(MESH_GRACE_MS + 500);
    TEST_ASSERT(mesh_state_phase() == MESH_CONNECTED,
                "phase is CONNECTED after on_connected");

    // Ticks during CONNECTED do nothing, even at far-future timestamps.
    TEST_ASSERT(mesh_state_tick(MESH_GRACE_MS + 999999) == MESH_ACTION_NONE,
                "ticks during CONNECTED return NONE");
    TEST_ASSERT(mesh_state_phase() == MESH_CONNECTED,
                "phase stays CONNECTED through ticks");

    // Disconnect → next tick must emit a fresh role-flip action, and
    // NOT return us to GRACE.
    mesh_state_on_disconnected(MESH_GRACE_MS + 1000000);
    TEST_ASSERT(mesh_state_phase() != MESH_GRACE,
                "disconnect does NOT return to GRACE");

    a = mesh_state_tick(MESH_GRACE_MS + 1000000);
    TEST_ASSERT(a == MESH_ACTION_START_SCANNING,
                "first tick after disconnect emits a real role action");
    TEST_ASSERT(mesh_state_phase() == MESH_SCANNING,
                "post-disconnect phase reflects emitted action");
}

// ---------------------------------------------------------------------------
// test_mesh_grace_skipped_if_companion_connects_immediately — if a
// connect happens during the grace window, on disconnect we resume in
// mesh mode (not back in grace). Documented behaviour from §6.
// ---------------------------------------------------------------------------
void test_mesh_grace_skipped_if_companion_connects_immediately(void) {
    uint32_t r[6] = { 1u, 0u, 0u,    // first flip: ADVERTISING
                      0u, 0u, 0u };  // post-disconnect flip: SCANNING
    seed_rng(r, 6);
    mesh_state_init(0, test_rng);

    // Companion connects during grace.
    mesh_state_on_connected(100);
    TEST_ASSERT(mesh_state_phase() == MESH_CONNECTED,
                "connect during grace → CONNECTED");

    // Disconnect.
    mesh_state_on_disconnected(200);
    TEST_ASSERT(mesh_state_phase() != MESH_GRACE,
                "post-disconnect from in-grace connection: not GRACE");

    // Next tick emits a mesh action.
    mesh_action_t a = mesh_state_tick(200);
    TEST_ASSERT(a == MESH_ACTION_START_SCANNING ||
                a == MESH_ACTION_START_ADVERTISING,
                "next tick emits mesh role action, not NONE");
}

// ---------------------------------------------------------------------------
// test_mesh_jitter_breaks_lockstep — two independent FSMs with
// independent randomness should not stay antiphase forever. Across
// 200 sampled ticks, at least *some* sample must land in a peer-able
// configuration (one ADVERTISING while the other SCANNING). A failure
// here would mean two real devices on a desk could never meet.
// ---------------------------------------------------------------------------
void test_mesh_jitter_breaks_lockstep(void) {
    // Two distinct RNG sequences — that is the property we care about
    // (real devices have independent entropy). Same-seed-but-offset
    // would just reproduce the same role schedule shifted in time, and
    // when sampled at matching offsets they'd appear identical even
    // though the underlying entropy is shared.
    uint32_t r_a[3 * 200];
    uint32_t r_b[3 * 200];
    for (int i = 0; i < (int)(sizeof(r_a)/sizeof(r_a[0])); i++) {
        r_a[i] = 0xA5A5A5A5u ^ (uint32_t)(i * 17);
        r_b[i] = 0x5A5A5A5Au ^ (uint32_t)(i * 23);
    }

    int sample_steps = 200;
    int step_ms = 100;
    mesh_phase_t phases_a[200];

    // FSM A: capture its phase at each sample tick.
    seed_rng(r_a, sizeof(r_a)/sizeof(r_a[0]));
    mesh_state_init(0, test_rng);
    uint32_t now = MESH_GRACE_MS;
    for (int i = 0; i < sample_steps; i++) {
        mesh_state_tick(now);
        phases_a[i] = mesh_state_phase();
        now += step_ms;
    }

    // FSM B: independent randomness, sample at the SAME absolute clock
    // values as A so we're comparing what's true at one moment in real
    // time, not at matching offsets in each FSM's own schedule.
    seed_rng(r_b, sizeof(r_b)/sizeof(r_b[0]));
    mesh_state_init(0, test_rng);
    int meet = 0;
    now = MESH_GRACE_MS;
    for (int i = 0; i < sample_steps; i++) {
        mesh_state_tick(now);
        mesh_phase_t pb = mesh_state_phase();
        if ((phases_a[i] == MESH_ADVERTISING && pb == MESH_SCANNING) ||
            (phases_a[i] == MESH_SCANNING    && pb == MESH_ADVERTISING)) {
            meet++;
        }
        now += step_ms;
    }

    TEST_ASSERT(meet > 0,
                "two independently-seeded FSMs land in a peer-able "
                "configuration at least once across 200 samples");
}
