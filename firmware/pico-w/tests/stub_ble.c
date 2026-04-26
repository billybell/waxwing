#include <stdint.h>
#include <stddef.h>

// Stub for ble_get_mtu() — returns the BTstack default MTU.
// commands.c calls this to size response chunks.
uint16_t ble_get_mtu(void) {
    return 247;
}
