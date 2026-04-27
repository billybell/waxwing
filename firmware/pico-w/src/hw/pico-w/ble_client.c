/*
 * BLE GATT-client side of the Pico W firmware. Used by mesh-mode peer
 * sync to scan for nearby Waxwing nodes, connect as Central, write
 * file commands, and receive notifications. Independent of the
 * peripheral path in ble.c — the two share the btstack instance but
 * register their own packet handlers and dispatch on the LE
 * connection role.
 */

#include "hw/pico-w/ble_client.h"

#include <string.h>
#include <stdio.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"

// Waxwing service UUID, two byte-orders for two consumers:
//
//   _BE: big-endian (gatt_client_* APIs take a uint8[16] in this form;
//        same byte-order as the SERVICE_UUID string in constants.h).
//   _LE: little-endian (the bytes as they appear on-air in advertising
//        packets — see the AD payload in ble.c::start_advertising_internal).
//
// Source UUID: CE575800-494E-4700-8000-00805F9B34FB
static const uint8_t WAXWING_UUID_BE[16] = {
    0xCE, 0x57, 0x58, 0x00, 0x49, 0x4E, 0x47, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB,
};
static const uint8_t WAXWING_UUID_LE[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x47, 0x4E, 0x49, 0x00, 0x58, 0x57, 0xCE,
};

// File Command (W) and File Response (N) characteristic UUIDs (big-endian).
static const uint8_t CHAR_FILE_COMMAND_BE[16] = {
    0xCE, 0x57, 0x58, 0x0D, 0x49, 0x4E, 0x47, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB,
};
static const uint8_t CHAR_FILE_RESPONSE_BE[16] = {
    0xCE, 0x57, 0x58, 0x0E, 0x49, 0x4E, 0x47, 0x00,
    0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB,
};

// ============================================================
// State
// ============================================================

typedef enum {
    CLIENT_IDLE,
    CLIENT_SCANNING,
    CLIENT_CONNECTING,
    CLIENT_DISCOVERING_SERVICE,
    CLIENT_DISCOVERING_CHARS,
    CLIENT_SUBSCRIBING,
    CLIENT_READY,
    CLIENT_DISCONNECTING,
} client_state_t;

static struct {
    client_state_t state;
    hci_con_handle_t conn_handle;
    gatt_client_service_t service;
    gatt_client_characteristic_t char_cmd;
    gatt_client_characteristic_t char_resp;
    gatt_client_notification_t   notif_listener;
    int    chars_seen;     /* how many target characteristics we've
                              picked up so far in the current discovery */

    ble_client_on_peer_seen_cb     on_peer_seen;
    ble_client_on_connected_cb     on_connected;
    ble_client_on_response_cb      on_response;
    ble_client_on_disconnected_cb  on_disconnected;
} g_client;

static btstack_packet_callback_registration_t g_hci_handler;

// ============================================================
// Forward declarations
// ============================================================

static void hci_event_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size);
static void gatt_client_event_handler(uint8_t packet_type, uint16_t channel,
                                      uint8_t *packet, uint16_t size);
static void notification_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size);
static void parse_advertising_report(uint8_t *packet);

// ============================================================
// Public API
// ============================================================

bool ble_client_init(void) {
    memset(&g_client, 0, sizeof(g_client));
    g_client.state = CLIENT_IDLE;
    g_client.conn_handle = HCI_CON_HANDLE_INVALID;

    g_hci_handler.callback = &hci_event_handler;
    hci_add_event_handler(&g_hci_handler);

    // We use the GATT client; init it once. (l2cap_init has already
    // been called by ble_init in ble.c.)
    gatt_client_init();
    return true;
}

void ble_client_start_scan(void) {
    if (g_client.state != CLIENT_IDLE) return;
    // Passive scan: 30 ms interval, 30 ms window. Aggressive enough to
    // catch ~100 ms advertisements reliably without saturating the
    // radio. Phase 1; can tune later.
    gap_set_scan_parameters(/*type=*/0, /*interval=*/0x0030,
                            /*window=*/0x0030);
    gap_start_scan();
    g_client.state = CLIENT_SCANNING;
    printf("[ble_client] scanning\r\n");
}

void ble_client_stop_scan(void) {
    if (g_client.state != CLIENT_SCANNING) return;
    gap_stop_scan();
    g_client.state = CLIENT_IDLE;
    printf("[ble_client] scan stopped\r\n");
}

bool ble_client_connect(const uint8_t bd_addr[6], uint8_t bd_addr_type) {
    if (g_client.state != CLIENT_SCANNING && g_client.state != CLIENT_IDLE) {
        return false;
    }
    if (g_client.state == CLIENT_SCANNING) {
        gap_stop_scan();
    }
    bd_addr_t addr;
    memcpy(addr, bd_addr, 6);
    g_client.state = CLIENT_CONNECTING;
    if (gap_connect(addr, (bd_addr_type_t)bd_addr_type) != 0) {
        g_client.state = CLIENT_IDLE;
        return false;
    }
    printf("[ble_client] connecting to %02x:%02x:%02x:%02x:%02x:%02x\r\n",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return true;
}

bool ble_client_send_command(const uint8_t *data, size_t len) {
    if (g_client.state != CLIENT_READY ||
        g_client.conn_handle == HCI_CON_HANDLE_INVALID) {
        printf("[ble_client] send refused: state=%d handle=0x%04x\r\n",
               (int)g_client.state, g_client.conn_handle);
        return false;
    }
    uint8_t status = gatt_client_write_value_of_characteristic_without_response(
        g_client.conn_handle,
        g_client.char_cmd.value_handle,
        (uint16_t)len, (uint8_t *)data);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[ble_client] write failed: status=0x%02x\r\n", status);
        return false;
    }
    return true;
}

void ble_client_disconnect(void) {
    if (g_client.conn_handle == HCI_CON_HANDLE_INVALID) return;
    g_client.state = CLIENT_DISCONNECTING;
    gap_disconnect(g_client.conn_handle);
}

void ble_client_set_on_peer_seen(ble_client_on_peer_seen_cb cb) {
    g_client.on_peer_seen = cb;
}
void ble_client_set_on_connected(ble_client_on_connected_cb cb) {
    g_client.on_connected = cb;
}
void ble_client_set_on_response(ble_client_on_response_cb cb) {
    g_client.on_response = cb;
}
void ble_client_set_on_disconnected(ble_client_on_disconnected_cb cb) {
    g_client.on_disconnected = cb;
}

// ============================================================
// Advertising-report parsing
//
// Walk the AD payload looking for either of the two AD types we
// expect to carry our service UUID:
//
//   0x07  Complete List of 128-bit Service Class UUIDs   (current adverts)
//   0x21  Service Data — 128-bit UUID                    (post step-6 adverts)
//
// Service Data also carries optional payload bytes after the UUID;
// when we get there the first such byte will be the manifest_version.
// We pull that out when present and default to 0 otherwise.
// ============================================================

static void parse_advertising_report(uint8_t *packet) {
    bd_addr_t   addr;
    bd_addr_type_t addr_type;
    uint8_t     data_len;
    const uint8_t *data;

    gap_event_advertising_report_get_address(packet, addr);
    addr_type = (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet);
    data_len  = gap_event_advertising_report_get_data_length(packet);
    data      = gap_event_advertising_report_get_data(packet);

    bool match = false;
    uint8_t manifest_version = 0;

    int i = 0;
    while (i < data_len) {
        uint8_t field_len  = data[i];
        if (field_len == 0 || (i + 1 + field_len) > data_len) break;
        uint8_t field_type = data[i + 1];

        if (field_type == BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS &&
            field_len >= 1 + 16) {
            if (memcmp(&data[i + 2], WAXWING_UUID_LE, 16) == 0) {
                match = true;
            }
        } else if (field_type == BLUETOOTH_DATA_TYPE_SERVICE_DATA_128_BIT_UUID &&
                   field_len >= 1 + 16) {
            if (memcmp(&data[i + 2], WAXWING_UUID_LE, 16) == 0) {
                match = true;
                if (field_len >= 1 + 16 + 1) {
                    manifest_version = data[i + 2 + 16];
                }
            }
        }
        i += 1 + field_len;
    }

    if (!match) return;

    ble_client_peer_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.bd_addr, addr, 6);
    peer.bd_addr_type     = (uint8_t)addr_type;
    peer.manifest_version = manifest_version;
    // tpk_prefix stays zero until we put it in the advert. peer_table
    // currently keys on 8-byte prefix; using BD address bytes as a
    // stand-in until then keeps decisions stable across scans.
    memcpy(peer.tpk_prefix, addr, 6);

    if (g_client.on_peer_seen) g_client.on_peer_seen(&peer);
}

// ============================================================
// HCI / GATT client event handling
// ============================================================

static void hci_event_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case GAP_EVENT_ADVERTISING_REPORT:
            if (g_client.state == CLIENT_SCANNING) {
                parse_advertising_report(packet);
            }
            break;

        case HCI_EVENT_LE_META: {
            if (hci_event_le_meta_get_subevent_code(packet) !=
                HCI_SUBEVENT_LE_CONNECTION_COMPLETE) break;

            // The peripheral path in ble.c also receives this event;
            // it filters on role==SLAVE. We filter on role==MASTER so
            // the two paths stay disjoint.
            uint8_t role =
                hci_subevent_le_connection_complete_get_role(packet);
            if (role != HCI_ROLE_MASTER) break;
            if (g_client.state != CLIENT_CONNECTING) break;

            uint8_t status =
                hci_subevent_le_connection_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[ble_client] connect failed: 0x%02x\r\n", status);
                g_client.state = CLIENT_IDLE;
                if (g_client.on_disconnected) g_client.on_disconnected();
                break;
            }
            g_client.conn_handle =
                hci_subevent_le_connection_complete_get_connection_handle(packet);
            g_client.state = CLIENT_DISCOVERING_SERVICE;
            g_client.chars_seen = 0;
            printf("[ble_client] connected, handle=0x%04x — discovering\r\n",
                   g_client.conn_handle);
            gatt_client_discover_primary_services_by_uuid128(
                gatt_client_event_handler, g_client.conn_handle,
                WAXWING_UUID_BE);
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t h =
                hci_event_disconnection_complete_get_connection_handle(packet);
            if (h != g_client.conn_handle) break;     // not ours
            printf("[ble_client] disconnected, reason=0x%02x\r\n",
                   hci_event_disconnection_complete_get_reason(packet));
            g_client.conn_handle = HCI_CON_HANDLE_INVALID;
            g_client.state = CLIENT_IDLE;
            if (g_client.on_disconnected) g_client.on_disconnected();
            break;
        }

        default: break;
    }
}

static void gatt_client_event_handler(uint8_t packet_type, uint16_t channel,
                                      uint8_t *packet, uint16_t size) {
    (void)packet_type; (void)channel; (void)size;

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            gatt_event_service_query_result_get_service(packet,
                                                        &g_client.service);
            break;

        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            gatt_client_characteristic_t c;
            gatt_event_characteristic_query_result_get_characteristic(packet, &c);
            if (memcmp(c.uuid128, CHAR_FILE_COMMAND_BE, 16) == 0) {
                g_client.char_cmd = c;
                g_client.chars_seen++;
            } else if (memcmp(c.uuid128, CHAR_FILE_RESPONSE_BE, 16) == 0) {
                g_client.char_resp = c;
                g_client.chars_seen++;
            }
            break;
        }

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            if (status != ATT_ERROR_SUCCESS) {
                printf("[ble_client] gatt query failed: 0x%02x\r\n", status);
                ble_client_disconnect();
                break;
            }

            switch (g_client.state) {
                case CLIENT_DISCOVERING_SERVICE:
                    g_client.state = CLIENT_DISCOVERING_CHARS;
                    gatt_client_discover_characteristics_for_service(
                        gatt_client_event_handler,
                        g_client.conn_handle,
                        &g_client.service);
                    break;

                case CLIENT_DISCOVERING_CHARS:
                    if (g_client.chars_seen < 2) {
                        printf("[ble_client] missing characteristics "
                               "(saw %d/2); disconnecting\r\n",
                               g_client.chars_seen);
                        ble_client_disconnect();
                        break;
                    }
                    g_client.state = CLIENT_SUBSCRIBING;
                    gatt_client_listen_for_characteristic_value_updates(
                        &g_client.notif_listener,
                        notification_handler,
                        g_client.conn_handle,
                        &g_client.char_resp);
                    gatt_client_write_client_characteristic_configuration(
                        gatt_client_event_handler,
                        g_client.conn_handle,
                        &g_client.char_resp,
                        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                    break;

                case CLIENT_SUBSCRIBING:
                    g_client.state = CLIENT_READY;
                    printf("[ble_client] ready\r\n");
                    if (g_client.on_connected) g_client.on_connected();
                    break;

                default: break;
            }
            break;
        }

        default: break;
    }
}

static void notification_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
    (void)packet_type; (void)channel; (void)size;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    const uint8_t *value = gatt_event_notification_get_value(packet);
    uint16_t       vlen  = gatt_event_notification_get_value_length(packet);
    if (g_client.on_response) g_client.on_response(value, (size_t)vlen);
}
