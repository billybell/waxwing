#include <M5Cardputer.h>

#include "hw/ble.h"
#include "hw/ble_client.h"
#include "hw/ssid_scan_esp32.h"

extern "C" {
#include "core/attest_cache.h"
#include "core/attestations.h"
#include "core/cborencode.h"
#include "core/commands.h"
#include "core/encounters.h"
#include "core/filestore.h"
#include "core/hal_crypto.h"
#include "core/identity.h"
#include "core/manifest_counter.h"
#include "core/mesh_state.h"
#include "core/peer_sync.h"
#include "core/peer_table.h"
#include "core/ssid_scan.h"
#include "core/ssid_scan_hal.h"
}

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdio>
#include <cstring>

namespace {

bool     g_was_connected   = false;
uint16_t g_last_conn_handle = 0xFFFF;
size_t   g_last_write_len   = 0;
uint32_t g_last_resp_len    = 0;
uint32_t g_dropped_cmds     = 0;
bool     g_sd_ready         = false;

uint8_t g_resp_buf[512];

waxwing_identity_t g_identity;
bool               g_identity_ready = false;

// Inbound File Command queue (NimBLE host task -> main loop).
struct CmdEntry {
    size_t  len;
    uint8_t data[512];
};
constexpr size_t kCmdQueueDepth = 4;
QueueHandle_t g_cmd_queue = nullptr;

// Outbound peer-sync session state (Pico W's main.c structure).
peer_sync_session_t *g_sync                = nullptr;
uint8_t              g_sync_peer_tpk[8]    = {};
uint8_t              g_sync_peer_version   = 0;
uint8_t              g_sync_buf[256]       = {};
uint32_t             g_syncs_completed     = 0;
// Set when on_peer_seen kicks off a connect; cleared once the
// session result is recorded in peer_table. If we hit
// on_client_disconnected with this still set, the attempt failed
// before we could record — we record FAILED so the backoff window
// kicks in and we don't immediately retry the same peer.
bool                 g_sync_pending        = false;

uint32_t now_ms() {
    return static_cast<uint32_t>(millis());
}

uint32_t mesh_rng() {
    return esp_random();
}

void publish_identity() {
    if (!g_identity_ready) return;

    const uint8_t *tpk = g_identity.pub;

    uint8_t buf[BLE_MAX_DATA_SIZE];
    size_t  pos = 0;

    pos += cborencode_map_header(buf + pos, 10);

    pos += cborencode_text_str(buf + pos, "protocol", 8);
    pos += cborencode_text_str(buf + pos, "waxwing-mesh", 12);

    pos += cborencode_text_str(buf + pos, "v", 1);
    pos += cborencode_uint(buf + pos, 0);

    const size_t name_len = std::strlen(g_identity.node_name);
    pos += cborencode_text_str(buf + pos, "name", 4);
    pos += cborencode_text_str(buf + pos, g_identity.node_name, name_len);

    pos += cborencode_text_str(buf + pos, "tpk", 3);
    pos += cborencode_byte_str(buf + pos, tpk, 32);

    pos += cborencode_text_str(buf + pos, "caps", 4);
    pos += cborencode_uint(buf + pos, 0);

    pos += cborencode_text_str(buf + pos, "firmware", 8);
    pos += cborencode_text_str(buf + pos, "cardputer", 9);

    pos += cborencode_text_str(buf + pos, "firmware_ver", 12);
    pos += cborencode_text_str(buf + pos, "0.1.0", 5);

    pos += cborencode_text_str(buf + pos, "attended", 8);
    pos += cborencode_bool(buf + pos, 1);

    pos += cborencode_text_str(buf + pos, "manifest_count", 14);
    pos += cborencode_uint(buf + pos, manifest_counter_get());

    pos += cborencode_text_str(buf + pos, "timestamp", 9);
    pos += cborencode_uint(buf + pos, 1);

    ble_set_identity_raw(buf, pos);
}

// ------------------------------------------------------------
// Inbound (peripheral) callbacks
// ------------------------------------------------------------

void on_inbound_connect(uint16_t handle) {
    g_last_conn_handle = handle;
    mesh_state_on_connected(now_ms());
}

void on_inbound_disconnect(uint16_t /*handle*/) {
    g_last_conn_handle = 0xFFFF;
    mesh_state_on_disconnected(now_ms());
}

void on_file_command(const uint8_t *data, size_t len) {
    if (len == 0 || len > sizeof(CmdEntry::data)) return;
    CmdEntry entry;
    entry.len = len;
    std::memcpy(entry.data, data, len);
    if (xQueueSend(g_cmd_queue, &entry, 0) != pdTRUE) {
        g_dropped_cmds++;
    }
}

void process_pending_commands() {
    CmdEntry entry;
    while (xQueueReceive(g_cmd_queue, &entry, 0) == pdTRUE) {
        int resp_len = commands_handle(entry.data, entry.len,
                                       g_resp_buf, sizeof(g_resp_buf));
        g_last_write_len = entry.len;
        g_last_resp_len  = resp_len > 0 ? static_cast<uint32_t>(resp_len) : 0;
        if (resp_len > 0 && ble_is_connected()) {
            ble_send_file_response(g_resp_buf, static_cast<size_t>(resp_len));
        }
        ble_set_manifest_version(manifest_counter_get());
    }
}

// ------------------------------------------------------------
// Outbound (central) callbacks — peer sync session lifecycle.
// Mirrors firmware/pico-w/src/hw/pico-w/main.c.
// ------------------------------------------------------------

void on_peer_seen(const ble_client_peer_t *peer) {
    if (g_sync_pending) return;  // already attempting a peer
    if (peer_table_decide(peer->tpk_prefix, peer->manifest_version,
                          now_ms()) == PEER_DECISION_SKIP) {
        return;
    }
    std::memcpy(g_sync_peer_tpk, peer->tpk_prefix, 8);
    g_sync_peer_version = peer->manifest_version;
    g_sync_pending      = true;
    ble_client_connect(peer->bd_addr, peer->bd_addr_type);
}

void on_client_connected() {
    mesh_state_on_connected(now_ms());

    size_t           out_len = 0;
    peer_sync_step_t step    = PEER_SYNC_ERROR;
    g_sync = peer_sync_start(g_sync_peer_tpk, g_sync_buf, sizeof(g_sync_buf),
                             &out_len, &step);
    if (!g_sync || step != PEER_SYNC_NEED_WRITE) {
        std::printf("[mesh] peer_sync_start refused (step=%d); disconnecting\r\n",
                    static_cast<int>(step));
        ble_client_disconnect();
        return;
    }
    if (!ble_client_send_command(g_sync_buf, out_len)) {
        std::printf("[mesh] first send failed; disconnecting\r\n");
        ble_client_disconnect();
    }
}

void on_client_response(const uint8_t *data, size_t len) {
    if (!g_sync) {
        std::printf("[mesh] WARN: response (%u bytes) with no active session\r\n",
                    static_cast<unsigned>(len));
        return;
    }
    size_t           out_len = 0;
    peer_sync_step_t step    = peer_sync_handle_response(g_sync, data, len,
                                                          g_sync_buf,
                                                          sizeof(g_sync_buf),
                                                          &out_len);
    if (step == PEER_SYNC_NEED_WRITE) {
        if (!ble_client_send_command(g_sync_buf, out_len)) {
            std::printf("[mesh] send_command failed; disconnecting\r\n");
            ble_client_disconnect();
        }
        return;
    }
    std::printf("[mesh] sync %s\r\n", step == PEER_SYNC_DONE ? "done" : "error");
    peer_table_record_sync(g_sync_peer_tpk, g_sync_peer_version,
                           (step == PEER_SYNC_DONE)
                               ? PEER_SYNC_RESULT_SUCCESS
                               : PEER_SYNC_RESULT_FAILED,
                           now_ms());
    g_sync_pending = false;
    if (step == PEER_SYNC_DONE) g_syncs_completed++;
    ble_set_manifest_version(manifest_counter_get());
    ble_client_disconnect();
}

void on_client_disconnected() {
    if (g_sync_pending) {
        // Either the connect attempt itself failed, or the link
        // dropped before peer_sync ran to completion. Either way:
        // backoff so we don't immediately retry the same peer.
        std::printf("[mesh] sync attempt aborted; recording FAILED\r\n");
        peer_table_record_sync(g_sync_peer_tpk, g_sync_peer_version,
                               PEER_SYNC_RESULT_FAILED, now_ms());
        g_sync_pending = false;
    }
    if (g_sync) {
        peer_sync_end(g_sync);
        g_sync = nullptr;
    }
    mesh_state_on_disconnected(now_ms());
}

// ------------------------------------------------------------
// Mesh-mode action driver: react to mesh_state_tick decisions.
// ------------------------------------------------------------

void apply_mesh_action(mesh_action_t a) {
    switch (a) {
        case MESH_ACTION_NONE:
            break;
        case MESH_ACTION_START_ADVERTISING:
            ble_client_stop_scan();
            ble_start_advertising();
            break;
        case MESH_ACTION_START_SCANNING:
            ble_stop_advertising();
            ble_client_start_scan();
            break;
        case MESH_ACTION_STOP_BOTH:
            ble_client_stop_scan();
            break;
    }
}

// ------------------------------------------------------------
// Display
// ------------------------------------------------------------

const char *phase_label(mesh_phase_t p) {
    switch (p) {
        case MESH_GRACE:       return "grace";
        case MESH_ADVERTISING: return "adv";
        case MESH_SCANNING:    return "scan";
        case MESH_CONNECTED:   return "conn";
    }
    return "?";
}

void render_status() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextColor(WHITE, BLACK);
    d.setTextDatum(top_left);
    d.setTextSize(2);
    d.drawString(g_identity_ready ? g_identity.node_name : "Waxwing", 6, 6);

    char line[32];

    if (!g_sd_ready) {
        d.setTextColor(RED, BLACK);
        d.drawString("SD: fail", 6, 40);
        d.setTextColor(WHITE, BLACK);
    } else {
        uint32_t free_b = 0, used_b = 0, reserve = 0, count = 0;
        fs_storage_info(&free_b, &used_b, &reserve, &count);
        std::snprintf(line, sizeof(line), "SD %u files",
                      static_cast<unsigned>(count));
        d.drawString(line, 6, 40);
    }

    std::snprintf(line, sizeof(line), "%s mv=%u s=%u",
                  phase_label(mesh_state_phase()),
                  static_cast<unsigned>(manifest_counter_get()),
                  static_cast<unsigned>(g_syncs_completed));
    d.drawString(line, 6, 64);

    if (g_last_write_len > 0 || g_last_resp_len > 0) {
        std::snprintf(line, sizeof(line), "rx=%u tx=%u",
                      static_cast<unsigned>(g_last_write_len),
                      static_cast<unsigned>(g_last_resp_len));
        d.drawString(line, 6, 88);
    }
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);
    M5Cardputer.Display.setRotation(1);

    g_cmd_queue = xQueueCreate(kCmdQueueDepth, sizeof(CmdEntry));

    g_sd_ready = (fs_init() == 0);
    if (g_sd_ready) {
        manifest_counter_init();
        g_identity_ready = waxwing_identity_load_or_generate(&g_identity);
    }
    peer_table_init();

    if (!ble_init()) {
        M5Cardputer.Display.fillScreen(RED);
        M5Cardputer.Display.setTextColor(WHITE, RED);
        M5Cardputer.Display.drawString("ble_init failed", 6, 6);
        return;
    }

    ble_set_on_connect(on_inbound_connect);
    ble_set_on_disconnect(on_inbound_disconnect);
    ble_set_on_write(on_file_command);

    if (g_identity_ready) {
        ble_set_node_name(g_identity.node_name);
        publish_identity();
    }
    ble_set_manifest_version(manifest_counter_get());

    ble_client_init();
    ble_client_set_on_peer_seen(on_peer_seen);
    ble_client_set_on_connected(on_client_connected);
    ble_client_set_on_response(on_client_response);
    ble_client_set_on_disconnected(on_client_disconnected);

    mesh_state_init(now_ms(), mesh_rng);

    cardputer_ssid_scan_init();
    encounters_init();
    attestations_init();
    attest_cache_init();
    std::printf("[main] encounters=%d attestations=%d cached=%d\r\n",
                encounters_count(), attestations_count(), attest_cache_count());

    ble_start_advertising();
    render_status();
}

void loop() {
    M5Cardputer.update();
    ble_process();
    ble_client_process();
    process_pending_commands();
    apply_mesh_action(mesh_state_tick(now_ms()));

    cardputer_ssid_scan_tick(now_ms(), mesh_state_phase() == MESH_CONNECTED);
    if (g_identity_ready) {
        ssid_scan_t scan;
        static uint32_t s_last_scanned_ms = 0;
        if (ssid_scan_hal_latest(&scan) && scan.scanned_ms != s_last_scanned_ms) {
            s_last_scanned_ms = scan.scanned_ms;
            if (encounters_should_record(&scan, scan.scanned_ms, 600000)) {
                if (encounters_record(&scan, scan.scanned_ms,
                                      g_identity.pub, g_identity.seed) == 0) {
                    std::printf("[main] encounter recorded (%d total)\r\n",
                                encounters_count());
                }
            }
        }
    }

    const bool      connected = ble_is_connected();
    const uint16_t  mtu       = connected ? ble_get_mtu() : 0;
    const mesh_phase_t phase  = mesh_state_phase();
    static uint16_t      s_last_mtu       = 0;
    static size_t        s_last_rx        = 0;
    static uint32_t      s_last_tx        = 0;
    static mesh_phase_t  s_last_phase     = MESH_GRACE;
    static uint32_t      s_last_synced    = 0;
    if (connected != g_was_connected ||
        mtu != s_last_mtu ||
        g_last_write_len != s_last_rx ||
        g_last_resp_len  != s_last_tx ||
        phase != s_last_phase ||
        g_syncs_completed != s_last_synced) {
        g_was_connected  = connected;
        s_last_mtu       = mtu;
        s_last_rx        = g_last_write_len;
        s_last_tx        = g_last_resp_len;
        s_last_phase     = phase;
        s_last_synced    = g_syncs_completed;
        render_status();
    }

    delay(20);
}
