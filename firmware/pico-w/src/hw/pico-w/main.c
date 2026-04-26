#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "core/identity.h"
#include "core/constants.h"
#include "hw/pico-w/ble.h"
#include "core/filestore.h"
#include "core/commands.h"

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
        last_led_toggle = now;
        // Onboard LED is on the CYW43 chip, not RP2040
        ble_set_led(led_state);
    }
}

// Response buffer for file commands. Sized to fit the largest single
// notification we ever send (a chunked read of up to 256 bytes plus CBOR
// envelope).
static uint8_t resp_buf[512];

// BLE write callback: invoked when the central writes to the File Command
// characteristic. Hand the bytes to commands.c, write the response back
// over the File Response notification.
static void on_file_command(const uint8_t *data, size_t len) {
    int resp_len = commands_handle(data, len, resp_buf, sizeof(resp_buf));
    if (resp_len > 0 && ble_is_connected()) {
        printf("[main] send_file_response: %d bytes, ble_mtu=%u\r\n",
               resp_len, (unsigned)ble_get_mtu());
        ble_send_file_response(resp_buf, (size_t)resp_len);
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(500);  // give USB-CDC a moment to enumerate

    printf("\r\n");
    printf("WAXWING C-FIRMWARE v2.1 DEBUG BUILD\r\n");
    printf("\r\n");

    waxwing_identity_t identity;
    if (!waxwing_identity_load_or_generate(&identity)) {
        printf("[main] FATAL: identity init failed\r\n");
        while (1) { __asm__("nop"); }
    }

    printf("[main] Node name: %s\r\n", identity.node_name);
    printf("[main] Public key: %s...\r\n", identity.tpk_hex);
    printf("\r\n");

    if (fs_init() < 0) {
        printf("[main] FATAL: fs_init failed\r\n");
        while (1) { __asm__("nop"); }
    }
    printf("[main] Filesystem ready\r\n");

    if (!ble_init()) {
        printf("[main] FATAL: BLE init failed\r\n");
        while (1) { __asm__("nop"); }
    }

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

    printf("[main] identity set: %zu bytes\r\n", waxwing_identity_len);

    ble_set_on_write(on_file_command);

    // Advertising starts inside ble.c when BTSTACK_EVENT_STATE reports
    // HCI_STATE_WORKING, so we don't call ble_start_advertising() here.
    printf("[main] Ready, entering run loop...\r\n\r\n");

    while (true) {
        ble_process();
        led_update(ble_is_connected());
    }
}
