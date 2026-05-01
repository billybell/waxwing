#include "hw/pico-w/ssid_scan_pico.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "core/ssid_scan_hal.h"

#define SCAN_INTERVAL_MS  60000u
#define SCAN_FIRST_MS     5000u    // wait this long after init for the first scan

static ssid_scan_t s_in_progress;
static ssid_scan_t s_latest;
static bool        s_have_latest = false;
static bool        s_initialized = false;
static bool        s_active      = false;
static uint32_t    s_next_kickoff_ms = SCAN_FIRST_MS;

static int on_scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r) return 0;
    uint8_t ssid_len = r->ssid_len;
    if (ssid_len > SSID_SCAN_SSID_MAX) ssid_len = SSID_SCAN_SSID_MAX;
    int8_t rssi8 = (r->rssi < -128) ? -128 : (r->rssi > 127 ? 127 : (int8_t)r->rssi);
    uint8_t channel = (r->channel > 255) ? 0 : (uint8_t)r->channel;
    ssid_scan_add(&s_in_progress, r->bssid, r->ssid, ssid_len, rssi8, channel);
    return 0;
}

bool pico_ssid_scan_init(void) {
    if (s_initialized) return true;
    cyw43_arch_enable_sta_mode();
    ssid_scan_init(&s_in_progress);
    ssid_scan_init(&s_latest);
    s_initialized = true;
    return true;
}

static void kickoff(uint32_t now_ms) {
    cyw43_wifi_scan_options_t opts = {0};
    int rc = cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_scan_result);
    if (rc != 0) {
        printf("[ssid_scan] cyw43_wifi_scan rc=%d\r\n", rc);
        s_next_kickoff_ms = now_ms + SCAN_INTERVAL_MS;
        return;
    }
    ssid_scan_init(&s_in_progress);
    s_active = true;
}

void pico_ssid_scan_tick(uint32_t now_ms, bool radio_busy) {
    if (!s_initialized) return;

    if (s_active) {
        if (!cyw43_wifi_scan_active(&cyw43_state)) {
            ssid_scan_sort_by_rssi(&s_in_progress);
            s_in_progress.scanned_ms = now_ms;
            s_latest = s_in_progress;
            s_have_latest = true;
            s_active = false;
            s_next_kickoff_ms = now_ms + SCAN_INTERVAL_MS;
            printf("[ssid_scan] scan done, %u observations\r\n",
                   (unsigned)s_latest.count);
        }
        return;
    }

    if (radio_busy) return;
    if ((int32_t)(now_ms - s_next_kickoff_ms) < 0) return;
    kickoff(now_ms);
}

const ssid_scan_t *pico_ssid_scan_latest(void) {
    return s_have_latest ? &s_latest : NULL;
}

bool ssid_scan_hal_latest(ssid_scan_t *out) {
    if (!s_have_latest || !out) return false;
    *out = s_latest;
    return true;
}
