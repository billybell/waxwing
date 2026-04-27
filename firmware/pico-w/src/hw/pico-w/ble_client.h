#ifndef WAXWING_BLE_CLIENT_H
#define WAXWING_BLE_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Pico W BLE GATT-client surface — the Central role used by mesh-mode
 * peer sync. Lives alongside the existing peripheral path in ble.c
 * but is kept in a separate file so each path stays simple to read.
 *
 * Lifecycle a higher-level driver runs:
 *
 *   ble_client_init()                            // once at boot
 *
 *   loop:
 *     ble_client_start_scan()                    // on entering SCANNING phase
 *     ... wait for ble_client_on_peer_seen ...
 *     ble_client_stop_scan()
 *     ble_client_connect(addr, addr_type)        // returns false if busy
 *     ... wait for ble_client_on_connected ...
 *     ble_client_send_command(cbor_bytes, len)   // write to File Command
 *     ... wait for ble_client_on_response ...
 *     ... repeat sends / responses ...
 *     ble_client_disconnect()                    // on session DONE / ERROR
 *     ... wait for ble_client_on_disconnected ...
 *
 * Only one concurrent outbound connection. The peripheral side
 * (companion connections, peer-pulled-from-us connections) is
 * unaffected and continues to work via ble.c.
 *
 * All callbacks fire from btstack's run loop (cyw43_arch_poll), so
 * they may execute during ble_process(). Keep the handlers cheap and
 * non-blocking.
 */

/* AD-payload-derived peer summary: BD address + the 1-byte manifest
 * counter the peer is currently advertising. The counter defaults to
 * 0 if the advert doesn't carry one (older firmware) — which the
 * peer_table treats as a never-seen-this-value condition and triggers
 * a connect, which is the right behaviour. */
typedef struct {
    uint8_t  bd_addr[6];
    uint8_t  bd_addr_type;       /* 0 public, 1 random, etc. (btstack types) */
    uint8_t  tpk_prefix[8];      /* placeholder until we put TPK in the advert */
    uint8_t  manifest_version;
} ble_client_peer_t;

typedef void (*ble_client_on_peer_seen_cb)(const ble_client_peer_t *peer);
typedef void (*ble_client_on_connected_cb)(void);
typedef void (*ble_client_on_response_cb)(const uint8_t *data, size_t len);
typedef void (*ble_client_on_disconnected_cb)(void);

bool ble_client_init(void);

/* Start / stop passive scanning. Idempotent. Filtering for the
 * Waxwing service UUID happens internally; the on_peer_seen callback
 * is only fired for matches. */
void ble_client_start_scan(void);
void ble_client_stop_scan(void);

/* Initiate an outbound connection to the given BD address. Returns
 * false if a connection is already up or in progress. */
bool ble_client_connect(const uint8_t bd_addr[6], uint8_t bd_addr_type);

/* Write the supplied CBOR command bytes to the peer's File Command
 * characteristic (write-without-response). Returns false if no
 * outbound connection is up or characteristics haven't been
 * discovered yet. */
bool ble_client_send_command(const uint8_t *data, size_t len);

/* Tear down the outbound connection. on_disconnected fires when the
 * link is fully closed. */
void ble_client_disconnect(void);

/* Callback registration. */
void ble_client_set_on_peer_seen(ble_client_on_peer_seen_cb cb);
void ble_client_set_on_connected(ble_client_on_connected_cb cb);
void ble_client_set_on_response(ble_client_on_response_cb cb);
void ble_client_set_on_disconnected(ble_client_on_disconnected_cb cb);

#endif // WAXWING_BLE_CLIENT_H
