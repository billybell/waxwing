#ifndef WAXWING_SSID_SCAN_H
#define WAXWING_SSID_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define SSID_SCAN_MAX        8
#define SSID_SCAN_SSID_MAX   32

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[SSID_SCAN_SSID_MAX + 1];   // NUL-terminated, may be empty for hidden
    uint8_t ssid_len;
    int8_t  rssi;
    uint8_t channel;
} ssid_observation_t;

typedef struct {
    uint32_t           scanned_ms;
    uint8_t            count;
    ssid_observation_t obs[SSID_SCAN_MAX];
} ssid_scan_t;

void ssid_scan_init(ssid_scan_t *scan);

// True for BSSIDs that are useless as a place fingerprint: locally administered
// MAC addresses are randomized by iOS personal hotspots, Android tethering,
// and most enterprise random-MAC schemes. The locally-administered bit is the
// 2nd-least-significant bit of the first byte (IEEE 802 §4.2.2).
bool ssid_scan_bssid_is_random(const uint8_t bssid[6]);

// Add one observation, dropping it silently if the BSSID looks randomized.
// If the table is full, the new entry replaces the weakest-RSSI entry (only
// if the new one is stronger). Duplicate BSSIDs update in place. Returns
// true if the observation was stored (or merged into an existing slot).
bool ssid_scan_add(ssid_scan_t *scan,
                   const uint8_t bssid[6],
                   const uint8_t *ssid, uint8_t ssid_len,
                   int8_t rssi, uint8_t channel);

// Sort observations in-place by RSSI, strongest first. Used after a scan
// completes so the most useful observations occupy the low indices.
void ssid_scan_sort_by_rssi(ssid_scan_t *scan);

// True iff `a` and `b` differ by ≥1 BSSID (set inequality, ignoring order).
// Used by the encounter-record dedup rule.
bool ssid_scan_bssids_differ(const ssid_scan_t *a, const ssid_scan_t *b);

#endif
