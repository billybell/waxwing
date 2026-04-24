#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "tusb.h"
#include "identity.h"
#include "constants.h"
#include "ble.h"

// LED blink interval (ms)
#define LED_BLINK_CONNECTED    100
#define LED_BLINK_DISCONNECTED 1000

static bool led_state = false;
static uint32_t last_led_toggle = 0;

static void led_update(bool connected) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t interval = connected ? LED_BLINK_CONNECTED : LED_BLINK_DISCONNECTED;

    if (now - last_led_toggle >= interval) {
        led_state = !led_state;
        // Use CYW43 GPIO API (the onboard LED is on the CYW43 chip, not RP2040)
        ble_set_led(led_state);
     }
}

// BLE write callback: invoked when central writes to File Command characteristic.
// TODO: Decode CBOR command from filestore/messages and dispatch operations.
static void on_file_command(const uint8_t *data, size_t len) {
    printf("[main] File command received (%zu bytes)\r\n", len);

      // Phase 1 echo: send back the same data as a response to verify the
      // BLE data path works end-to-end. Replace with real logic when
      // filestore.c is ported (Phase 4).
    if (ble_is_connected()) {
        ble_send_file_response(data, len);
     }
}

int main(void) {
     // Initialize serial output (UART0 + USB)
    stdio_init_all();

      // Wait for USB to be ready (enumerate with host).
     // 500ms covers most cases; much faster than hardcoded 2s sleep.
    sleep_ms(500);

    printf("\r\n");
    printf("WAXWING C-FIRMWARE v2.1 DEBUG BUILD\r\n");
    printf("\r\n");

      // Load or generate persistent node identity from flash
    waxwing_identity_t identity;
    if (!waxwing_identity_load_or_generate(&identity)) {
        printf("[main] FATAL: identity init failed\r\n");
        while (1) { __asm__("nop"); }
     }

    printf("[main] Node name: %s\r\n", identity.node_name);
    printf("[main] Public key: %s...\r\n", identity.tpk_hex);
    printf("\r\n");

      // Initialize BLE stack (this also initializes CYW43 chip)
    if (!ble_init()) {
        printf("[main] FATAL: BLE init failed\r\n");
        while (1) { __asm__("nop"); }
     }

      // Build and publish device identity for BLE GATT server
    waxwing_device_identity_t ble_identity;
    ble_identity.protocol_version = 1;
    snprintf(ble_identity.protocol_name, sizeof(ble_identity.protocol_name), "%s", PROTOCOL_NAME);
    snprintf(ble_identity.node_name, sizeof(ble_identity.node_name), "%s", identity.node_name);
    memcpy(ble_identity.tpk, identity.pub, 32);
    snprintf(ble_identity.tpk_hex, sizeof(ble_identity.tpk_hex), "%s", identity.tpk_hex);
    memcpy(ble_identity.tpk_fingerprint, identity.fingerprint, 8);
    ble_identity.capabilities = PICO_W_CAPS;
    ble_identity.manifest_count = 0;
    ble_identity.mode = CAP_UNATTENDED;

    ble_set_identity(&ble_identity);

    printf("[main] identity set: %u bytes\r\n", waxwing_identity_len);

    ble_set_on_write(on_file_command);

    // Advertising starts inside ble.c when BTSTACK_EVENT_STATE reports
    // HCI_STATE_WORKING, so we don't call ble_start_advertising() here.
    printf("[main] Ready, entering BLE run loop...\r\n\r\n");

    while (true) {
        ble_process();
        led_update(ble_is_connected());
    }
}
