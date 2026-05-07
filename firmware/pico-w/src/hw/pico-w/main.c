#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "core/identity.h"
#include "core/attest_cache.h"
#include "core/attestations.h"
#include "core/cborencode.h"
#include "core/constants.h"
#include "core/encounter_record.h"
#include "core/encounter_session.h"
#include "core/encounters.h"
#include "core/filestore.h"
#include "core/commands.h"
#include "core/hal_crypto.h"
#include "core/manifest_counter.h"
#include "core/meeting_count.h"
#include "core/mesh_state.h"
#include "core/peer_ledger.h"
#include "core/peer_sync.h"
#include "core/peer_table.h"
#include "core/ssid_scan.h"
#include "hw/pico-w/ble.h"
#include "hw/pico-w/ble_client.h"
#include "hw/pico-w/ssid_scan_pico.h"

// LED blink intervals (ms)
#define LED_GRACE_MS         50      // very fast pulse during boot grace window
#define LED_ADVERTISING_MS   1000    // slow blink while advertising
#define LED_CONNECTED_MS     100     // fast blink while connected

static bool     led_state         = false;
static uint32_t last_led_toggle   = 0;

// Scanning gets a small bit pattern (double-blink) so it's visually
// distinct from advertising. Implemented as a 4-step phase counter with
// each step lasting LED_SCANNING_STEP_MS.
#define LED_SCANNING_STEP_MS 100
static uint8_t  led_scan_step     = 0;
static uint32_t led_scan_step_t   = 0;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint32_t mesh_rng(void) {
    uint32_t v = 0;
    if (!hal_random_bytes((uint8_t *)&v, sizeof(v))) {
        // Fallback: stir in the clock so we still pick *something*
        // unpredictable rather than always 0. Identity init has
        // already burned the real RNG by this point and shouldn't
        // fail again, but be defensive.
        v = now_ms() ^ 0x9E3779B1u;
    }
    return v;
}

// ============================================================
// LED driver — distinguishes mesh phase visually.
// ============================================================
static void led_update(void) {
    uint32_t t = now_ms();
    mesh_phase_t phase = mesh_state_phase();

    switch (phase) {
        case MESH_GRACE:
            if (t - last_led_toggle >= LED_GRACE_MS) {
                led_state = !led_state;
                last_led_toggle = t;
                ble_set_led(led_state);
            }
            break;

        case MESH_ADVERTISING:
            if (t - last_led_toggle >= LED_ADVERTISING_MS) {
                led_state = !led_state;
                last_led_toggle = t;
                ble_set_led(led_state);
            }
            break;

        case MESH_SCANNING:
            // Double-blink: on, off, on, long-off — total ~1 s cycle.
            if (t - led_scan_step_t >= LED_SCANNING_STEP_MS) {
                led_scan_step_t = t;
                bool on = (led_scan_step == 0 || led_scan_step == 2);
                ble_set_led(on);
                led_scan_step = (led_scan_step + 1) & 0x07;
            }
            break;

        case MESH_CONNECTED:
            if (t - last_led_toggle >= LED_CONNECTED_MS) {
                led_state = !led_state;
                last_led_toggle = t;
                ble_set_led(led_state);
            }
            break;
    }
}

// ============================================================
// Identity + encounter orchestration shared state.
// ============================================================

static waxwing_identity_t g_identity;
static bool               g_identity_ready = false;

static uint8_t resp_buf[512];

// ============================================================
// Encounter handshake helpers.
// ============================================================

// Late-bind callback for the responder path: looks up the now-known
// peer pub in peer_ledger and copies its lifetime totals + reputation
// into the per-peer struct. Returning false (no entry) leaves the
// fields at zero, which encounter_session interprets as first contact.
static bool peer_ledger_late_bind(const uint8_t peer_pub[ENCOUNTER_PUB_BYTES],
                                  encounter_per_peer_input_t *out, void *ctx) {
    (void)ctx;
    peer_ledger_entry_t e;
    if (!peer_ledger_get(peer_pub, &e)) return false;
    out->rep_of_peer                    = e.my_rep_score;
    out->tx_bytes_to_peer_lifetime      = e.tx_bytes_lifetime;
    out->rx_bytes_from_peer_lifetime    = e.rx_bytes_lifetime;
    out->file_count_from_peer_lifetime  = e.file_count_lifetime;
    return true;
}

// Build the local input the encounter session needs from this node's
// identity, current meeting count, and latest BSSID scan. `expected`
// is the 8-byte TPK prefix we believe the peer advertises; pass NULL
// when no such prefix is known.
//
// Today the BLE advertisement doesn't carry the TPK prefix — the
// `tpk_prefix` field on `ble_client_peer_t` is the BD address bytes
// in disguise (a peer_table uniqueness key, not a real TPK), so both
// roles currently call this with NULL. The `if (expected)` branch is
// kept in place for when ads start carrying the actual 8-byte TPK
// prefix; it lights up the per-peer pre-population on the initiator
// side without any further changes.
//
// Responder side: we install a late_bind callback that encounter_
// session invokes after PROPOSE arrives, when the peer's full pub
// is known. That's how the responder gets accurate lifetime fields
// from peer_ledger today (initiator stays at zeros until ads carry
// TPK). Genuinely-new peers always carry zeros, which is the correct
// first-contact state.
static void build_local_input(encounter_local_input_t *me,
                              const uint8_t *expected) {
    memset(me, 0, sizeof(*me));
    memcpy(me->pub,  g_identity.pub,  ENCOUNTER_PUB_BYTES);
    memcpy(me->seed, g_identity.seed, ENCOUNTER_PUB_BYTES);
    hal_random_bytes(me->nonce, ENCOUNTER_NONCE_BYTES);
    if (expected) {
        memcpy(me->expected_peer_prefix, expected, 8);
    }
    me->meeting_count = meeting_count_get();

    const ssid_scan_t *scan = pico_ssid_scan_latest();
    if (scan) {
        uint8_t n = scan->count;
        if (n > ENCOUNTER_BSSID_MAX) n = ENCOUNTER_BSSID_MAX;
        me->bssids_count = n;
        for (uint8_t i = 0; i < n; i++) {
            memcpy(me->bssids[i], scan->obs[i].bssid, 6);
        }
    }

    if (expected) {
        peer_ledger_entry_t e;
        if (peer_ledger_get_by_prefix(expected, &e)) {
            me->tx_bytes_to_peer_lifetime     = e.tx_bytes_lifetime;
            me->rx_bytes_from_peer_lifetime   = e.rx_bytes_lifetime;
            me->file_count_from_peer_lifetime = e.file_count_lifetime;
            me->rep_of_peer                    = e.my_rep_score;
        }
    } else {
        me->late_bind     = peer_ledger_late_bind;
        me->late_bind_ctx = NULL;
    }
}

// Persist a completed encounter record under /files/ so cmd_ls / iOS
// can pick it up. Filename is `enc_<16-hex>.cbor` — first 8 bytes of
// the 16-byte content-addressed encounter id, plenty of collision
// margin for the ENCOUNTERS_CAP-bounded record set we keep on-device.
//
// Bumps meeting_count and updates peer_ledger for the *other* party
// in one shot, since DONE on the encounter session is the canonical
// "we just had an encounter" signal.
static void on_encounter_done(const encounter_record_t *rec, bool we_are_a) {
    uint8_t id[ENCOUNTER_ID_BYTES];
    encounter_record_id(rec, id);

    char name[FS_MAX_NAME_LEN];
    int n = snprintf(name, sizeof(name),
                     "enc_%02x%02x%02x%02x%02x%02x%02x%02x.cbor",
                     id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
    (void)n;

    // 512-byte buffer kept off the BLE callback's stack frame — same
    // motivation as the matching helper in cardputer/src/main.cpp.
    static uint8_t enc_persist_buf[ENCOUNTER_RECORD_MAX_BYTES];
    size_t blob_len = encounter_record_encode_full(rec, enc_persist_buf,
                                                    sizeof(enc_persist_buf));
    if (blob_len > 0) {
        if (fs_write(name, enc_persist_buf, blob_len) == 0) {
            manifest_counter_bump();
            ble_set_manifest_version(manifest_counter_get());
        } else {
            printf("[enc] fs_write failed for %s\r\n", name);
        }
    }

    meeting_count_bump();

    const uint8_t *peer_pub = we_are_a ? rec->pub_b : rec->pub_a;
    uint64_t peer_count    = we_are_a ? rec->meeting_count_b
                                       : rec->meeting_count_a;
    peer_ledger_apply(peer_pub, /*tx*/0, /*rx*/0, /*files*/0, peer_count);

    printf("[enc] DONE id=%02x%02x%02x%02x peer=%02x%02x%02x%02x\r\n",
           id[0], id[1], id[2], id[3],
           peer_pub[0], peer_pub[1], peer_pub[2], peer_pub[3]);
}

// Build a small `{ ok: true }` ACK sent by the responder after CONFIRM
// verifies, or `{ error: "<msg>" }` on failure. Same shape as cmd_*
// responses so the existing CBOR-parsing client recognizes it.
static int build_ok_ack(uint8_t *out, size_t out_max) {
    if (out_max < 8) return -1;
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    return (int)(p - out);
}

static int build_error_ack(uint8_t *out, size_t out_max, const char *msg) {
    if (out_max < 32) return -1;
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "error", 5);
    size_t mlen = strlen(msg);
    if (mlen > 24) mlen = 24;
    p += cborencode_text_str(p, msg, mlen);
    return (int)(p - out);
}

// ============================================================
// Inbound responder-side encounter session + companion path.
// ============================================================

static encounter_session_t g_resp_session;
static bool                g_resp_armed   = false;
static bool                g_resp_done    = false;

// Per-connection byte counters for the responder side. Track every
// peer-characteristic byte that flows during the connection (handshake
// included). At disconnect, if we know the peer's full pub from the
// completed encounter, we apply the deltas to peer_ledger so the next
// session between us populates the lifetime fields with real numbers.
//
// Companion-characteristic bytes are NOT counted — peer_ledger tracks
// peer-to-peer relationships only.
static uint64_t g_inbound_peer_tx = 0;
static uint64_t g_inbound_peer_rx = 0;
static uint8_t  g_inbound_peer_pub[ENCOUNTER_PUB_BYTES];
static bool     g_inbound_peer_pub_known = false;

static void reset_responder_session(void) {
    memset(&g_resp_session, 0, sizeof(g_resp_session));
    g_resp_armed = false;
    g_resp_done  = false;
    g_inbound_peer_tx        = 0;
    g_inbound_peer_rx        = 0;
    g_inbound_peer_pub_known = false;
}

// Send a response on the file_response characteristic AND tally the
// payload toward the per-session peer rx/tx counters (responder ->
// peer = our tx). Use this any time the responder side sends bytes
// to a peer over the notification channel.
static void send_peer_response(const uint8_t *data, size_t len) {
    ble_send_file_response(data, len);
    g_inbound_peer_tx += (uint64_t)len;
}

static void arm_responder_session(void) {
    if (!g_identity_ready) return;
    encounter_local_input_t me;
    build_local_input(&me, /*expected=*/NULL);

    uint8_t  scratch[ENCOUNTER_MSG_MAX_BYTES];
    size_t   olen = 0;
    encounter_step_t step = encounter_session_start(
        &g_resp_session, ENCOUNTER_ROLE_RESPONDER, &me,
        scratch, sizeof(scratch), &olen);
    if (step == ENCOUNTER_STEP_NEED_READ) {
        g_resp_armed = true;
    } else {
        printf("[enc] responder start refused (step=%d)\r\n", (int)step);
    }
}

// Companion-path writes (CHAR_FILE_COMMAND).
static void on_file_command(const uint8_t *data, size_t len) {
    int resp_len = commands_handle(data, len, resp_buf, sizeof(resp_buf));
    if (resp_len > 0 && ble_is_connected()) {
        ble_send_file_response(resp_buf, (size_t)resp_len);
    }
    ble_set_manifest_version(manifest_counter_get());
}

// Peer-path writes (CHAR_PEER_COMMAND). Pre-DONE bytes feed the
// encounter session; post-DONE bytes route through commands_handle in
// PEER mode. Every byte exchanged is tallied to the per-session
// counter for application to peer_ledger at disconnect.
static void on_peer_command(const uint8_t *data, size_t len) {
    g_inbound_peer_rx += (uint64_t)len;

    if (!g_resp_armed) {
        // Connect raced ahead of identity load, or session was torn
        // down. Try to arm now so the first PROPOSE doesn't fall on
        // the floor.
        arm_responder_session();
        if (!g_resp_armed) {
            int n = build_error_ack(resp_buf, sizeof(resp_buf), "no session");
            if (n > 0) send_peer_response(resp_buf, (size_t)n);
            return;
        }
    }

    if (g_resp_done) {
        int rc = commands_handle_session(COMMANDS_SESSION_PEER, data, len,
                                          resp_buf, sizeof(resp_buf));
        if (rc > 0 && ble_is_connected()) {
            send_peer_response(resp_buf, (size_t)rc);
        }
        ble_set_manifest_version(manifest_counter_get());
        return;
    }

    // Encounter handshake in progress.
    size_t out_len = 0;
    encounter_step_t step = encounter_session_handle(
        &g_resp_session, data, len,
        resp_buf, sizeof(resp_buf), &out_len);

    if (step == ENCOUNTER_STEP_NEED_WRITE) {
        send_peer_response(resp_buf, out_len);
        return;
    }
    if (step == ENCOUNTER_STEP_DONE) {
        encounter_record_t rec;
        if (encounter_session_take_record(&g_resp_session, &rec)) {
            on_encounter_done(&rec, /*we_are_a=*/false);
            // We are the B side; the peer is A.
            memcpy(g_inbound_peer_pub, rec.pub_a, ENCOUNTER_PUB_BYTES);
            g_inbound_peer_pub_known = true;
        }
        g_resp_done = true;
        int n = build_ok_ack(resp_buf, sizeof(resp_buf));
        if (n > 0) send_peer_response(resp_buf, (size_t)n);
        return;
    }
    // ERROR
    int n = build_error_ack(resp_buf, sizeof(resp_buf), "handshake failed");
    if (n > 0) send_peer_response(resp_buf, (size_t)n);
    reset_responder_session();
}

// ============================================================
// Outbound initiator-side encounter session + peer_sync glue.
// ============================================================

typedef enum {
    OUT_IDLE,
    OUT_HANDSHAKE,            // PROPOSE sent, awaiting ACCEPT
    OUT_HANDSHAKE_CONFIRMED,  // CONFIRM sent, awaiting OK ACK
    OUT_SYNCING,              // peer_sync running
} outbound_phase_t;

static outbound_phase_t     g_out_phase = OUT_IDLE;
static encounter_session_t  g_init_session;
static peer_sync_session_t *g_sync = NULL;
static uint8_t              g_sync_peer_tpk[8];
static uint8_t              g_sync_peer_version;
static uint8_t              g_sync_buf[256];

// Per-session byte counters for the outbound connection. Same shape
// as the responder counters; applied to peer_ledger at disconnect.
static uint64_t g_outbound_tx = 0;
static uint64_t g_outbound_rx = 0;
static uint8_t  g_outbound_peer_pub[ENCOUNTER_PUB_BYTES];
static bool     g_outbound_peer_pub_known = false;

// Wrap ble_client_send_command so every outbound write is tallied
// toward the per-session counter. Returns the underlying result so
// callers can react to send failures.
static bool send_outbound(const uint8_t *data, size_t len) {
    bool ok = ble_client_send_command(data, len);
    if (ok) g_outbound_tx += (uint64_t)len;
    return ok;
}

static void start_peer_sync(void) {
    size_t           out_len = 0;
    peer_sync_step_t step    = PEER_SYNC_ERROR;
    g_sync = peer_sync_start(g_sync_peer_tpk, g_sync_buf, sizeof(g_sync_buf),
                             &out_len, &step);
    if (!g_sync || step != PEER_SYNC_NEED_WRITE) {
        printf("[mesh] peer_sync_start refused (step=%d); disconnecting\r\n",
               (int)step);
        ble_client_disconnect();
        return;
    }
    g_out_phase = OUT_SYNCING;
    if (!send_outbound(g_sync_buf, out_len)) {
        printf("[mesh] first sync send failed; disconnecting\r\n");
        ble_client_disconnect();
    }
}

static void on_peer_seen(const ble_client_peer_t *peer) {
    if (peer_table_decide(peer->tpk_prefix, peer->manifest_version,
                          now_ms()) == PEER_DECISION_SKIP) {
        return;
    }
    memcpy(g_sync_peer_tpk, peer->tpk_prefix, 8);
    g_sync_peer_version = peer->manifest_version;
    g_outbound_tx              = 0;
    g_outbound_rx              = 0;
    g_outbound_peer_pub_known  = false;
    ble_client_connect(peer->bd_addr, peer->bd_addr_type);
}

static void on_client_connected(void) {
    mesh_state_on_connected(now_ms());

    // Backward-compat: pre-M4 peers don't expose CHAR_PEER_COMMAND.
    // Fall straight through to peer_sync as before, no handshake.
    if (!ble_client_uses_peer_characteristic()) {
        printf("[mesh] peer has no peer_cmd; legacy peer_sync path\r\n");
        start_peer_sync();
        return;
    }

    if (!g_identity_ready) {
        printf("[mesh] no identity; skipping handshake\r\n");
        ble_client_disconnect();
        return;
    }

    // Pass NULL: ble_client populates `g_sync_peer_tpk` from the
    // peer's BD address, not from a TPK prefix in the advertisement
    // (the ad doesn't carry one yet — see parse_advertising_report).
    // Until ads expose the TPK prefix, the initiator can't pre-bind a
    // real expected-peer-prefix or a peer_ledger lookup, so we treat
    // every outbound encounter as first-contact-style on our side.
    encounter_local_input_t me;
    build_local_input(&me, NULL);

    uint8_t  scratch[ENCOUNTER_MSG_MAX_BYTES];
    size_t   olen = 0;
    encounter_step_t step = encounter_session_start(
        &g_init_session, ENCOUNTER_ROLE_INITIATOR, &me,
        scratch, sizeof(scratch), &olen);
    if (step != ENCOUNTER_STEP_NEED_WRITE) {
        printf("[enc] initiator start refused (step=%d)\r\n", (int)step);
        ble_client_disconnect();
        return;
    }
    g_out_phase = OUT_HANDSHAKE;
    if (!send_outbound(scratch, olen)) {
        printf("[enc] PROPOSE send failed; disconnecting\r\n");
        ble_client_disconnect();
    }
}

static void on_client_response(const uint8_t *data, size_t len) {
    g_outbound_rx += (uint64_t)len;
    switch (g_out_phase) {
        case OUT_HANDSHAKE: {
            // Expecting ACCEPT.
            uint8_t scratch[ENCOUNTER_MSG_MAX_BYTES];
            size_t  olen = 0;
            encounter_step_t step = encounter_session_handle(
                &g_init_session, data, len,
                scratch, sizeof(scratch), &olen);
            if (step != ENCOUNTER_STEP_NEED_WRITE) {
                printf("[enc] ACCEPT handle failed (step=%d)\r\n", (int)step);
                ble_client_disconnect();
                return;
            }
            // CONFIRM bytes ready. Local session is at DONE the moment
            // we send. Persist the record now (the responder has
            // committed via sig_b too); the OK ACK is just a wire-level
            // synchronization point so we don't race subsequent writes.
            encounter_record_t rec;
            if (encounter_session_take_record(&g_init_session, &rec)) {
                on_encounter_done(&rec, /*we_are_a=*/true);
                // We are A; the peer is B.
                memcpy(g_outbound_peer_pub, rec.pub_b, ENCOUNTER_PUB_BYTES);
                g_outbound_peer_pub_known = true;
            }
            g_out_phase = OUT_HANDSHAKE_CONFIRMED;
            if (!send_outbound(scratch, olen)) {
                printf("[enc] CONFIRM send failed; disconnecting\r\n");
                ble_client_disconnect();
            }
            return;
        }
        case OUT_HANDSHAKE_CONFIRMED:
            // Expecting OK ACK. We don't strictly need to inspect it —
            // arrival means the responder accepted CONFIRM. On any
            // shape we proceed to peer_sync; an "error" from the
            // responder surfaces as a peer_sync transport failure on
            // the next exchange anyway.
            (void)data; (void)len;
            start_peer_sync();
            return;
        case OUT_SYNCING: {
            if (!g_sync) {
                printf("[mesh] WARN: response (%zu bytes) with no sync\r\n", len);
                return;
            }
            size_t           out_len = 0;
            peer_sync_step_t step = peer_sync_handle_response(g_sync, data, len,
                                                               g_sync_buf,
                                                               sizeof(g_sync_buf),
                                                               &out_len);
            if (step == PEER_SYNC_NEED_WRITE) {
                if (!send_outbound(g_sync_buf, out_len)) {
                    printf("[mesh] send_command failed; disconnecting\r\n");
                    ble_client_disconnect();
                }
                return;
            }
            printf("[mesh] sync %s\r\n", step == PEER_SYNC_DONE ? "done" : "error");
            peer_table_record_sync(g_sync_peer_tpk, g_sync_peer_version,
                                   (step == PEER_SYNC_DONE)
                                        ? PEER_SYNC_RESULT_SUCCESS
                                        : PEER_SYNC_RESULT_FAILED,
                                   now_ms());
            ble_set_manifest_version(manifest_counter_get());
            ble_client_disconnect();
            return;
        }
        case OUT_IDLE:
        default:
            printf("[mesh] WARN: unexpected response in IDLE\r\n");
            return;
    }
}

static void on_client_disconnected(void) {
    if (g_outbound_peer_pub_known &&
        (g_outbound_tx > 0 || g_outbound_rx > 0)) {
        peer_ledger_apply(g_outbound_peer_pub,
                          g_outbound_tx,
                          g_outbound_rx,
                          /*file_delta=*/0,
                          /*peer_meeting_count_seen=*/0);
    }
    if (g_sync) {
        peer_sync_end(g_sync);
        g_sync = NULL;
    }
    g_out_phase = OUT_IDLE;
    memset(&g_init_session, 0, sizeof(g_init_session));
    g_outbound_peer_pub_known = false;
    g_outbound_tx = 0;
    g_outbound_rx = 0;
    mesh_state_on_disconnected(now_ms());
}

// ============================================================
// Inbound (peripheral) connect / disconnect.
// ============================================================

static void on_inbound_connected(uint16_t conn_handle) {
    (void)conn_handle;
    // Arm a fresh responder session on each connection. Companion
    // connections never write to the peer characteristic, so the armed
    // session sits unused and gets reset on disconnect — harmless.
    arm_responder_session();
    mesh_state_on_connected(now_ms());
}

static void on_inbound_disconnected(uint16_t conn_handle) {
    (void)conn_handle;
    // If the encounter completed during this connection we know the
    // peer's full pub; apply the byte deltas accumulated since connect.
    // peer_meeting_count_seen=0 means "don't update" (the on_encounter
    // path already wrote the up-to-date count).
    if (g_inbound_peer_pub_known &&
        (g_inbound_peer_tx > 0 || g_inbound_peer_rx > 0)) {
        peer_ledger_apply(g_inbound_peer_pub,
                          g_inbound_peer_tx,
                          g_inbound_peer_rx,
                          /*file_delta=*/0,
                          /*peer_meeting_count_seen=*/0);
    }
    reset_responder_session();
    mesh_state_on_disconnected(now_ms());
}

// ============================================================
// Run-loop tick: react to mesh_state actions.
// ============================================================

static void apply_mesh_action(mesh_action_t a) {
    switch (a) {
        case MESH_ACTION_NONE:
            break;
        case MESH_ACTION_START_ADVERTISING:
            ble_client_stop_scan();
            // The peripheral path keeps advertising on its own after
            // disconnect; calling start here ensures we re-enable if
            // we'd been scanning.
            ble_start_advertising();
            break;
        case MESH_ACTION_START_SCANNING:
            ble_stop_advertising();
            ble_client_start_scan();
            break;
        case MESH_ACTION_STOP_BOTH:
            // Connected — both adverts and scans are off (ble.c stops
            // adverts on connect; ble_client only scans while we ask).
            ble_client_stop_scan();
            break;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(500);

    printf("\r\nWAXWING C-FIRMWARE v2.2 (mesh phase)\r\n\r\n");

    if (fs_init() < 0) {
        printf("[main] FATAL: fs_init failed\r\n");
        while (1) { __asm__("nop"); }
    }
    printf("[main] Filesystem ready\r\n");

    if (!waxwing_identity_load_or_generate(&g_identity)) {
        printf("[main] FATAL: identity init failed\r\n");
        while (1) { __asm__("nop"); }
    }
    g_identity_ready = true;
    printf("[main] Node name: %s\r\n", g_identity.node_name);

    manifest_counter_init();
    meeting_count_init();
    peer_ledger_init();
    peer_table_init();
    printf("[main] manifest_version=%u meeting_count=%llu peers=%d\r\n",
           (unsigned)manifest_counter_get(),
           (unsigned long long)meeting_count_get(),
           peer_ledger_count());

    if (!ble_init()) {
        printf("[main] FATAL: BLE init failed\r\n");
        while (1) { __asm__("nop"); }
    }

    waxwing_device_identity_t ble_identity;
    ble_identity.protocol_version = 1;
    snprintf(ble_identity.protocol_name, sizeof(ble_identity.protocol_name), "%s", PROTOCOL_NAME);
    snprintf(ble_identity.node_name, sizeof(ble_identity.node_name), "%s", g_identity.node_name);
    memcpy(ble_identity.tpk, g_identity.pub, 32);
    snprintf(ble_identity.tpk_hex, sizeof(ble_identity.tpk_hex), "%s", g_identity.tpk_hex);
    memcpy(ble_identity.tpk_fingerprint, g_identity.fingerprint, 8);
    ble_identity.capabilities  = PICO_W_CAPS;
    ble_identity.manifest_count = 0;
    ble_identity.mode          = CAP_UNATTENDED;
    ble_set_identity(&ble_identity);

    // Seed the advertised counter before the first advertisement frame
    // hits the air.
    ble_set_manifest_version(manifest_counter_get());

    ble_set_on_write(on_file_command);
    ble_set_on_peer_write(on_peer_command);
    ble_set_on_connect(on_inbound_connected);
    ble_set_on_disconnect(on_inbound_disconnected);

    ble_client_init();
    ble_client_set_on_peer_seen(on_peer_seen);
    ble_client_set_on_connected(on_client_connected);
    ble_client_set_on_response(on_client_response);
    ble_client_set_on_disconnected(on_client_disconnected);

    mesh_state_init(now_ms(), mesh_rng);

    if (!pico_ssid_scan_init()) {
        printf("[main] WARN: ssid_scan init failed (BLE will still work)\r\n");
    }
    encounters_init();
    attestations_init();
    attest_cache_init();
    printf("[main] encounters=%d attestations=%d cached=%d\r\n",
           encounters_count(), attestations_count(), attest_cache_count());

    printf("[main] Ready, entering run loop...\r\n\r\n");

    // M4 stage 9: solo SSID-self-attestation cadence retired. Encounters
    // are now produced exclusively by the two-party handshake on the
    // peer characteristic (encounter_session.c). The v1 store stays
    // readable via cmd_encounters_get for iOS until stage 8 lands; we
    // just stop adding new records to it.
    while (true) {
        ble_process();
        apply_mesh_action(mesh_state_tick(now_ms()));
        pico_ssid_scan_tick(now_ms(), mesh_state_phase() == MESH_CONNECTED);

        led_update();
    }
}
