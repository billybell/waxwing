/**
    * waxwing/ble.c - BLE GATT server for Waxwing Mesh (Pico W)
    *
    * Built on BTstack running on CYW43 chip via pico_btstack_cyw43.
    * Phase 1 characteristics: Device Identity (READ), File Command (WRITE),
    * File Response (READ + NOTIFY).
    */

#include "ble.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

// BTstack umbrella header (like gatt_counter.c example)
#include "btstack.h"

// pico SDK + CYW43 integration
#include "pico/cyw43_arch.h"

// Pico SDK
#include "pico/stdlib.h"

// Characteristic handles from generated GATT database
#include "waxwing_ble_att_server.h"

// Fallback if board header not transitively included
#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif

// ============================================================
// Internal state
// ============================================================

static bool      g_connected     = false;
static uint16_t  g_conn_handle   = 0xFFFF;
static bool      g_notif_enabled = false;

// CBOR-encoded identity data (set via ble_set_identity / ble_set_identity_raw)
uint8_t  waxwing_identity_data[BLE_MAX_DATA_SIZE];
size_t   waxwing_identity_len = 0;

// File Command buffer - receives raw data from central
static uint8_t g_file_cmd_buf[BLE_MAX_DATA_SIZE];
static size_t  g_file_cmd_len = 0;

// File Response buffer - written by ble_send_file_response, sent via notify
static uint8_t g_file_resp_buf[BLE_MAX_DATA_SIZE];
static size_t  g_file_resp_len = 0;

// Node name for scan response (set via ble_set_identity)
static char g_node_name[16] = "Waxwing";

// Callbacks (set from main.c)
static ble_on_connect_cb    g_on_connect_cb      = NULL;
static ble_on_disconnect_cb g_on_disconnect_cb = NULL;
static ble_on_write_cb      g_on_write_cb        = NULL;

// BTstack packet handler registration
static btstack_packet_callback_registration_t g_hci_event_callback_reg;

// ============================================================
// Forward declarations
// ============================================================

static void  packet_handler(uint8_t packet_type, uint16_t channel,
                            uint8_t *packet, uint16_t size);
static void  att_event_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size);
static uint16_t att_read_cb(hci_con_handle_t conn_handle, uint16_t att_handle,
                            uint16_t offset, uint8_t *buf, uint16_t buf_size);
static int    att_write_cb(hci_con_handle_t conn_handle, uint16_t att_handle,
                           uint16_t transaction_mode, uint16_t offset,
                           uint8_t *buffer, uint16_t buffer_size);
static void   start_advertising_internal(void);

// ============================================================
// Helpers
// ============================================================

/// Encode a CBOR unsigned integer (major type 0). Returns bytes written (1-5).
///
/// CBOR major type is the top 3 bits of the initial byte. For unsigned int
/// that is 000 — so the header is `0x00 | additional_info`. The previous
/// small-value branch used `0x20 | value`, which is major type 1 (negative
/// integer) and was decoded by iOS/Python as `-1 - value`; that silently
/// corrupted every small uint in the identity payload and caused the
/// iOS-side `DeviceIdentity.fromCBOR` to reject the blob on the `v` field.
static size_t cbor_encode_uint(uint8_t *buf, uint32_t value) {
    if (value <= 23) {
        buf[0] = (uint8_t)value;                  // major type 0, inline value
        return 1;
    } else if (value <= 0xFF) {
        buf[0] = 0x18;                            // 1-byte argument follows
        buf[1] = (uint8_t)value;
        return 2;
    } else if (value <= 0xFFFF) {
        buf[0] = 0x19;                            // 2-byte argument follows
        buf[1] = (value >> 8) & 0xFF;
        buf[2] = value & 0xFF;
        return 3;
    } else {
        buf[0] = 0x1A;                            // 4-byte argument follows
        buf[1] = (value >> 24) & 0xFF;
        buf[2] = (value >> 16) & 0xFF;
        buf[3] = (value >> 8) & 0xFF;
        buf[4] = value & 0xFF;
        return 5;
    }
}

/// Encode a CBOR text string. Returns number of bytes written.
static size_t cbor_encode_text_str(uint8_t *buf, const char *str, uint16_t len) {
    if (len <= 23) {
        buf[0] = (uint8_t)(0x60 | len);
        memcpy(&buf[1], str, len);
        return 1 + len;
     } else {
        buf[0] = 0x78;
        buf[1] = (uint8_t)len;
        memcpy(&buf[2], str, len);
        return 2 + len;
     }
}

/// Encode a CBOR byte string. Returns number of bytes written.
static size_t cbor_encode_byte_str(uint8_t *buf, const uint8_t *data, uint16_t len) {
    if (len <= 23) {
        buf[0] = (uint8_t)(0x40 | len);
        memcpy(&buf[1], data, len);
        return 1 + len;
     } else if (len <= 0xFF) {
        buf[0] = 0x58;
        buf[1] = (uint8_t)len;
        memcpy(&buf[2], data, len);
        return 2 + len;
     } else {
        buf[0] = 0x59;
        buf[1] = (len >> 8) & 0xFF;
        buf[2] = len & 0xFF;
        memcpy(&buf[3], data, len);
        return 3 + len;
     }
}

/// Serialise waxwing_device_identity_t into a single CBOR map.
/// Produces the Phase-1 schema expected by iOS/Python:
///     { "protocol": str, "v": int, "name": str, "tpk": bytes, "caps": int,
///       "firmware": str, "firmware_ver": str, "attended": bool,
///       "unattended_mode": str|<absent>, "manifest_count": int,
///       "timestamp": int }
static void build_identity_from_struct(const waxwing_device_identity_t *id) {
    size_t pos = 0;

    // 11 fields when unattended_mode is present, 10 otherwise. `id->mode == 0`
    // means attended (no unattended_mode key).
    uint8_t n_fields = (id->mode == 0) ? 10 : 11;

    // Map header (major type 5). n_fields ≤ 23 → single byte header.
    waxwing_identity_data[pos++] = (uint8_t)(0xA0 | n_fields);

    // "protocol" = "waxwing-mesh"
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "protocol", 8);
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "waxwing-mesh", 12);

    // "v" = uint (protocol version)
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "v", 1);
    pos += cbor_encode_uint(&waxwing_identity_data[pos], id->protocol_version);

    // "name" = human-readable node identifier (e.g. "WX:AABBCCDD")
    size_t name_len = strnlen(id->node_name, sizeof(id->node_name));
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "name", 4);
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], id->node_name,
                                (uint16_t)name_len);

    // "tpk" = byte string(32) transport public key
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "tpk", 3);
    pos += cbor_encode_byte_str(&waxwing_identity_data[pos], id->tpk, 32);

    // "caps" = uint (capability bitmask)
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "caps", 4);
    pos += cbor_encode_uint(&waxwing_identity_data[pos], id->capabilities);

    // "firmware" = "pico-w"
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "firmware", 8);
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "pico-w", 6);

    // "firmware_ver" = "0.1.0"
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "firmware_ver", 12);
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "0.1.0", 5);

    // "attended" = bool (true when mode == 0, false otherwise)
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "attended", 8);
    waxwing_identity_data[pos++] = (id->mode == 0) ? 0xF5 : 0xF4;  // true/false

    // "unattended_mode" = str (only present when mode != 0)
    if (id->mode != 0) {
        pos += cbor_encode_text_str(&waxwing_identity_data[pos], "unattended_mode", 15);
        pos += cbor_encode_text_str(&waxwing_identity_data[pos], "relay", 5);
    }

    // "manifest_count" = uint (always 0 for Phase 1)
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "manifest_count", 14);
    pos += cbor_encode_uint(&waxwing_identity_data[pos], 0);

    // "timestamp" = uint (placeholder: hardcoded to 1 — real RTC in Phase 2)
    pos += cbor_encode_text_str(&waxwing_identity_data[pos], "timestamp", 9);
    pos += cbor_encode_uint(&waxwing_identity_data[pos], 1);

    waxwing_identity_len = pos;

    printf("[ble] identity CBOR blob: %zu bytes, first 16:", pos);
    for (size_t i = 0; i < pos && i < 16; i++) {
        printf(" %02x", waxwing_identity_data[i]);
    }
    printf("\r\n");
}

// ============================================================
// ATT Read Callback
// ============================================================

static uint16_t att_read_cb(hci_con_handle_t conn_handle, uint16_t att_handle,
                            uint16_t offset, uint8_t *buf, uint16_t buf_size) {
     (void)conn_handle;

    switch (att_handle) {
        case ATT_CHARACTERISTIC_CE575801_494E_4700_8000_00805F9B34FB_01_VALUE_HANDLE:
              // Device Identity - return CBOR blob, chunked by offset
            if (buf == NULL) {
                return (uint16_t)waxwing_identity_len;
             }
            return att_read_callback_handle_blob(
                    waxwing_identity_data, (uint16_t)waxwing_identity_len,
                    offset, buf, buf_size);

        case ATT_CHARACTERISTIC_CE57580D_494E_4700_8000_00805F9B34FB_01_VALUE_HANDLE:
              // File Command - not readable (WRITE-only)
            return att_read_callback_handle_blob(g_file_cmd_buf,
                     (uint16_t)g_file_cmd_len, offset, buf, buf_size);

        case ATT_CHARACTERISTIC_CE57580E_494E_4700_8000_00805F9B34FB_01_VALUE_HANDLE:
              // File Response - return pending response data
            return att_read_callback_handle_blob(g_file_resp_buf,
                     (uint16_t)g_file_resp_len, offset, buf, buf_size);

        default:
            return 0;
     }
}

// ============================================================
// ATT Write Callback
// ============================================================

static int att_write_cb(hci_con_handle_t conn_handle, uint16_t att_handle,
                        uint16_t transaction_mode, uint16_t offset,
                        uint8_t *buffer, uint16_t buffer_size) {
     (void)conn_handle;
     (void)transaction_mode;
     (void)offset;

    switch (att_handle) {
        case ATT_CHARACTERISTIC_CE57580E_494E_4700_8000_00805F9B34FB_01_CLIENT_CONFIGURATION_HANDLE:
              // CCCD write: enable/disable notifications on File Response
            if (buffer_size == 2) {
                g_notif_enabled = (little_endian_read_16(buffer, 0) ==
                        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
             }
            break;

        case ATT_CHARACTERISTIC_CE57580D_494E_4700_8000_00805F9B34FB_01_VALUE_HANDLE:
              // File Command write from central - store and forward to callback
            if (buffer_size > 0 && buffer_size <= BLE_MAX_DATA_SIZE) {
                memcpy(g_file_cmd_buf, buffer, buffer_size);
                g_file_cmd_len = buffer_size;

                if (g_on_write_cb != NULL) {
                    g_on_write_cb(g_file_cmd_buf, g_file_cmd_len);
                  }
             }
            break;

        default:
            break;
     }
    return 0;
}

// ============================================================
// CCCD Clearing (iOS compatibility fix)
// ============================================================

/// Clear CCCD notification state on disconnect.
/// btstack auto-clears per-connection CCCD state via MAX_NR_GATT_CLIENTS.
/// iOS caches subscription state at its own BLE stack level - if we hit
/// the "stale subscription" issue seen in MicroPython, add GATT DB write
/// here to force-clear: gatts_write(cccd_handle, 0x0000).
static void clear_cccd(void) {
      // Also reset characteristic value to prevent stale data on reconnect
    g_file_resp_len = 0;
}

// ============================================================
// Packet Handler (BLE state + disconnect)
// ============================================================

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    uint16_t conn_handle;
     (void)channel;
     (void)size;

      // Only process HCI event packets
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
              // BLE stack powered on when state == BTSTACK_STATE_STARTED (3)
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                printf("[ble] BLE enabled\r\n");
                start_advertising_internal();
              } else {
                printf("[ble] BLE disabled (state=%u)\r\n",
                        btstack_event_state_get_state(packet));
              }
            break;

        case HCI_EVENT_LE_META:
              // Track connection so g_connected / g_conn_handle are valid for notifies.
            if (hci_event_le_meta_get_subevent_code(packet) ==
                    HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                g_conn_handle =
                    hci_subevent_le_connection_complete_get_connection_handle(packet);
                g_connected     = true;
                g_notif_enabled = false;
                printf("[ble] connected, handle=0x%04x\r\n", g_conn_handle);
                if (g_on_connect_cb != NULL) {
                    g_on_connect_cb(g_conn_handle);
                  }
              }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            conn_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            g_connected = false;
            g_conn_handle = 0xFFFF;
            g_notif_enabled = false;
            clear_cccd();
            printf("[ble] disconnected, reason=0x%02x\r\n",
                    hci_event_disconnection_complete_get_reason(packet));

            if (g_on_disconnect_cb != NULL) {
                g_on_disconnect_cb(conn_handle);
              }
              // BTstack keeps advertising parameters between connections; just re-enable.
            gap_advertisements_enable(1);
            break;

        default:
            break;
     }
}

// ============================================================
// ATT Event Handler (notifications)
// ============================================================

static void att_event_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
     (void)packet_type;
     (void)channel;
     (void)size;

    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case ATT_EVENT_CAN_SEND_NOW:
              // Send pending notification on File Response characteristic
            if (g_notif_enabled && g_file_resp_len > 0 && g_conn_handle != 0xFFFF) {
                att_server_notify(g_conn_handle,
                    ATT_CHARACTERISTIC_CE57580E_494E_4700_8000_00805F9B34FB_01_VALUE_HANDLE,
                    g_file_resp_buf, (uint16_t)g_file_resp_len);
                  // Clear after sending to avoid stale data on re-send
                g_file_resp_len = 0;
              }
            break;

        default:
            break;
     }
}

// ============================================================
// Advertising
// ============================================================

static void start_advertising_internal(void) {
    uint16_t adv_int_min = 0x00A0;      // 100ms (units of 0.625ms)
    uint16_t adv_int_max = 0x00A0;
    uint8_t  adv_type      = 0;           // Connectable undirected (ADV_IND)
    bd_addr_t null_addr    = { 0 };

      // Advertising packet: flags + complete list of 128-bit service UUIDs.
      // 128-bit UUIDs go on the wire LITTLE-ENDIAN, so CE575800-494E-4700-
      // 8000-00805F9B34FB is emitted LSB-first as the 16 bytes below. The
      // UUID block's length byte is 0x11 (1 type byte + 16 UUID bytes).
    static const uint8_t adv_data[] = {
          0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
          0x11, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
          0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
          0x00, 0x47, 0x4e, 0x49, 0x00, 0x58, 0x57, 0xce,
      };

      // Scan response: complete local name (e.g. "WX:AABBCCDD").
      // Max AD payload is 31 bytes; 2 bytes go to length+type headers.
    static uint8_t scan_rsp[31];
    size_t name_len = strlen(g_node_name);
    if (name_len > 29) name_len = 29;
    scan_rsp[0] = (uint8_t)(name_len + 1);
    scan_rsp[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&scan_rsp[2], g_node_name, name_len);
    uint8_t scan_rsp_len = (uint8_t)(name_len + 2);

    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0,
                                  null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv_data), (uint8_t *)adv_data);
    gap_scan_response_set_data(scan_rsp_len, scan_rsp);
    gap_advertisements_enable(1);

    printf("[ble] advertising started (name=%s)\r\n", g_node_name);
}

// ============================================================
// Public API
// ============================================================

bool ble_init(void) {
    printf("[ble] Initializing BTstack on CYW43...\r\n");

      // cyw43_arch_init() also calls btstack_cyw43_init() when the target links
      // pico_btstack_cyw43 (that library adds CYW43_ENABLE_BLUETOOTH=1 as a
      // compile definition, which enables the init branch in cyw43_arch_poll.c).
      // That call sets up btstack memory, the async-context run loop, and the
      // HCI transport — so we must not call those ourselves here.
    if (cyw43_arch_init() != 0) {
        printf("[ble] cyw43_arch_init FAILED\r\n");
        return false;
     }

    l2cap_init();
    sm_init();
    att_server_init(profile_data, att_read_cb, att_write_cb);

    g_hci_event_callback_reg.callback = &packet_handler;
    hci_add_event_handler(&g_hci_event_callback_reg);
    att_server_register_packet_handler(att_event_handler);

    g_connected = false;
    g_conn_handle = 0xFFFF;
    g_notif_enabled = false;

    int rc = hci_power_control(HCI_POWER_ON);
    printf("[ble] hci_power_control=%d, waiting for BTSTACK_EVENT_STATE...\r\n", rc);
    return rc == 0;
}

void ble_set_identity(const waxwing_device_identity_t *identity) {
    if (!identity) return;

      // Store node name for scan response advertising
    strncpy(g_node_name, identity->node_name, sizeof(g_node_name) - 1);
    g_node_name[sizeof(g_node_name) - 1] = '\0';

      // Build CBOR-encoded blob and store in characteristic value buffer
    build_identity_from_struct(identity);

    printf("[ble] device identity set (%zu bytes), name=%s\r\n",
            waxwing_identity_len, g_node_name);
}

void ble_set_identity_raw(const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > BLE_MAX_DATA_SIZE) return;

    memcpy(waxwing_identity_data, data, len);
    waxwing_identity_len = len;

    printf("[ble] device identity set raw (%zu bytes)\r\n", len);
}

void ble_start_advertising(void) {
    start_advertising_internal();
}

void ble_stop_advertising(void) {
    gap_advertisements_enable(0);
    printf("[ble] advertising stopped\r\n");
}

void ble_process(void) {
    cyw43_arch_poll();
}


bool ble_is_connected(void) {
    return g_connected;
}

uint16_t ble_get_conn_handle(void) {
    return g_conn_handle;
}

bool ble_send_file_response(const uint8_t *data, size_t len) {
    if (!g_notif_enabled || !g_connected || g_conn_handle == 0xFFFF) {
        return false;
     }
    if (len > BLE_MAX_DATA_SIZE) len = BLE_MAX_DATA_SIZE;

    memcpy(g_file_resp_buf, data, len);
    g_file_resp_len = len;

       // Request BTstack notification when it can send
    att_server_request_can_send_now_event(g_conn_handle);

    return true;
}

void ble_set_led(bool on) {
       // Pico W LED is on CYW43 chip (GPIO pin 0 by default for LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

void ble_set_on_connect(ble_on_connect_cb cb) {
    g_on_connect_cb = cb;
}

void ble_set_on_disconnect(ble_on_disconnect_cb cb) {
    g_on_disconnect_cb = cb;
}

void ble_set_on_write(ble_on_write_cb cb) {
    g_on_write_cb = cb;
}
