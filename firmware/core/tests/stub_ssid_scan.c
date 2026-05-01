#include "core/ssid_scan_hal.h"
#include "stub_ssid_scan.h"

#include <string.h>

static ssid_scan_t s_fixture;
static bool        s_have = false;

void mock_ssid_scan_set(const ssid_scan_t *scan) {
    s_fixture = *scan;
    s_have = true;
}

void mock_ssid_scan_clear(void) {
    s_have = false;
    memset(&s_fixture, 0, sizeof(s_fixture));
}

bool ssid_scan_hal_latest(ssid_scan_t *out) {
    if (!s_have) return false;
    *out = s_fixture;
    return true;
}
