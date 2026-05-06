#include <M5Cardputer.h>

#include "hw/ble.h"
#include "hw/ble_client.h"
#include "hw/ssid_scan_esp32.h"

extern "C" {
#include "core/attest_cache.h"
#include "core/attestations.h"
#include "core/cborencode.h"
#include "core/commands.h"
#include "core/encounter_record.h"
#include "core/encounter_session.h"
#include "core/encounters.h"
#include "core/filestore.h"
#include "core/hal_crypto.h"
#include "core/identity.h"
#include "core/manifest_counter.h"
#include "core/meeting_count.h"
#include "core/mesh_state.h"
#include "core/peer_ledger.h"
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
QueueHandle_t g_cmd_queue      = nullptr;
QueueHandle_t g_peer_cmd_queue = nullptr;

// M4 stage 6: responder-side encounter session state. Armed on every
// inbound connect; the first PROPOSE drives it through to DONE.
encounter_session_t g_resp_session = {};
bool                g_resp_armed   = false;
bool                g_resp_done    = false;

// Per-session byte counters for the responder side. Tally every byte
// that flows on the peer characteristic during a connection (handshake
// and post-DONE peer commands). At disconnect we apply the deltas to
// peer_ledger keyed by the peer pub captured at encounter DONE so the
// lifetime fields in the next session populate with real numbers.
uint64_t g_inbound_peer_tx       = 0;
uint64_t g_inbound_peer_rx       = 0;
uint8_t  g_inbound_peer_pub[ENCOUNTER_PUB_BYTES] = {};
bool     g_inbound_peer_pub_known = false;

// M4 stage 6: outbound-side encounter session and phase machine.
// Pre-handshake peers (no peer_cmd characteristic) skip these and run
// the legacy peer_sync path directly.
enum class OutPhase : uint8_t {
    Idle,
    Handshake,           // PROPOSE sent, awaiting ACCEPT
    HandshakeConfirmed,  // CONFIRM sent, awaiting OK ACK
    Syncing,             // peer_sync running
};
OutPhase            g_out_phase    = OutPhase::Idle;
encounter_session_t g_init_session = {};

// Per-session byte counters for the outbound side. Same shape as the
// inbound counters; applied to peer_ledger at on_client_disconnected.
uint64_t g_outbound_tx                       = 0;
uint64_t g_outbound_rx                       = 0;
uint8_t  g_outbound_peer_pub[ENCOUNTER_PUB_BYTES] = {};
bool     g_outbound_peer_pub_known           = false;

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

// Forward declarations (definitions live further down).
void arm_responder_session();
void reset_responder_session();

// Wrap ble_client_send_command so outbound writes are tallied toward
// the per-session counter that gets applied to peer_ledger at
// disconnect.
inline bool send_outbound(const uint8_t *data, size_t len) {
    bool ok = ble_client_send_command(data, len);
    if (ok) g_outbound_tx += static_cast<uint64_t>(len);
    return ok;
}

// ------------------------------------------------------------
// Inbound (peripheral) callbacks
// ------------------------------------------------------------

void on_inbound_connect(uint16_t handle) {
    g_last_conn_handle = handle;
    arm_responder_session();
    mesh_state_on_connected(now_ms());
}

void on_inbound_disconnect(uint16_t /*handle*/) {
    g_last_conn_handle = 0xFFFF;
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
// Encounter handshake helpers (M4 stage 6).
// ------------------------------------------------------------

void build_local_input(encounter_local_input_t *me, const uint8_t *expected) {
    std::memset(me, 0, sizeof(*me));
    std::memcpy(me->pub,  g_identity.pub,  ENCOUNTER_PUB_BYTES);
    std::memcpy(me->seed, g_identity.seed, ENCOUNTER_PUB_BYTES);
    hal_random_bytes(me->nonce, ENCOUNTER_NONCE_BYTES);
    if (expected) {
        std::memcpy(me->expected_peer_prefix, expected, 8);
    }
    me->meeting_count = meeting_count_get();

    ssid_scan_t scan;
    if (ssid_scan_hal_latest(&scan)) {
        uint8_t n = scan.count;
        if (n > ENCOUNTER_BSSID_MAX) n = ENCOUNTER_BSSID_MAX;
        me->bssids_count = n;
        for (uint8_t i = 0; i < n; i++) {
            std::memcpy(me->bssids[i], scan.obs[i].bssid, 6);
        }
    }

    // Initiator path knows the 8-byte peer prefix; populate the
    // lifetime fields and rep score from peer_ledger if we've met this
    // peer before. Responder side leaves them zero until the late-bind
    // session-API refinement.
    if (expected) {
        peer_ledger_entry_t e;
        if (peer_ledger_get_by_prefix(expected, &e)) {
            me->tx_bytes_to_peer_lifetime     = e.tx_bytes_lifetime;
            me->rx_bytes_from_peer_lifetime   = e.rx_bytes_lifetime;
            me->file_count_from_peer_lifetime = e.file_count_lifetime;
            me->rep_of_peer                    = e.my_rep_score;
        }
    }
}

void on_encounter_done(const encounter_record_t *rec, bool we_are_a) {
    uint8_t id[ENCOUNTER_ID_BYTES];
    encounter_record_id(rec, id);

    char name[FS_MAX_NAME_LEN];
    std::snprintf(name, sizeof(name),
                  "enc_%02x%02x%02x%02x%02x%02x%02x%02x.cbor",
                  id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);

    uint8_t buf[ENCOUNTER_RECORD_MAX_BYTES];
    size_t  blob_len = encounter_record_encode_full(rec, buf, sizeof(buf));
    if (blob_len > 0) {
        if (fs_write(name, buf, blob_len) == 0) {
            manifest_counter_bump();
            ble_set_manifest_version(manifest_counter_get());
        } else {
            std::printf("[enc] fs_write failed for %s\r\n", name);
        }
    }

    meeting_count_bump();

    const uint8_t *peer_pub = we_are_a ? rec->pub_b : rec->pub_a;
    uint64_t peer_count    = we_are_a ? rec->meeting_count_b
                                       : rec->meeting_count_a;
    peer_ledger_apply(peer_pub, /*tx*/0, /*rx*/0, /*files*/0, peer_count);

    std::printf("[enc] DONE id=%02x%02x%02x%02x peer=%02x%02x%02x%02x\r\n",
                id[0], id[1], id[2], id[3],
                peer_pub[0], peer_pub[1], peer_pub[2], peer_pub[3]);
}

int build_ok_ack(uint8_t *out, size_t out_max) {
    if (out_max < 8) return -1;
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "ok", 2);
    p += cborencode_bool(p, 1);
    return static_cast<int>(p - out);
}

int build_error_ack(uint8_t *out, size_t out_max, const char *msg) {
    if (out_max < 32) return -1;
    uint8_t *p = out;
    p += cborencode_map_header(p, 1);
    p += cborencode_text_str(p, "error", 5);
    size_t mlen = std::strlen(msg);
    if (mlen > 24) mlen = 24;
    p += cborencode_text_str(p, msg, mlen);
    return static_cast<int>(p - out);
}

void reset_responder_session() {
    std::memset(&g_resp_session, 0, sizeof(g_resp_session));
    g_resp_armed = false;
    g_resp_done  = false;
    g_inbound_peer_tx        = 0;
    g_inbound_peer_rx        = 0;
    g_inbound_peer_pub_known = false;
}

// Wrap ble_send_file_response so peer-mode notifications are tallied.
// Companion-mode responses must keep using ble_send_file_response
// directly so they don't pollute the peer ledger.
void send_peer_response(const uint8_t *data, size_t len) {
    ble_send_file_response(data, len);
    g_inbound_peer_tx += static_cast<uint64_t>(len);
}

void arm_responder_session() {
    if (!g_identity_ready) return;
    encounter_local_input_t me;
    build_local_input(&me, /*expected=*/nullptr);

    uint8_t  scratch[ENCOUNTER_MSG_MAX_BYTES];
    size_t   olen = 0;
    encounter_step_t step = encounter_session_start(
        &g_resp_session, ENCOUNTER_ROLE_RESPONDER, &me,
        scratch, sizeof(scratch), &olen);
    if (step == ENCOUNTER_STEP_NEED_READ) {
        g_resp_armed = true;
    } else {
        std::printf("[enc] responder start refused (step=%d)\r\n",
                    static_cast<int>(step));
    }
}

// Inbound peer-command writes (CHAR_PEER_COMMAND). NimBLE callback —
// queue the bytes for the main loop to dispatch.
void on_peer_command(const uint8_t *data, size_t len) {
    if (len == 0 || len > sizeof(CmdEntry::data)) return;
    CmdEntry entry;
    entry.len = len;
    std::memcpy(entry.data, data, len);
    if (xQueueSend(g_peer_cmd_queue, &entry, 0) != pdTRUE) {
        g_dropped_cmds++;
    }
}

void process_pending_peer_commands() {
    CmdEntry entry;
    while (xQueueReceive(g_peer_cmd_queue, &entry, 0) == pdTRUE) {
        g_inbound_peer_rx += static_cast<uint64_t>(entry.len);

        if (!g_resp_armed) {
            arm_responder_session();
            if (!g_resp_armed) {
                int n = build_error_ack(g_resp_buf, sizeof(g_resp_buf),
                                        "no session");
                if (n > 0) send_peer_response(g_resp_buf,
                                              static_cast<size_t>(n));
                continue;
            }
        }

        if (g_resp_done) {
            int rc = commands_handle_session(COMMANDS_SESSION_PEER,
                                              entry.data, entry.len,
                                              g_resp_buf, sizeof(g_resp_buf));
            if (rc > 0 && ble_is_connected()) {
                send_peer_response(g_resp_buf, static_cast<size_t>(rc));
            }
            ble_set_manifest_version(manifest_counter_get());
            continue;
        }

        size_t out_len = 0;
        encounter_step_t step = encounter_session_handle(
            &g_resp_session, entry.data, entry.len,
            g_resp_buf, sizeof(g_resp_buf), &out_len);

        if (step == ENCOUNTER_STEP_NEED_WRITE) {
            send_peer_response(g_resp_buf, out_len);
            continue;
        }
        if (step == ENCOUNTER_STEP_DONE) {
            encounter_record_t rec;
            if (encounter_session_take_record(&g_resp_session, &rec)) {
                on_encounter_done(&rec, /*we_are_a=*/false);
                std::memcpy(g_inbound_peer_pub, rec.pub_a,
                             ENCOUNTER_PUB_BYTES);
                g_inbound_peer_pub_known = true;
            }
            g_resp_done = true;
            int n = build_ok_ack(g_resp_buf, sizeof(g_resp_buf));
            if (n > 0) send_peer_response(g_resp_buf,
                                          static_cast<size_t>(n));
            continue;
        }
        // ERROR
        int n = build_error_ack(g_resp_buf, sizeof(g_resp_buf),
                                "handshake failed");
        if (n > 0) send_peer_response(g_resp_buf, static_cast<size_t>(n));
        reset_responder_session();
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
    g_sync_peer_version       = peer->manifest_version;
    g_sync_pending            = true;
    g_outbound_tx             = 0;
    g_outbound_rx             = 0;
    g_outbound_peer_pub_known = false;
    ble_client_connect(peer->bd_addr, peer->bd_addr_type);
}

void start_peer_sync() {
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
    g_out_phase = OutPhase::Syncing;
    if (!send_outbound(g_sync_buf, out_len)) {
        std::printf("[mesh] first sync send failed; disconnecting\r\n");
        ble_client_disconnect();
    }
}

void on_client_connected() {
    mesh_state_on_connected(now_ms());

    if (!ble_client_uses_peer_characteristic()) {
        std::printf("[mesh] peer has no peer_cmd; legacy peer_sync path\r\n");
        start_peer_sync();
        return;
    }

    if (!g_identity_ready) {
        std::printf("[mesh] no identity; skipping handshake\r\n");
        ble_client_disconnect();
        return;
    }

    encounter_local_input_t me;
    build_local_input(&me, g_sync_peer_tpk);

    uint8_t  scratch[ENCOUNTER_MSG_MAX_BYTES];
    size_t   olen = 0;
    encounter_step_t step = encounter_session_start(
        &g_init_session, ENCOUNTER_ROLE_INITIATOR, &me,
        scratch, sizeof(scratch), &olen);
    if (step != ENCOUNTER_STEP_NEED_WRITE) {
        std::printf("[enc] initiator start refused (step=%d)\r\n",
                    static_cast<int>(step));
        ble_client_disconnect();
        return;
    }
    g_out_phase = OutPhase::Handshake;
    if (!send_outbound(scratch, olen)) {
        std::printf("[enc] PROPOSE send failed; disconnecting\r\n");
        ble_client_disconnect();
    }
}

void on_client_response(const uint8_t *data, size_t len) {
    g_outbound_rx += static_cast<uint64_t>(len);
    switch (g_out_phase) {
        case OutPhase::Handshake: {
            uint8_t scratch[ENCOUNTER_MSG_MAX_BYTES];
            size_t  olen = 0;
            encounter_step_t step = encounter_session_handle(
                &g_init_session, data, len,
                scratch, sizeof(scratch), &olen);
            if (step != ENCOUNTER_STEP_NEED_WRITE) {
                std::printf("[enc] ACCEPT handle failed (step=%d)\r\n",
                            static_cast<int>(step));
                ble_client_disconnect();
                return;
            }
            encounter_record_t rec;
            if (encounter_session_take_record(&g_init_session, &rec)) {
                on_encounter_done(&rec, /*we_are_a=*/true);
                std::memcpy(g_outbound_peer_pub, rec.pub_b,
                             ENCOUNTER_PUB_BYTES);
                g_outbound_peer_pub_known = true;
            }
            g_out_phase = OutPhase::HandshakeConfirmed;
            if (!send_outbound(scratch, olen)) {
                std::printf("[enc] CONFIRM send failed; disconnecting\r\n");
                ble_client_disconnect();
            }
            return;
        }
        case OutPhase::HandshakeConfirmed:
            (void)data; (void)len;
            start_peer_sync();
            return;
        case OutPhase::Syncing: {
            if (!g_sync) {
                std::printf("[mesh] WARN: response (%u bytes) with no sync\r\n",
                            static_cast<unsigned>(len));
                return;
            }
            size_t           out_len = 0;
            peer_sync_step_t step    = peer_sync_handle_response(g_sync, data, len,
                                                                  g_sync_buf,
                                                                  sizeof(g_sync_buf),
                                                                  &out_len);
            if (step == PEER_SYNC_NEED_WRITE) {
                if (!send_outbound(g_sync_buf, out_len)) {
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
            return;
        }
        case OutPhase::Idle:
        default:
            std::printf("[mesh] WARN: unexpected response in Idle\r\n");
            return;
    }
}

void on_client_disconnected() {
    if (g_outbound_peer_pub_known &&
        (g_outbound_tx > 0 || g_outbound_rx > 0)) {
        peer_ledger_apply(g_outbound_peer_pub,
                          g_outbound_tx,
                          g_outbound_rx,
                          /*file_delta=*/0,
                          /*peer_meeting_count_seen=*/0);
    }
    if (g_sync_pending) {
        std::printf("[mesh] sync attempt aborted; recording FAILED\r\n");
        peer_table_record_sync(g_sync_peer_tpk, g_sync_peer_version,
                               PEER_SYNC_RESULT_FAILED, now_ms());
        g_sync_pending = false;
    }
    if (g_sync) {
        peer_sync_end(g_sync);
        g_sync = nullptr;
    }
    g_out_phase = OutPhase::Idle;
    std::memset(&g_init_session, 0, sizeof(g_init_session));
    g_outbound_peer_pub_known = false;
    g_outbound_tx = 0;
    g_outbound_rx = 0;
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

    g_cmd_queue      = xQueueCreate(kCmdQueueDepth, sizeof(CmdEntry));
    g_peer_cmd_queue = xQueueCreate(kCmdQueueDepth, sizeof(CmdEntry));

    g_sd_ready = (fs_init() == 0);
    if (g_sd_ready) {
        manifest_counter_init();
        meeting_count_init();
        peer_ledger_init();
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
    ble_set_on_peer_write(on_peer_command);

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
    process_pending_peer_commands();
    apply_mesh_action(mesh_state_tick(now_ms()));

    cardputer_ssid_scan_tick(now_ms(), mesh_state_phase() == MESH_CONNECTED);
    // M4 stage 9: solo SSID-self-attestation cadence retired. New
    // encounters are produced exclusively by the two-party handshake
    // on the peer characteristic (encounter_session.c). The v1 store
    // stays readable via cmd_encounters_get for iOS until stage 8.

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
