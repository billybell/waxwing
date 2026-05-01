#include "hw/ssid_scan_esp32.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

extern "C" {
#include "core/ssid_scan.h"
#include "core/ssid_scan_hal.h"
}

namespace {

constexpr uint32_t SCAN_INTERVAL_MS = 60000;
constexpr uint32_t SCAN_FIRST_MS    = 5000;

ssid_scan_t s_in_progress;
ssid_scan_t s_latest;
bool        s_have_latest     = false;
bool        s_initialized     = false;
bool        s_active          = false;
uint32_t    s_next_kickoff_ms = SCAN_FIRST_MS;

void collect_results(uint32_t now_ms) {
    int rc = WiFi.scanComplete();
    ssid_scan_init(&s_in_progress);
    if (rc > 0) {
        for (int i = 0; i < rc; i++) {
            String   ssid     = WiFi.SSID(i);
            uint8_t *bssid    = WiFi.BSSID(i);
            int32_t  rssi_raw = WiFi.RSSI(i);
            int32_t  ch_raw   = WiFi.channel(i);
            if (!bssid) continue;
            int8_t  rssi    = (int8_t)constrain(rssi_raw, -128, 127);
            uint8_t channel = (ch_raw < 0 || ch_raw > 255) ? 0 : (uint8_t)ch_raw;
            ssid_scan_add(&s_in_progress,
                          bssid,
                          reinterpret_cast<const uint8_t *>(ssid.c_str()),
                          (uint8_t)ssid.length(),
                          rssi, channel);
        }
    }
    WiFi.scanDelete();
    ssid_scan_sort_by_rssi(&s_in_progress);
    s_in_progress.scanned_ms = now_ms;
    s_latest = s_in_progress;
    s_have_latest = true;
    s_active = false;
    s_next_kickoff_ms = now_ms + SCAN_INTERVAL_MS;
    std::printf("[ssid_scan] scan done, %u observations\r\n",
                (unsigned)s_latest.count);
}

}  // namespace

void cardputer_ssid_scan_init(void) {
    if (s_initialized) return;
    // STA mode is required for scanning. Don't connect to anything; we want
    // a passive scan so BT/companion traffic isn't interrupted by association.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true /* erase config */, true /* eraseAP */);
    ssid_scan_init(&s_in_progress);
    ssid_scan_init(&s_latest);
    s_initialized = true;
}

void cardputer_ssid_scan_tick(uint32_t now_ms, bool radio_busy) {
    if (!s_initialized) return;

    if (s_active) {
        int rc = WiFi.scanComplete();
        if (rc == WIFI_SCAN_RUNNING) return;
        collect_results(now_ms);
        return;
    }

    if (radio_busy) return;
    if ((int32_t)(now_ms - s_next_kickoff_ms) < 0) return;

    int rc = WiFi.scanNetworks(true /* async */, true /* show_hidden */);
    if (rc == WIFI_SCAN_RUNNING) {
        s_active = true;
    } else if (rc >= 0) {
        // Sync result (driver returned immediately) — collect now.
        s_active = true;
        collect_results(now_ms);
    } else {
        std::printf("[ssid_scan] scanNetworks rc=%d\r\n", rc);
        s_next_kickoff_ms = now_ms + SCAN_INTERVAL_MS;
    }
}

extern "C" bool ssid_scan_hal_latest(ssid_scan_t *out) {
    if (!s_have_latest || !out) return false;
    *out = s_latest;
    return true;
}
