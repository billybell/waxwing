#include <M5Cardputer.h>

#include "hw/ble.h"

#include <cstdio>
#include <cstring>

namespace {

bool     g_was_connected = false;
uint16_t g_last_conn_handle = 0xFFFF;
size_t   g_last_write_len = 0;

void seed_stub_identity() {
    // Phase-1 placeholder. Replace with NVS-backed Ed25519 identity
    // once hal_crypto / NVS-storage steps land. For bring-up we just
    // need *some* readable bytes so a peer can confirm the
    // characteristic is wired through.
    uint8_t blob[32];
    for (size_t i = 0; i < sizeof(blob); ++i) {
        blob[i] = static_cast<uint8_t>(0xC0 + i);
    }
    ble_set_identity_raw(blob, sizeof(blob));
}

void on_connect(uint16_t handle) {
    g_last_conn_handle = handle;
}

void on_disconnect(uint16_t /*handle*/) {
    g_last_conn_handle = 0xFFFF;
    ble_start_advertising();
}

void on_write(const uint8_t* /*data*/, size_t len) {
    g_last_write_len = len;
}

void render_status() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextColor(WHITE, BLACK);
    d.setTextDatum(top_left);
    d.setTextSize(2);
    d.drawString("Waxwing", 6, 6);

    const bool connected = ble_is_connected();
    if (connected) {
        char line[32];
        std::snprintf(line, sizeof(line), "conn h=%u m=%u",
                      static_cast<unsigned>(g_last_conn_handle),
                      static_cast<unsigned>(ble_get_mtu()));
        d.drawString(line, 6, 40);
    } else {
        d.drawString("advertising", 6, 40);
    }

    if (g_last_write_len > 0) {
        char line[32];
        std::snprintf(line, sizeof(line), "rx %u bytes",
                      static_cast<unsigned>(g_last_write_len));
        d.drawString(line, 6, 74);
    }
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);
    M5Cardputer.Display.setRotation(1);

    if (!ble_init()) {
        M5Cardputer.Display.fillScreen(RED);
        M5Cardputer.Display.setTextColor(WHITE, RED);
        M5Cardputer.Display.drawString("ble_init failed", 6, 6);
        return;
    }

    ble_set_on_connect(on_connect);
    ble_set_on_disconnect(on_disconnect);
    ble_set_on_write(on_write);

    seed_stub_identity();
    ble_start_advertising();
    render_status();
}

void loop() {
    M5Cardputer.update();
    ble_process();

    const bool     connected = ble_is_connected();
    const uint16_t mtu       = connected ? ble_get_mtu() : 0;
    static uint16_t g_last_rendered_mtu = 0;
    static size_t   g_last_rendered_write_len = 0;
    if (connected != g_was_connected ||
        mtu != g_last_rendered_mtu ||
        g_last_write_len != g_last_rendered_write_len) {
        g_was_connected = connected;
        g_last_rendered_mtu = mtu;
        g_last_rendered_write_len = g_last_write_len;
        render_status();
    }

    delay(50);
}
