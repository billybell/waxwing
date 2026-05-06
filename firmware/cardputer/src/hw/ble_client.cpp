// CardPuter BLE GATT-client (Central role) for mesh-mode peer sync.
// NimBLE-Arduino implementation. NimBLE callbacks run on the host
// task; we enqueue events here and let main-task callers drain them
// via ble_client_process() so the user-supplied handlers (and the
// SD I/O they trigger) never run on the host stack.

#include "ble_client.h"
#include "ble.h"

#include <NimBLEDevice.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdio>
#include <cstring>


namespace {

// NimBLE address byte layout: native order is little-endian (LSB at
// index 0). The Pico W ble_client_peer_t carries big-endian bytes.
// Conversions happen at the boundaries.

const NimBLEUUID kSvcUUID(WAXWING_SERVICE_UUID);
const NimBLEUUID kCmdUUID(WAXWING_CHAR_FILE_CMD);
const NimBLEUUID kPeerCmdUUID(WAXWING_CHAR_PEER_CMD);
const NimBLEUUID kRspUUID(WAXWING_CHAR_FILE_RSP);

enum class EvtType : uint8_t {
    PeerSeen,
    Connected,
    Disconnected,
    Response,
};

struct Evt {
    EvtType  type;
    // PeerSeen:
    uint8_t  bd_addr[6];   // big-endian
    uint8_t  bd_addr_type;
    uint8_t  manifest_version;
    // Response:
    uint16_t resp_len;
    uint8_t  resp[256];
};

constexpr size_t kEvtQueueDepth = 8;
QueueHandle_t g_evt_q = nullptr;

NimBLEClient*               g_client        = nullptr;
NimBLERemoteCharacteristic* g_char_file_cmd = nullptr;
NimBLERemoteCharacteristic* g_char_peer_cmd = nullptr;
NimBLERemoteCharacteristic* g_char_rsp      = nullptr;

ble_client_on_peer_seen_cb     g_cb_peer_seen    = nullptr;
ble_client_on_connected_cb     g_cb_connected    = nullptr;
ble_client_on_response_cb      g_cb_response     = nullptr;
ble_client_on_disconnected_cb  g_cb_disconnected = nullptr;

bool g_scanning   = false;
bool g_connected  = false;

void post(const Evt &e) {
    if (g_evt_q) xQueueSend(g_evt_q, &e, 0);
}

void reverse6(const uint8_t *in, uint8_t *out) {
    for (int i = 0; i < 6; ++i) out[i] = in[5 - i];
}

class ScanCallbacks final : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice *dev) override {
        // Two paths to "this is a Waxwing peer":
        //   (a) AD types 0x06/0x07 — complete/incomplete services list.
        //       NimBLE parses these into the service-UUIDs vector and
        //       isAdvertisingService() reflects them.
        //   (b) AD type 0x21 — service-data 128-bit UUID. NimBLE does
        //       NOT merge these into the services-UUIDs vector, so
        //       isAdvertisingService() misses peers (like the Pico W)
        //       whose primary AD only carries service-data.
        // Check both. The first byte of matching service-data is the
        // 1-byte manifest-version hint.
        bool    matches          = dev->isAdvertisingService(kSvcUUID);
        uint8_t manifest_version = 0;
        if (dev->haveServiceData()) {
            const int n = dev->getServiceDataCount();
            for (int i = 0; i < n; ++i) {
                if (dev->getServiceDataUUID(i) == kSvcUUID) {
                    matches = true;
                    std::string sd = dev->getServiceData(i);
                    if (!sd.empty()) {
                        manifest_version = static_cast<uint8_t>(sd[0]);
                    }
                    break;
                }
            }
        }
        if (!matches) return;

        Evt e{};
        e.type             = EvtType::PeerSeen;
        e.manifest_version = manifest_version;

        // NimBLEAddress::getVal() returns LSB-first 6 bytes (2.x).
        const uint8_t *native = dev->getAddress().getVal();
        reverse6(native, e.bd_addr);
        e.bd_addr_type = dev->getAddressType();

        post(e);
    }
};

class ClientCallbacks final : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient * /*c*/) override {}
    void onDisconnect(NimBLEClient * /*c*/, int /*reason*/) override {
        Evt e{};
        e.type = EvtType::Disconnected;
        post(e);
    }
};

ScanCallbacks   g_scan_cbs;
ClientCallbacks g_client_cbs;

void notify_cb(NimBLERemoteCharacteristic * /*chr*/, uint8_t *data,
               size_t len, bool /*isNotify*/) {
    Evt e{};
    e.type = EvtType::Response;
    if (len > sizeof(e.resp)) len = sizeof(e.resp);
    e.resp_len = static_cast<uint16_t>(len);
    std::memcpy(e.resp, data, len);
    post(e);
}

}  // namespace

bool ble_client_init(void) {
    g_evt_q = xQueueCreate(kEvtQueueDepth, sizeof(Evt));
    if (!g_evt_q) return false;

    auto *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_scan_cbs, /*want_dups=*/true);
    // Passive scan: we only need the primary AD (which carries the
    // service-data with the manifest-version byte and the service
    // UUID). Active scan (where we TX scan-request packets to elicit
    // scan responses) puts more pressure on the controller and isn't
    // needed for Waxwing peer detection.
    scan->setActiveScan(false);
    scan->setInterval(48);  // 30 ms (units of 0.625 ms)
    scan->setWindow(48);
    return true;
}

void ble_client_start_scan(void) {
    if (g_scanning) return;
    // 2.x signature: start(duration_ms, is_continue, restart). 0 = continuous.
    NimBLEDevice::getScan()->start(0, false, true);
    g_scanning = true;
    std::printf("[ble_client] scanning\r\n");
}

void ble_client_stop_scan(void) {
    if (!g_scanning) return;
    NimBLEDevice::getScan()->stop();
    g_scanning = false;
    std::printf("[ble_client] scan stopped\r\n");
}

bool ble_client_connect(const uint8_t bd_addr[6], uint8_t bd_addr_type) {
    if (g_connected) return false;
    if (g_scanning) ble_client_stop_scan();

    // Force peripheral advertising off. The ESP32 BT controller can
    // return BLE_HS_ECONTROLLER (status=13) when central connect is
    // initiated while the peripheral is still advertising — even
    // though NimBLE supports the role combo on paper. mesh_state
    // should have stopped advertising when entering SCANNING, but
    // race windows + state-flag drift make this defensive call
    // worth the cost.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv) adv->stop();

    // Wait for the controller to actually finish the disc-cancel.
    // NimBLEScan::stop() sends the cancel and returns immediately,
    // but the controller may still be processing — initiating a
    // connect during that window also returns status=13.
    NimBLEScan *scan = NimBLEDevice::getScan();
    for (int spin = 0; scan->isScanning() && spin < 50; ++spin) {
        delay(10);
    }

    // Recreate the client per connect. NimBLE-Arduino 1.4.x can leave
    // a NimBLEClient in a state after a failed connect that makes the
    // next attempt also fail. Cheap to delete + create.
    if (g_client) {
        NimBLEDevice::deleteClient(g_client);
        g_client = nullptr;
    }
    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(&g_client_cbs, false);

    uint8_t native[6];
    reverse6(bd_addr, native);

    std::printf("[ble_client] connecting to %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                bd_addr[0], bd_addr[1], bd_addr[2],
                bd_addr[3], bd_addr[4], bd_addr[5]);

    // Look the peer up in NimBLE's own scan-results store and pass
    // the resulting NimBLEAdvertisedDevice* into connect(). The
    // address-based connect(NimBLEAddress, ...) overload was
    // returning BLE_HS_ECONTROLLER (status=13) on the ESP32-S3 BT
    // controller — the dev-pointer overload uses NimBLE's tracked
    // address+type+phy info, which the controller accepts.
    NimBLEScanResults results = scan->getResults();
    NimBLEAdvertisedDevice *found = nullptr;
    for (int i = 0, n = results.getCount(); i < n; ++i) {
        const NimBLEAdvertisedDevice *cand = results.getDevice(i);
        if (cand && std::memcmp(cand->getAddress().getVal(), native, 6) == 0) {
            found = const_cast<NimBLEAdvertisedDevice *>(cand);
            break;
        }
    }
    if (!found) {
        std::printf("[ble_client] peer not in scan results\r\n");
        Evt e{}; e.type = EvtType::Disconnected;
        post(e);
        return false;
    }
    if (!g_client->connect(found, false)) {
        std::printf("[ble_client] connect failed\r\n");
        Evt e{}; e.type = EvtType::Disconnected;
        post(e);
        return false;
    }

    NimBLERemoteService *svc = g_client->getService(kSvcUUID);
    if (!svc) {
        std::printf("[ble_client] service not found\r\n");
        g_client->disconnect();
        return false;
    }
    g_char_file_cmd = svc->getCharacteristic(kCmdUUID);
    g_char_peer_cmd = svc->getCharacteristic(kPeerCmdUUID);   // optional
    g_char_rsp      = svc->getCharacteristic(kRspUUID);
    if (!g_char_file_cmd || !g_char_rsp) {
        std::printf("[ble_client] required chars not found (file_cmd=%p rsp=%p)\r\n",
                    g_char_file_cmd, g_char_rsp);
        g_client->disconnect();
        return false;
    }
    std::printf("[ble_client] discovered chars: file_cmd=1 peer_cmd=%d resp=1\r\n",
                g_char_peer_cmd ? 1 : 0);
    if (!g_char_rsp->subscribe(true, notify_cb)) {
        std::printf("[ble_client] subscribe failed\r\n");
        g_client->disconnect();
        return false;
    }

    g_connected = true;
    Evt e{}; e.type = EvtType::Connected;
    post(e);
    return true;
}

bool ble_client_send_command(const uint8_t *data, size_t len) {
    if (!g_connected) return false;
    // Prefer the peer characteristic (M4 stage 6) when the responder
    // exposes it; fall back to file_cmd for pre-M4 firmware.
    NimBLERemoteCharacteristic *target =
        g_char_peer_cmd ? g_char_peer_cmd : g_char_file_cmd;
    if (!target) return false;
    return target->writeValue(data, len, /*response=*/false);
}

bool ble_client_uses_peer_characteristic(void) {
    return g_char_peer_cmd != nullptr;
}

void ble_client_disconnect(void) {
    if (g_client && g_connected) {
        g_client->disconnect();
        // onDisconnect will fire and post the event.
    }
}

void ble_client_process(void) {
    if (!g_evt_q) return;
    Evt e;
    while (xQueueReceive(g_evt_q, &e, 0) == pdTRUE) {
        switch (e.type) {
            case EvtType::PeerSeen: {
                if (g_cb_peer_seen) {
                    ble_client_peer_t peer{};
                    std::memcpy(peer.bd_addr, e.bd_addr, 6);
                    peer.bd_addr_type     = e.bd_addr_type;
                    peer.manifest_version = e.manifest_version;
                    // BD-address stand-in for tpk_prefix until the
                    // TPK is folded into the advert.
                    std::memcpy(peer.tpk_prefix, e.bd_addr, 6);
                    g_cb_peer_seen(&peer);
                }
                break;
            }
            case EvtType::Connected:
                if (g_cb_connected) g_cb_connected();
                break;
            case EvtType::Disconnected:
                g_connected     = false;
                g_char_file_cmd = nullptr;
                g_char_peer_cmd = nullptr;
                g_char_rsp      = nullptr;
                if (g_cb_disconnected) g_cb_disconnected();
                break;
            case EvtType::Response:
                if (g_cb_response) g_cb_response(e.resp, e.resp_len);
                break;
        }
    }
}

void ble_client_set_on_peer_seen(ble_client_on_peer_seen_cb cb)       { g_cb_peer_seen    = cb; }
void ble_client_set_on_connected(ble_client_on_connected_cb cb)       { g_cb_connected    = cb; }
void ble_client_set_on_response(ble_client_on_response_cb cb)         { g_cb_response     = cb; }
void ble_client_set_on_disconnected(ble_client_on_disconnected_cb cb) { g_cb_disconnected = cb; }
