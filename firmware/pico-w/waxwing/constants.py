# waxwing/constants.py
# MicroPython-compatible constants for Waxwing Mesh firmware (Pico W)
# Mirrors tools/waxwing/constants.py but avoids PC-only dependencies.

# ---------------------------------------------------------------------------
# BLE Service UUID (128-bit, little-endian bytes for bluetooth module)
# CE575800-494E-4700-8000-00805F9B34FB
# ---------------------------------------------------------------------------
SERVICE_UUID = "CE575800-494E-4700-8000-00805F9B34FB"

# Characteristic UUIDs (Phase 1: identity + manifest stub)
CHAR_DEVICE_IDENTITY    = "CE575801-494E-4700-8000-00805F9B34FB"
CHAR_MANIFEST_META      = "CE575802-494E-4700-8000-00805F9B34FB"
CHAR_MANIFEST_CHUNK     = "CE575803-494E-4700-8000-00805F9B34FB"
CHAR_TRANSFER_REQUEST   = "CE575804-494E-4700-8000-00805F9B34FB"
CHAR_TRANSFER_DATA      = "CE575805-494E-4700-8000-00805F9B34FB"
CHAR_TRANSFER_ACK       = "CE575806-494E-4700-8000-00805F9B34FB"
CHAR_RATING_SUBMIT      = "CE575807-494E-4700-8000-00805F9B34FB"
CHAR_REP_EXCHANGE       = "CE575808-494E-4700-8000-00805F9B34FB"
CHAR_WIFI_NEGOTIATE     = "CE575809-494E-4700-8000-00805F9B34FB"
CHAR_PAIRING_AUTH       = "CE57580A-494E-4700-8000-00805F9B34FB"
CHAR_SYNC_ATTEST        = "CE57580B-494E-4700-8000-00805F9B34FB"
CHAR_ENCOUNTER_LEDGER   = "CE57580C-494E-4700-8000-00805F9B34FB"

# ---------------------------------------------------------------------------
# Device capability flags (bitmask in device identity)
# ---------------------------------------------------------------------------
CAP_BLE_TRANSFER    = 0x01  # BLE chunked file transfer
CAP_WIFI_AP         = 0x02  # Can create WiFi AP
CAP_WIFI_CLIENT     = 0x04  # Can join WiFi network
CAP_WIFI_DIRECT     = 0x08  # WiFi Direct / P2P
CAP_GPS             = 0x10  # Has GPS module
CAP_STORAGE_SD      = 0x20  # External SD card present
CAP_ATTENDED        = 0x40  # Attended node (has companion app paired)
CAP_UNATTENDED      = 0x80  # Unattended relay/publisher/archive mode

# Pico W Phase-1 capabilities: BLE transfer only, unattended
PICO_W_CAPS = CAP_BLE_TRANSFER | CAP_UNATTENDED

# ---------------------------------------------------------------------------
# Unattended node modes
# ---------------------------------------------------------------------------
UNATTENDED_RELAY     = "relay"
UNATTENDED_PUBLISHER = "publisher"
UNATTENDED_ARCHIVE   = "archive"

# ---------------------------------------------------------------------------
# Transfer opcodes (CHAR_TRANSFER_REQUEST / CHAR_TRANSFER_ACK)
# ---------------------------------------------------------------------------
OP_TRANSFER_REQUEST  = 0x01
OP_TRANSFER_ACCEPT   = 0x02
OP_TRANSFER_REJECT   = 0x03
OP_TRANSFER_COMPLETE = 0x04
OP_TRANSFER_ABORT    = 0x05
OP_CHUNK_ACK         = 0x10
OP_CHUNK_NACK        = 0x11

# ---------------------------------------------------------------------------
# Review / rating actions
# ---------------------------------------------------------------------------
ACTION_RECOMMEND_STRONG = 2
ACTION_RECOMMEND        = 1
ACTION_PASS_ALONG       = 0
ACTION_HOLD             = -1
ACTION_REJECT           = -2

# ---------------------------------------------------------------------------
# WiFi negotiation modes
# ---------------------------------------------------------------------------
WIFI_MODE_PERIPHERAL_AP  = 0x01
WIFI_MODE_CENTRAL_AP     = 0x02
WIFI_MODE_MULTIPEER      = 0x03
WIFI_MODE_LOCAL_NETWORK  = 0x04

# ---------------------------------------------------------------------------
# Protocol version
# ---------------------------------------------------------------------------
PROTOCOL_NAME    = "waxwing-mesh"
PROTOCOL_VERSION = 1

# ---------------------------------------------------------------------------
# Firmware identity
# ---------------------------------------------------------------------------
FIRMWARE_NAME    = "pico-w"
FIRMWARE_VERSION = "0.1.0"

# Node name prefix used in BLE advertisement scan response
# Full name will be "WX:" + 8 hex chars of tpk fingerprint
NODE_NAME_PREFIX = "WX:"

# Identity file path (persisted on flash)
IDENTITY_FILE = "/waxwing_identity.bin"

# ---------------------------------------------------------------------------
# BLE advertisement / GATT sizing
# ---------------------------------------------------------------------------
# Max bytes we pre-allocate for the Device Identity characteristic value.
# CBOR-encoded device identity is well under this for Phase 1.
DEVICE_IDENTITY_MAX_BYTES = 256

# Sliding window size for chunked BLE transfers
TRANSFER_WINDOW_SIZE = 4

# Default MTU negotiation target
DEFAULT_MTU = 256
