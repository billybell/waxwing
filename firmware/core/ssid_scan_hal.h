#ifndef WAXWING_SSID_SCAN_HAL_H
#define WAXWING_SSID_SCAN_HAL_H

#include <stdbool.h>
#include "core/ssid_scan.h"

// Platform glue: copy the most recent completed scan into `out`.
// Returns false if no scan has finished yet.
bool ssid_scan_hal_latest(ssid_scan_t *out);

#endif
