#include <M5Cardputer.h>

#include "hw/ble.h"

extern "C" {
#include "core/cborencode.h"
#include "core/commands.h"
#include "core/filestore.h"
#include "core/manifest_counter.h"
}

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

// Inbound File Command writes are queued and processed in loop()
// rather than synchronously inside the NimBLE host-task callback.
// Doing CBOR + filestore + SD work on the host task's 4 KB stack
// blew the canary on the very first write; the Arduino main task
// has 8 KB and isn't latency-critical for SD I/O.
struct CmdEntry {
    size_t  len;
    uint8_t data[512];
};
constexpr size_t kCmdQueueDepth = 4;
QueueHandle_t g_cmd_queue = nullptr;

void seed_stub_identity() {
    // CBOR identity map matching the Pico W wire format (see
    // firmware/pico-w/src/hw/pico-w/ble.c::build_identity_from_struct
    // and firmware/pico-w/CLAUDE.md). The TPK bytes are still a
    // step-3 stub — replaced by a real Ed25519 public key once the
    // NVS-backed identity store lands. iOS validates the map shape,
    // so encoding has to be right even with a placeholder key.
    uint8_t tpk[32];
    for (size_t i = 0; i < sizeof(tpk); ++i) {
        tpk[i] = static_cast<uint8_t>(0xC0 + i);
    }

    uint8_t buf[BLE_MAX_DATA_SIZE];
    size_t  pos = 0;

    pos += cborencode_map_header(buf + pos, 10);

    pos += cborencode_text_str(buf + pos, "protocol", 8);
    pos += cborencode_text_str(buf + pos, "waxwing-mesh", 12);

    pos += cborencode_text_str(buf + pos, "v", 1);
    pos += cborencode_uint(buf + pos, 0);

    pos += cborencode_text_str(buf + pos, "name", 4);
    pos += cborencode_text_str(buf + pos, "cardputer", 9);

    pos += cborencode_text_str(buf + pos, "tpk", 3);
    pos += cborencode_byte_str(buf + pos, tpk, sizeof(tpk));

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

void on_connect(uint16_t handle) {
    g_last_conn_handle = handle;
}

void on_disconnect(uint16_t /*handle*/) {
    g_last_conn_handle = 0xFFFF;
    ble_start_advertising();
}

void on_file_command(const uint8_t *data, size_t len) {
    // Runs on NimBLE host task. Just queue and return — actual
    // command dispatch happens in loop().
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

void render_status() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextColor(WHITE, BLACK);
    d.setTextDatum(top_left);
    d.setTextSize(2);
    d.drawString("Waxwing", 6, 6);

    char line[32];

    if (!g_sd_ready) {
        d.setTextColor(RED, BLACK);
        d.drawString("SD: fail", 6, 40);
        d.setTextColor(WHITE, BLACK);
    } else {
        uint32_t free_b = 0, used_b = 0, reserve = 0, count = 0;
        fs_storage_info(&free_b, &used_b, &reserve, &count);
        std::snprintf(line, sizeof(line), "SD %u files", static_cast<unsigned>(count));
        d.drawString(line, 6, 40);
    }

    if (ble_is_connected()) {
        std::snprintf(line, sizeof(line), "conn h=%u m=%u",
                      static_cast<unsigned>(g_last_conn_handle),
                      static_cast<unsigned>(ble_get_mtu()));
        d.drawString(line, 6, 64);
    } else {
        d.drawString("advertising", 6, 64);
    }

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
    }

    if (!ble_init()) {
        M5Cardputer.Display.fillScreen(RED);
        M5Cardputer.Display.setTextColor(WHITE, RED);
        M5Cardputer.Display.drawString("ble_init failed", 6, 6);
        return;
    }

    ble_set_on_connect(on_connect);
    ble_set_on_disconnect(on_disconnect);
    ble_set_on_write(on_file_command);

    seed_stub_identity();
    ble_set_manifest_version(manifest_counter_get());
    ble_start_advertising();
    render_status();
}

void loop() {
    M5Cardputer.update();
    ble_process();
    process_pending_commands();

    const bool      connected = ble_is_connected();
    const uint16_t  mtu       = connected ? ble_get_mtu() : 0;
    static uint16_t s_last_mtu  = 0;
    static size_t   s_last_rx   = 0;
    static uint32_t s_last_tx   = 0;
    if (connected != g_was_connected ||
        mtu != s_last_mtu ||
        g_last_write_len != s_last_rx ||
        g_last_resp_len  != s_last_tx) {
        g_was_connected = connected;
        s_last_mtu = mtu;
        s_last_rx  = g_last_write_len;
        s_last_tx  = g_last_resp_len;
        render_status();
    }

    delay(50);
}
