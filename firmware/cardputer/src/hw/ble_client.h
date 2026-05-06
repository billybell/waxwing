#ifndef WAXWING_CARDPUTER_BLE_CLIENT_H
#define WAXWING_CARDPUTER_BLE_CLIENT_H

// CardPuter BLE GATT-client surface — the Central role used by
// mesh-mode peer sync. Mirrors firmware/pico-w/src/hw/pico-w/ble_client.h
// so the upstream mesh driver in main.cpp can be a near-clone of the
// Pico W's main.c.
//
// Lifecycle:
//   ble_client_init()                            once at boot, after ble_init
//
//   loop:
//     ble_client_start_scan()                    when entering SCANNING
//     ... wait for ble_client_on_peer_seen ...
//     ble_client_stop_scan()
//     ble_client_connect(addr, addr_type)        returns false if busy
//     ... wait for ble_client_on_connected ...
//     ble_client_send_command(cbor_bytes, len)
//     ... wait for ble_client_on_response ...
//     ... repeat sends / responses ...
//     ble_client_disconnect()                    on session DONE / ERROR
//     ... wait for ble_client_on_disconnected ...
//
// Only one concurrent outbound connection. The peripheral side is
// unaffected and continues to work via ble.cpp.
//
// Unlike the Pico W's btstack-driven model where callbacks fire from
// the run loop directly, NimBLE-Arduino delivers callbacks on its own
// host task. To keep the mesh driver single-threaded and avoid stack
// blowups on SD I/O, NimBLE callbacks here just enqueue events to an
// internal queue. Call ble_client_process() from loop() to drain the
// queue and dispatch to the registered callbacks (which then run in
// the main task).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t bd_addr[6];          // big-endian (matches Pico W)
    uint8_t bd_addr_type;        // NimBLE BLE_ADDR_PUBLIC=0 / RANDOM=1
    uint8_t tpk_prefix[8];       // BD-addr stand-in until TPK is in advert
    uint8_t manifest_version;    // 1-byte hint from service-data AD
} ble_client_peer_t;

typedef void (*ble_client_on_peer_seen_cb)(const ble_client_peer_t *peer);
typedef void (*ble_client_on_connected_cb)(void);
typedef void (*ble_client_on_response_cb)(const uint8_t *data, size_t len);
typedef void (*ble_client_on_disconnected_cb)(void);

#ifdef __cplusplus
extern "C" {
#endif

bool ble_client_init(void);

void ble_client_start_scan(void);
void ble_client_stop_scan(void);

bool ble_client_connect(const uint8_t bd_addr[6], uint8_t bd_addr_type);
bool ble_client_send_command(const uint8_t *data, size_t len);
void ble_client_disconnect(void);

// True iff the currently-connected peer exposed the Peer Command
// characteristic during discovery. main.cpp branches on this to drive
// the encounter handshake when present, or fall back to legacy
// peer_sync against pre-M4 firmware. Only valid between on_connected
// and on_disconnected.
bool ble_client_uses_peer_characteristic(void);

// Drain the event queue, dispatching to registered callbacks. Call
// every loop iteration.
void ble_client_process(void);

void ble_client_set_on_peer_seen(ble_client_on_peer_seen_cb cb);
void ble_client_set_on_connected(ble_client_on_connected_cb cb);
void ble_client_set_on_response(ble_client_on_response_cb cb);
void ble_client_set_on_disconnected(ble_client_on_disconnected_cb cb);

#ifdef __cplusplus
}
#endif

#endif
