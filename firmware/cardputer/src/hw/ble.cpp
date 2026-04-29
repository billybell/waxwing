// CardPuter NimBLE port of the Waxwing GATT server.
//
// Phase 1: GATT bring-up only. The Identity characteristic returns
// whatever ble_set_identity_raw() last cached. File Command writes
// are forwarded to the registered callback but no command-dispatch
// path is wired yet — that lands when the filestore HAL is in.

#include "ble.h"

#include <NimBLEDevice.h>

#include <cstring>

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
NimBLECharacteristic* g_char_file_rsp   = nullptr;
NimBLEAdvertising*    g_advertising     = nullptr;

uint16_t g_conn_handle = 0xFFFF;
bool     g_connected   = false;
bool     g_advertising_active = false;

ble_on_connect_cb    g_cb_connect    = nullptr;
ble_on_disconnect_cb g_cb_disconnect = nullptr;
ble_on_write_cb      g_cb_write      = nullptr;

class ServerCallbacks final : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* /*server*/, ble_gap_conn_desc* desc) override {
        g_conn_handle = desc->conn_handle;
        g_connected   = true;
        g_advertising_active = false;
        if (g_cb_connect) {
            g_cb_connect(g_conn_handle);
        }
    }

    void onDisconnect(NimBLEServer* /*server*/, ble_gap_conn_desc* desc) override {
        const uint16_t handle = desc->conn_handle;
        g_connected   = false;
        g_conn_handle = 0xFFFF;
        if (g_cb_disconnect) {
            g_cb_disconnect(handle);
        }
    }
};

class FileCommandCallbacks final : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* chr) override {
        const std::string value = chr->getValue();
        if (g_cb_write) {
            g_cb_write(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        }
    }
};

ServerCallbacks       g_server_cbs;
FileCommandCallbacks  g_file_cmd_cbs;

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
    NimBLEDevice::init("Waxwing");
    NimBLEDevice::setMTU(247);

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

    g_char_file_rsp = g_service->createCharacteristic(
        WAXWING_CHAR_FILE_RSP,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    g_service->start();

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
    // NimBLE runs its own host task; nothing to pump here. Kept for
    // API parity with the Pico W port.
}

bool     ble_is_connected(void)   { return g_connected; }
uint16_t ble_get_conn_handle(void) { return g_conn_handle; }

uint16_t ble_get_mtu(void) {
    if (!g_connected || !g_server) return 23;
    return g_server->getPeerMTU(g_conn_handle);
}

bool ble_send_file_response(const uint8_t* data, size_t len) {
    if (!g_connected || !g_char_file_rsp) return false;
    g_char_file_rsp->setValue(data, len);
    g_char_file_rsp->notify();
    return true;
}

void ble_set_on_connect(ble_on_connect_cb cb)       { g_cb_connect = cb; }
void ble_set_on_disconnect(ble_on_disconnect_cb cb) { g_cb_disconnect = cb; }
void ble_set_on_write(ble_on_write_cb cb)           { g_cb_write = cb; }
