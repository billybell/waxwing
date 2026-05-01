#pragma once
#include <stdint.h>
#include <stdbool.h>

void cardputer_ssid_scan_init(void);
void cardputer_ssid_scan_tick(uint32_t now_ms, bool radio_busy);
