#include "core/ssid_scan.h"

#include <string.h>

void ssid_scan_init(ssid_scan_t *scan) {
    memset(scan, 0, sizeof(*scan));
}

bool ssid_scan_bssid_is_random(const uint8_t bssid[6]) {
    return (bssid[0] & 0x02) != 0;
}

static int find_bssid(const ssid_scan_t *scan, const uint8_t bssid[6]) {
    for (uint8_t i = 0; i < scan->count; i++) {
        if (memcmp(scan->obs[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static int weakest_slot(const ssid_scan_t *scan) {
    int idx = 0;
    int8_t worst = scan->obs[0].rssi;
    for (uint8_t i = 1; i < scan->count; i++) {
        if (scan->obs[i].rssi < worst) {
            worst = scan->obs[i].rssi;
            idx = i;
        }
    }
    return idx;
}

bool ssid_scan_add(ssid_scan_t *scan,
                   const uint8_t bssid[6],
                   const uint8_t *ssid, uint8_t ssid_len,
                   int8_t rssi, uint8_t channel) {
    if (ssid_scan_bssid_is_random(bssid)) return false;

    int idx = find_bssid(scan, bssid);
    if (idx >= 0) {
        scan->obs[idx].rssi    = rssi;
        scan->obs[idx].channel = channel;
        return true;
    }

    if (scan->count < SSID_SCAN_MAX) {
        idx = scan->count++;
    } else {
        int weak = weakest_slot(scan);
        if (rssi <= scan->obs[weak].rssi) return false;
        idx = weak;
    }

    ssid_observation_t *o = &scan->obs[idx];
    memcpy(o->bssid, bssid, 6);
    if (ssid_len > SSID_SCAN_SSID_MAX) ssid_len = SSID_SCAN_SSID_MAX;
    if (ssid_len > 0) memcpy(o->ssid, ssid, ssid_len);
    o->ssid[ssid_len] = '\0';
    o->ssid_len = ssid_len;
    o->rssi     = rssi;
    o->channel  = channel;
    return true;
}

void ssid_scan_sort_by_rssi(ssid_scan_t *scan) {
    // Insertion sort, descending by RSSI. count <= SSID_SCAN_MAX (8) so the
    // O(n²) cost is irrelevant compared to the wifi scan itself.
    for (uint8_t i = 1; i < scan->count; i++) {
        ssid_observation_t key = scan->obs[i];
        int j = i;
        while (j > 0 && scan->obs[j - 1].rssi < key.rssi) {
            scan->obs[j] = scan->obs[j - 1];
            j--;
        }
        scan->obs[j] = key;
    }
}

bool ssid_scan_bssids_differ(const ssid_scan_t *a, const ssid_scan_t *b) {
    if (a->count != b->count) return true;
    for (uint8_t i = 0; i < a->count; i++) {
        if (find_bssid(b, a->obs[i].bssid) < 0) return true;
    }
    return false;
}
