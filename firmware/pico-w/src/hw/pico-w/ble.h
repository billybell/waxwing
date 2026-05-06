#ifndef WAXWING_BLE_H
#define WAXWING_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// BLE Service UUID (128-bit, little-endian bytes for bluetooth module)
// CE575800-494E-4700-8000-00805F9B34FB
#define SERVICE_UUID_STR "CE575800-494E-4700-8000-00805F9B34FB"

// Characteristic UUIDs
#define CHAR_DEVICE_IDENTITY       "CE575801-494E-4700-8000-00805F9B34FB"
#define CHAR_FILE_COMMAND          "CE57580D-494E-4700-8000-00805F9B34FB"
#define CHAR_FILE_RESPONSE         "CE57580E-494E-4700-8000-00805F9B34FB"
#define CHAR_PEER_COMMAND          "CE57580F-494E-4700-8000-00805F9B34FB"

// Maximum size for BLE characteristic data
#define BLE_MAX_DATA_SIZE 256

/**
 * Structured device identity (for building CBOR-encoded payloads).
 * Phase 1: ble.c serializes this into waxwing_identity_data[] on set.
 */
typedef struct {
    uint8_t protocol_version;
    char protocol_name[16];
    char node_name[16];
    uint8_t tpk[32];                // Transport public key
    char tpk_hex[65];               // TPK as hex string
    uint8_t tpk_fingerprint[8];     // Truncated public key fingerprint
    uint16_t capabilities;
    uint32_t manifest_count;
    uint8_t mode;
} waxwing_device_identity_t;

// BLE callback function types
typedef void (*ble_on_connect_cb)(uint16_t conn_handle);
typedef void (*ble_on_disconnect_cb)(uint16_t conn_handle);
typedef void (*ble_on_write_cb)(const uint8_t *data, size_t len);

/**
 * Public API - call from main.c or other modules.
 */

// Initialize BLE stack (CYW43, L2CAP, ATT server, advertising).
// Must be called before any other ble_* functions.
// Returns true on success.
bool ble_init(void);

// Set device identity and populate the Device Identity characteristic.
// Calls att_server_write_characteristic with a minimal CBOR blob.
void ble_set_identity(const waxwing_device_identity_t *identity);

// Directly set raw (CBOR-encoded) identity bytes.
void ble_set_identity_raw(const uint8_t *data, size_t len);

// Start advertising at ~500ms intervals with flags + service UUID + node name.
void ble_start_advertising(void);

// Stop advertising.
void ble_stop_advertising(void);

// Update the 1-byte manifest_version we advertise in the service-data
// block. Cheap to call — pushes a fresh AD payload to the controller
// without touching scan response or restarting advertising. Safe to
// call before advertising has started; the value will take effect at
// the first start_advertising_internal().
void ble_set_manifest_version(uint8_t version);

// Process BLE events (btstack is IRQ-driven but kept for API compatibility).
void ble_process(void);

// Check if currently connected to a central.
bool ble_is_connected(void);

// Get current connection handle (0xFFFF if not connected).
uint16_t ble_get_conn_handle(void);

// Get the negotiated ATT MTU for the current connection. Returns the
// BLE default of 23 when there is no active connection or before MTU
// exchange has completed.
uint16_t ble_get_mtu(void);

// Send notification on File Response characteristic.
// Returns true if queued successfully, false otherwise.
bool ble_send_file_response(const uint8_t *data, size_t len);

// Set the onboard CYW43 LED state (main.c calls this for blink patterns).
void ble_set_led(bool on);

// Register callbacks (routed through btstack ATT packet handler).
void ble_set_on_connect(ble_on_connect_cb cb);
void ble_set_on_disconnect(ble_on_disconnect_cb cb);
void ble_set_on_write(ble_on_write_cb cb);

// Register a callback for writes to the peer command characteristic.
// Same shape as ble_set_on_write but fires from the peer-mode handle.
// Used by main.c to drive the encounter handshake on the responder
// side. Writes to the companion characteristic still go through
// ble_set_on_write.
void ble_set_on_peer_write(ble_on_write_cb cb);

/**
 * External buffers for CBOR-encoded data (defined in ble.c).
 * Used by main.c/message module to set characteristic values.
 */
extern uint8_t waxwing_identity_data[BLE_MAX_DATA_SIZE];
extern size_t  waxwing_identity_len;

#endif // WAXWING_BLE_H
