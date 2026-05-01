#ifndef WAXWING_PICO_W_SSID_SCAN_H
#define WAXWING_PICO_W_SSID_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include "core/ssid_scan.h"

bool pico_ssid_scan_init(void);

// Drive the scan state machine. Call from the main run loop. `radio_busy`
// suppresses new scan kickoffs (a sync is mid-flight on the shared BLE/WiFi
// radio); already-running scans complete normally.
void pico_ssid_scan_tick(uint32_t now_ms, bool radio_busy);

// Most-recent completed scan. Returns NULL if no scan has finished yet.
const ssid_scan_t *pico_ssid_scan_latest(void);

#endif
