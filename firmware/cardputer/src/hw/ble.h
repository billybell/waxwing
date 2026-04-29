#ifndef WAXWING_CARDPUTER_BLE_H
#define WAXWING_CARDPUTER_BLE_H

// CardPuter BLE port. API surface intentionally mirrors
// firmware/pico-w/src/hw/pico-w/ble.h so the same upstream wiring
// (commands.c dispatch, manifest-counter pushes from main, etc.)
// drops in once the filestore + crypto HALs are implemented.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WAXWING_SERVICE_UUID  "CE575800-494E-4700-8000-00805F9B34FB"
#define WAXWING_CHAR_IDENTITY "CE575801-494E-4700-8000-00805F9B34FB"
#define WAXWING_CHAR_FILE_CMD "CE57580D-494E-4700-8000-00805F9B34FB"
#define WAXWING_CHAR_FILE_RSP "CE57580E-494E-4700-8000-00805F9B34FB"

#define BLE_MAX_DATA_SIZE 256

typedef void (*ble_on_connect_cb)(uint16_t conn_handle);
typedef void (*ble_on_disconnect_cb)(uint16_t conn_handle);
typedef void (*ble_on_write_cb)(const uint8_t *data, size_t len);

#ifdef __cplusplus
extern "C" {
#endif

bool     ble_init(void);
void     ble_set_identity_raw(const uint8_t *data, size_t len);
void     ble_set_manifest_version(uint8_t version);
// Update the name advertised in the scan response. Safe to call
// before or after advertising starts; takes effect at the next
// advertising data publish.
void     ble_set_node_name(const char *name);
void     ble_start_advertising(void);
void     ble_stop_advertising(void);
void     ble_process(void);
bool     ble_is_connected(void);
uint16_t ble_get_conn_handle(void);
uint16_t ble_get_mtu(void);
bool     ble_send_file_response(const uint8_t *data, size_t len);

void ble_set_on_connect(ble_on_connect_cb cb);
void ble_set_on_disconnect(ble_on_disconnect_cb cb);
void ble_set_on_write(ble_on_write_cb cb);

#ifdef __cplusplus
}
#endif

#endif
