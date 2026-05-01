#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "core/identity.h"
#include "core/attest_cache.h"
#include "core/attestations.h"
#include "core/constants.h"
#include "core/encounters.h"
#include "core/filestore.h"
#include "core/commands.h"
#include "core/hal_crypto.h"
#include "core/manifest_counter.h"
#include "core/mesh_state.h"
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
// Peripheral file-command path (companion app + inbound peers).
// ============================================================

static uint8_t resp_buf[512];

static void on_file_command(const uint8_t *data, size_t len) {
    int resp_len = commands_handle(data, len, resp_buf, sizeof(resp_buf));
    if (resp_len > 0 && ble_is_connected()) {
        ble_send_file_response(resp_buf, (size_t)resp_len);
    }
    ble_set_manifest_version(manifest_counter_get());
}

// ============================================================
// Mesh-mode driver: peer_sync session lifetime + the ble_client glue.
// ============================================================

static peer_sync_session_t *g_sync = NULL;
static uint8_t              g_sync_peer_tpk[8];
static uint8_t              g_sync_peer_version;
static uint8_t              g_sync_buf[256];

static void on_peer_seen(const ble_client_peer_t *peer) {
    // Decide: connect or skip?
    if (peer_table_decide(peer->tpk_prefix, peer->manifest_version,
                          now_ms()) == PEER_DECISION_SKIP) {
        return;
    }
    // Stop scanning while we attempt the connection. Cache the peer
    // we're about to talk to so the on_connected handler can start a
    // session against the right TPK / version.
    memcpy(g_sync_peer_tpk, peer->tpk_prefix, 8);
    g_sync_peer_version = peer->manifest_version;
    ble_client_connect(peer->bd_addr, peer->bd_addr_type);
}

static void on_client_connected(void) {
    mesh_state_on_connected(now_ms());

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
    if (!ble_client_send_command(g_sync_buf, out_len)) {
        printf("[mesh] first send failed; disconnecting\r\n");
        ble_client_disconnect();
    }
}

static void on_client_response(const uint8_t *data, size_t len) {
    if (!g_sync) {
        printf("[mesh] WARN: response (%zu bytes) with no active session\r\n", len);
        return;
    }
    size_t           out_len = 0;
    peer_sync_step_t step = peer_sync_handle_response(g_sync, data, len,
                                                       g_sync_buf,
                                                       sizeof(g_sync_buf),
                                                       &out_len);
    if (step == PEER_SYNC_NEED_WRITE) {
        if (!ble_client_send_command(g_sync_buf, out_len)) {
            printf("[mesh] send_command failed; disconnecting\r\n");
            ble_client_disconnect();
        }
        return;
    }
    // DONE or ERROR — record the result and disconnect.
    printf("[mesh] sync %s\r\n", step == PEER_SYNC_DONE ? "done" : "error");
    peer_table_record_sync(g_sync_peer_tpk, g_sync_peer_version,
                           (step == PEER_SYNC_DONE)
                                ? PEER_SYNC_RESULT_SUCCESS
                                : PEER_SYNC_RESULT_FAILED,
                           now_ms());
    ble_set_manifest_version(manifest_counter_get());
    ble_client_disconnect();
}

static void on_client_disconnected(void) {
    if (g_sync) {
        peer_sync_end(g_sync);
        g_sync = NULL;
    }
    mesh_state_on_disconnected(now_ms());
}

// ============================================================
// Inbound (peripheral) connect / disconnect just feed the FSM.
// ============================================================

static void on_inbound_connected(uint16_t conn_handle) {
    (void)conn_handle;
    mesh_state_on_connected(now_ms());
}

static void on_inbound_disconnected(uint16_t conn_handle) {
    (void)conn_handle;
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

    waxwing_identity_t identity;
    if (!waxwing_identity_load_or_generate(&identity)) {
        printf("[main] FATAL: identity init failed\r\n");
        while (1) { __asm__("nop"); }
    }
    printf("[main] Node name: %s\r\n", identity.node_name);

    manifest_counter_init();
    peer_table_init();
    printf("[main] manifest_version=%u\r\n",
           (unsigned)manifest_counter_get());

    if (!ble_init()) {
        printf("[main] FATAL: BLE init failed\r\n");
        while (1) { __asm__("nop"); }
    }

    waxwing_device_identity_t ble_identity;
    ble_identity.protocol_version = 1;
    snprintf(ble_identity.protocol_name, sizeof(ble_identity.protocol_name), "%s", PROTOCOL_NAME);
    snprintf(ble_identity.node_name, sizeof(ble_identity.node_name), "%s", identity.node_name);
    memcpy(ble_identity.tpk, identity.pub, 32);
    snprintf(ble_identity.tpk_hex, sizeof(ble_identity.tpk_hex), "%s", identity.tpk_hex);
    memcpy(ble_identity.tpk_fingerprint, identity.fingerprint, 8);
    ble_identity.capabilities  = PICO_W_CAPS;
    ble_identity.manifest_count = 0;
    ble_identity.mode          = CAP_UNATTENDED;
    ble_set_identity(&ble_identity);

    // Seed the advertised counter before the first advertisement frame
    // hits the air.
    ble_set_manifest_version(manifest_counter_get());

    ble_set_on_write(on_file_command);
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

    static uint32_t s_last_scanned_ms = 0;

    printf("[main] Ready, entering run loop...\r\n\r\n");

    while (true) {
        ble_process();
        apply_mesh_action(mesh_state_tick(now_ms()));
        pico_ssid_scan_tick(now_ms(), mesh_state_phase() == MESH_CONNECTED);

        const ssid_scan_t *scan = pico_ssid_scan_latest();
        if (scan && scan->scanned_ms != s_last_scanned_ms) {
            s_last_scanned_ms = scan->scanned_ms;
            if (encounters_should_record(scan, scan->scanned_ms, 600000)) {
                if (encounters_record(scan, scan->scanned_ms,
                                      identity.pub, identity.seed) == 0) {
                    printf("[main] encounter recorded (%d total)\r\n",
                           encounters_count());
                }
            }
        }

        led_update();
    }
}
