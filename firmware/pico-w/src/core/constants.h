#ifndef WAXWING_CONSTANTS_H
#define WAXWING_CONSTANTS_H

// BLE Service UUID (128-bit, little-endian bytes for bluetooth module)
// CE575800-494E-4700-8000-00805F9B34FB
#define SERVICE_UUID "CE575800-494E-4700-8000-00805F9B34FB"

// Characteristic UUIDs (Phase 1: identity + manifest stub)
#define CHAR_DEVICE_IDENTITY    "CE575801-494E-4700-8000-00805F9B34FB"
#define CHAR_MANIFEST_META      "CE575802-494E-4700-8000-00805F9B34FB"
#define CHAR_MANIFEST_CHUNK     "CE575803-494E-4700-8000-00805F9B34FB"
#define CHAR_TRANSFER_REQUEST   "CE575804-494E-4700-8000-00805F9B34FB"
#define CHAR_TRANSFER_DATA      "CE575805-494E-4700-8000-00805F9B34FB"
#define CHAR_TRANSFER_ACK       "CE575806-494E-4700-8000-00805F9B34FB"
#define CHAR_RATING_SUBMIT      "CE575807-494E-4700-8000-00805F9B34FB"
#define CHAR_REP_EXCHANGE       "CE575808-494E-4700-8000-00805F9B34FB"
#define CHAR_WIFI_NEGOTIATE     "CE575809-494E-4700-8000-00805F9B34FB"
#define CHAR_PAIRING_AUTH       "CE57580A-494E-4700-8000-00805F9B34FB"
#define CHAR_SYNC_ATTEST        "CE57580B-494E-4700-8000-00805F9B34FB"
#define CHAR_ENCOUNTER_LEDGER   "CE57580C-494E-4700-8000-00805F9B34FB"

// Phase 1.5: File management characteristics (companion → node file push/pull)
#define CHAR_FILE_COMMAND       "CE57580D-494E-4700-8000-00805F9B34FB"
#define CHAR_FILE_RESPONSE      "CE57580E-494E-4700-8000-00805F9B34FB"

// Device capability flags (bitmask in device identity)
#define CAP_BLE_TRANSFER    0x01  // BLE chunked file transfer
#define CAP_WIFI_AP         0x02  // Can create WiFi AP
#define CAP_WIFI_CLIENT     0x04  // Can join WiFi network
#define CAP_WIFI_DIRECT     0x08  // WiFi Direct / P2P
#define CAP_GPS             0x10  // Has GPS module
#define CAP_STORAGE_SD      0x20  // External SD card present
#define CAP_ATTENDED        0x40  // Attended node (has companion app paired)
#define CAP_UNATTENDED      0x80  // Unattended relay/publisher/archive mode

// Pico W Phase-1 capabilities: BLE transfer only, unattended
#define PICO_W_CAPS (CAP_BLE_TRANSFER | CAP_UNATTENDED)

// Unattended node modes
#define UNATTENDED_RELAY     "relay"
#define UNATTENDED_PUBLISHER "publisher"
#define UNATTENDED_ARCHIVE   "archive"

// Transfer opcodes (CHAR_TRANSFER_REQUEST / CHAR_TRANSFER_ACK)
#define OP_TRANSFER_REQUEST  0x01
#define OP_TRANSFER_ACCEPT   0x02
#define OP_TRANSFER_REJECT   0x03
#define OP_TRANSFER_COMPLETE 0x04
#define OP_TRANSFER_ABORT    0x05
#define OP_CHUNK_ACK         0x10
#define OP_CHUNK_NACK        0x11

// Review / rating actions
#define ACTION_RECOMMEND_STRONG 2
#define ACTION_RECOMMEND 1
#define ACTION_PASS_ALONG 0
#define ACTION_HOLD -1
#define ACTION_REJECT -2

// WiFi negotiation modes
#define WIFI_MODE_PERIPHERAL_AP  0x01
#define WIFI_MODE_CENTRAL_AP     0x02
#define WIFI_MODE_MULTIPEER     0x03
#define WIFI_MODE_LOCAL_NETWORK  0x04

// Protocol version
#define PROTOCOL_NAME    "waxwing-mesh"
#define PROTOCOL_VERSION 1

// Firmware identity
#define FIRMWARE_NAME    "pico-w"
#define FIRMWARE_VERSION "0.1.0"

// Node name prefix used in BLE advertisement scan response
// Full name will be "WX:" + 8 hex chars of tpk fingerprint
#define NODE_NAME_PREFIX "WX:"

// Identity file path (persisted on flash)
#define IDENTITY_FILE "/waxwing_identity.bin"

// BLE advertisement / GATT sizing
#define DEVICE_IDENTITY_MAX_BYTES 256
#define TRANSFER_WINDOW_SIZE 4
#define DEFAULT_MTU 256

// File storage limits
#define MAX_FILE_SIZE 2048          // single-shot text file limit (bytes)
#define MAX_CHUNKED_FILE_SIZE (512 * 1024)   // 512 KB hard cap per file
#define STORAGE_RESERVE (32 * 1024)  // keep 32 KB free for firmware / GC headroom
#define HASH_PREFIX_BYTES 8        // truncated SHA-256 prefix for manifest
#define LIST_PAGE_SIZE 4           // entries per paginated ls response

// Mesh-mode timing (Phase 3 peer sync; see PEER_SYNC_PLAN.md)
#define MESH_GRACE_MS               10000     // boot companion-pair window
#define MESH_DWELL_MIN_MS            1000     // alternation lower bound
#define MESH_DWELL_MAX_MS            5000     // alternation upper bound
#define MESH_DWELL_JITTER_PCT          20     // ±20 % on top of dwell draw
#define PEER_SUCCESS_BACKOFF_MS    600000     // 10 min after a clean sync
#define PEER_FAILED_BACKOFF_MS      30000     // 30 s after an error/disconnect
#define PEER_BACKOFF_JITTER_PCT        20     // ±20 % so clocks don't realign
#define PEER_TABLE_CAP                 32     // RAM-only LRU of seen peers
#define PEER_TPK_PREFIX_LEN             8     // bytes of TPK we use as a key

#endif // WAXWING_CONSTANTS_H