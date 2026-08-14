// CardPuter NimBLE port of the Waxwing GATT server.
//
// Phase 1: GATT bring-up only. The Identity characteristic returns
// whatever ble_set_identity_raw() last cached. File Command writes
// are forwarded to the registered callback but no command-dispatch
// path is wired yet — that lands when the filestore HAL is in.

#include "ble.h"

#include <NimBLEDevice.h>
#include <stdio.h>
#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {

constexpr uint16_t kServiceDataCompanyId = 0xFFFF;
constexpr size_t   kIdentityMaxLen       = BLE_MAX_DATA_SIZE;

uint8_t g_identity[kIdentityMaxLen];
size_t  g_identity_len = 0;
uint8_t g_manifest_version = 0;
char    g_node_name[16] = "Waxwing";

NimBLEServer*         g_server          = nullptr;
NimBLEService*        g_service         = nullptr;
NimBLECharacteristic* g_char_identity   = nullptr;
NimBLECharacteristic* g_char_file_cmd   = nullptr;
NimBLECharacteristic* g_char_peer_cmd   = nullptr;
NimBLECharacteristic* g_char_file_rsp   = nullptr;
NimBLEAdvertising*    g_advertising     = nullptr;

uint16_t g_conn_handle = 0xFFFF;
bool     g_connected   = false;
bool     g_advertising_active = false;

enum class BleEvtType : uint8_t {
    Connected,
    Disconnected,
};

struct BleEvt {
    BleEvtType type;
    uint16_t   handle;
};

constexpr size_t kBleEvtQueueDepth = 4;
QueueHandle_t g_ble_evt_q = nullptr;

ble_on_connect_cb    g_cb_connect    = nullptr;
ble_on_disconnect_cb g_cb_disconnect = nullptr;
ble_on_write_cb      g_cb_write      = nullptr;
ble_on_write_cb      g_cb_peer_write = nullptr;

class ServerCallbacks final : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& info) override {
        g_conn_handle = info.getConnHandle();
        g_connected   = true;
        g_advertising_active = false;
        
        BleEvt e{BleEvtType::Connected, g_conn_handle};
        if (g_ble_evt_q) xQueueSend(g_ble_evt_q, &e, 0);
    }

    void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& info,
                      int /*reason*/) override {
        const uint16_t handle = info.getConnHandle();
        g_connected   = false;
        g_conn_handle = 0xFFFF;
        
        BleEvt e{BleEvtType::Disconnected, handle};
        if (g_ble_evt_q) xQueueSend(g_ble_evt_q, &e, 0);
    }
};

class FileCommandCallbacks final : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* chr,
                 NimBLEConnInfo& /*info*/) override {
        const std::string value = chr->getValue();
        if (g_cb_write) {
            g_cb_write(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        }
    }
};

class PeerCommandCallbacks final : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* chr,
                 NimBLEConnInfo& /*info*/) override {
        const std::string value = chr->getValue();
        if (g_cb_peer_write) {
            g_cb_peer_write(reinterpret_cast<const uint8_t*>(value.data()),
                             value.size());
        }
    }
};

ServerCallbacks       g_server_cbs;
FileCommandCallbacks  g_file_cmd_cbs;
PeerCommandCallbacks  g_peer_cmd_cbs;

void publish_advertising_payload() {
    if (!g_advertising) return;

    // BLE 4 advertising and scan-response packets are each capped at 31
    // bytes. With a 128-bit (16-byte) service UUID, putting both the
    // complete-services list AND service-data in the primary AD pushes
    // it over the limit (3 + 18 + 19 = 40). Split:
    //   primary AD     = flags(3) + service-data(19) = 22 bytes
    //   scan response  = name(9)  + complete-services(18) = 27 bytes
    // The service UUID is still discoverable because service-data
    // names it; the explicit list in the scan response is for scanners
    // that filter by complete-services AD type.
    NimBLEAdvertisementData adv;
    adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    std::string service_data;
    service_data.push_back(static_cast<char>(g_manifest_version));
    adv.setServiceData(NimBLEUUID(WAXWING_SERVICE_UUID), service_data);
    g_advertising->setAdvertisementData(adv);

    NimBLEAdvertisementData scan_resp;
    scan_resp.setName(g_node_name);
    scan_resp.setCompleteServices(NimBLEUUID(WAXWING_SERVICE_UUID));
    g_advertising->setScanResponseData(scan_resp);
}

}  // namespace

bool ble_init(void) {
    g_ble_evt_q = xQueueCreate(kBleEvtQueueDepth, sizeof(BleEvt));

    NimBLEDevice::init("Waxwing");
    NimBLEDevice::setMTU(247);

    // Stick with NimBLE's default own address type (random
    // non-resolvable). Forcing BLE_OWN_ADDR_PUBLIC was causing
    // status=13 on central connect: NimBLEDevice::init does not
    // automatically load the chip's factory MAC into the host
    // stack's identity, so the LE Create Connection HCI command
    // referenced an address the controller never had configured
    // and was rejected. Random address is fine for our use —
    // peer identity is established via the Ed25519 TPK at the
    // protocol layer, not the BLE address.
    // No pairing / no MITM / no bonding. Waxwing peer-sync is
    // unauthenticated at the BLE layer (auth lives in the protocol).
    // Default security can attempt SMP exchange mid-connect and break
    // the central handshake.
    NimBLEDevice::setSecurityAuth(false, false, false);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_server_cbs);

    g_service = g_server->createService(WAXWING_SERVICE_UUID);

    g_char_identity = g_service->createCharacteristic(
        WAXWING_CHAR_IDENTITY,
        NIMBLE_PROPERTY::READ);

    g_char_file_cmd = g_service->createCharacteristic(
        WAXWING_CHAR_FILE_CMD,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    g_char_file_cmd->setCallbacks(&g_file_cmd_cbs);

    // M4 stage 6: peer command characteristic, gated by the encounter
    // handshake before any other command runs. Same Write / Write
    // Without Response properties as file_cmd.
    g_char_peer_cmd = g_service->createCharacteristic(
        WAXWING_CHAR_PEER_CMD,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    g_char_peer_cmd->setCallbacks(&g_peer_cmd_cbs);

    g_char_file_rsp = g_service->createCharacteristic(
        WAXWING_CHAR_FILE_RSP,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    g_advertising = NimBLEDevice::getAdvertising();
    publish_advertising_payload();

    return true;
}

void ble_set_identity_raw(const uint8_t* data, size_t len) {
    if (len > kIdentityMaxLen) len = kIdentityMaxLen;
    std::memcpy(g_identity, data, len);
    g_identity_len = len;
    if (g_char_identity) {
        g_char_identity->setValue(g_identity, g_identity_len);
    }
}

void ble_set_manifest_version(uint8_t version) {
    if (version == g_manifest_version) return;
    g_manifest_version = version;
    publish_advertising_payload();
}

void ble_set_node_name(const char *name) {
    if (!name || !*name) return;
    std::strncpy(g_node_name, name, sizeof(g_node_name) - 1);
    g_node_name[sizeof(g_node_name) - 1] = '\0';
    publish_advertising_payload();
}

void ble_start_advertising(void) {
    if (!g_advertising || g_advertising_active) return;
    g_advertising->start();
    g_advertising_active = true;
}

void ble_stop_advertising(void) {
    if (!g_advertising || !g_advertising_active) return;
    g_advertising->stop();
    g_advertising_active = false;
}

void ble_process(void) {
    if (!g_ble_evt_q) return;
    BleEvt e;
    while (xQueueReceive(g_ble_evt_q, &e, 0) == pdTRUE) {
        switch (e.type) {
            case BleEvtType::Connected:
                if (g_cb_connect) g_cb_connect(e.handle);
                break;
            case BleEvtType::Disconnected:
                if (g_cb_disconnect) g_cb_disconnect(e.handle);
                break;
        }
    }
}

bool     ble_is_connected(void)   { return g_connected; }
uint16_t ble_get_conn_handle(void) { return g_conn_handle; }

uint16_t ble_get_mtu(void) {
    if (!g_connected || !g_server) return 23;
    return g_server->getPeerMTU(g_conn_handle);
}

bool ble_send_file_response(const uint8_t* data, size_t len) {
    if (!g_connected || !g_char_file_rsp) {
        printf("[BLE] send_file_response not connected or no char\r\n");
        return false;
    }
    printf("[BLE] send_file_response len=%zu at %lu\n", len, (unsigned long)millis());
    g_char_file_rsp->setValue(data, len);
    bool ret = g_char_file_rsp->notify();
    printf("[BLE] notify returned %d\n", ret);
    return ret;
}

void ble_set_on_connect(ble_on_connect_cb cb)       { g_cb_connect = cb; }
void ble_set_on_disconnect(ble_on_disconnect_cb cb) { g_cb_disconnect = cb; }
void ble_set_on_write(ble_on_write_cb cb)           { g_cb_write = cb; }
void ble_set_on_peer_write(ble_on_write_cb cb)      { g_cb_peer_write = cb; }
