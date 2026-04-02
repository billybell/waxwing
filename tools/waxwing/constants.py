"""
Waxwing Mesh Protocol Constants
See /PROTOCOL.md for the full specification.
"""

# ---------------------------------------------------------------------------
# UUIDs
# All Waxwing UUIDs use the base suffix -494E-4700-8000-00805F9B34FB
# "WX" = 0x5758, "ING\0" encodes the mnemonic for "Waxwing"
# ---------------------------------------------------------------------------

SERVICE_UUID              = "CE575800-494E-4700-8000-00805F9B34FB"

CHAR_DEVICE_IDENTITY      = "CE575801-494E-4700-8000-00805F9B34FB"
CHAR_MANIFEST_CHUNK       = "CE575802-494E-4700-8000-00805F9B34FB"
CHAR_TRANSFER_REQUEST     = "CE575803-494E-4700-8000-00805F9B34FB"
CHAR_TRANSFER_DATA        = "CE575804-494E-4700-8000-00805F9B34FB"
CHAR_TRANSFER_CONTROL     = "CE575805-494E-4700-8000-00805F9B34FB"
CHAR_REPUTATION_EXCHANGE  = "CE575806-494E-4700-8000-00805F9B34FB"
CHAR_RATING_SUBMISSION    = "CE575807-494E-4700-8000-00805F9B34FB"
CHAR_WIFI_HANDOFF         = "CE575809-494E-4700-8000-00805F9B34FB"
CHAR_DEVICE_CONFIG        = "CE57580A-494E-4700-8000-00805F9B34FB"
CHAR_SYNC_ATTESTATION     = "CE57580B-494E-4700-8000-00805F9B34FB"
CHAR_ENCOUNTER_LEDGER     = "CE57580C-494E-4700-8000-00805F9B34FB"

# ---------------------------------------------------------------------------
# Protocol
# ---------------------------------------------------------------------------

PROTOCOL_VERSION = 1
PROTOCOL_NAME    = "waxwing-mesh"

# ---------------------------------------------------------------------------
# Capability flags  (caps bitmask in Device Identity)
# ---------------------------------------------------------------------------

CAP_WIFI_CLIENT   = 0x01   # Can connect to a WiFi AP
CAP_WIFI_AP       = 0x02   # Can create a soft AP
CAP_MULTIPEER     = 0x04   # iOS Multipeer Connectivity (AWDL)
CAP_LOCAL_NETWORK = 0x08   # Both devices on same infrastructure WiFi
CAP_UNATTENDED    = 0x10   # Node is deployed in unattended mode

def caps_to_strings(caps: int) -> list[str]:
    """Return a list of human-readable capability names for a caps bitmask."""
    result = []
    if caps & CAP_WIFI_CLIENT:   result.append("WiFi-Client")
    if caps & CAP_WIFI_AP:       result.append("WiFi-AP")
    if caps & CAP_MULTIPEER:     result.append("Multipeer")
    if caps & CAP_LOCAL_NETWORK: result.append("Local-Net")
    if caps & CAP_UNATTENDED:    result.append("Unattended")
    return result

# ---------------------------------------------------------------------------
# Transfer Request opcodes  (Central → Peripheral, CHAR_TRANSFER_REQUEST)
# ---------------------------------------------------------------------------

REQ_REQUEST       = 0x01   # Request a file by ID, optionally with resume offset
REQ_CANCEL        = 0x02   # Cancel an in-progress transfer
REQ_MANIFEST_READ = 0x03   # Set manifest read offset before reading CHAR_MANIFEST_CHUNK

# ---------------------------------------------------------------------------
# Transfer Control opcodes  (Central → Peripheral)
# ---------------------------------------------------------------------------

CTRL_ACK_CHUNK    = 0x10   # Acknowledge chunk at seq N
CTRL_NACK_CHUNK   = 0x11   # Request retransmit of chunk at seq N
CTRL_PAUSE        = 0x12   # Pause transmission (flow control)
CTRL_RESUME       = 0x13   # Resume transmission
CTRL_COMPLETE     = 0x14   # Transfer complete, checksum verified

# Transfer Control opcodes  (Peripheral → Central, via NOTIFY)
CTRL_TRANSFER_START = 0x20  # About to begin sending chunks
CTRL_TRANSFER_DONE  = 0x21  # All chunks sent
CTRL_ERROR          = 0x22  # Transfer error

# Transfer error codes
ERR_CHECKSUM_FAIL   = 0x01
ERR_STORAGE_FULL    = 0x02
ERR_FILE_NOT_FOUND  = 0x03
ERR_TRANSFER_ABORT  = 0x04

# ---------------------------------------------------------------------------
# Sync Attestation opcodes  (CHAR_SYNC_ATTESTATION)
# ---------------------------------------------------------------------------

ATTEST_PROPOSE  = 0x01   # Central initiates attestation exchange
ATTEST_ACCEPT   = 0x02   # Peripheral accepts and provides its signature
ATTEST_DECLINE  = 0x03   # Either party declines (feature disabled)
ATTEST_COMPLETE = 0x04   # Central provides its signature + full record

# ---------------------------------------------------------------------------
# Device Config opcodes  (CHAR_DEVICE_CONFIG — companion only)
# ---------------------------------------------------------------------------

CFG_AUTH_CHALLENGE_REQUEST = 0x01
CFG_AUTH_CHALLENGE         = 0x02
CFG_AUTH_RESPONSE          = 0x03
CFG_AUTH_OK                = 0x04
CFG_AUTH_FAIL              = 0x05
CFG_WIFI_CONNECTED         = 0x10

# ---------------------------------------------------------------------------
# Review actions  (used in ratings and Forwarding Declarations)
# ---------------------------------------------------------------------------

ACTION_RECOMMEND_STRONG = 2    # "Love it" — forward eagerly, gossip positive signal
ACTION_RECOMMEND        = 1    # "Like it" — forward at high priority
ACTION_PASS_ALONG       = 0    # "Pass along" — forward at low priority, no gossip
ACTION_HOLD             = -1   # "Not for me" — do not forward, no gossip
ACTION_REJECT           = -2   # "Spam / harmful" — do not forward, gossip negative signal

FORWARDING_ACTIONS = {ACTION_RECOMMEND_STRONG, ACTION_RECOMMEND, ACTION_PASS_ALONG}
GOSSIPED_ACTIONS   = {ACTION_RECOMMEND_STRONG, ACTION_RECOMMEND, ACTION_REJECT}

ACTION_LABELS = {
    ACTION_RECOMMEND_STRONG : "Recommend (strong)",
    ACTION_RECOMMEND        : "Recommend",
    ACTION_PASS_ALONG       : "Pass Along",
    ACTION_HOLD             : "Hold",
    ACTION_REJECT           : "Reject",
}

# ---------------------------------------------------------------------------
# Routing types
# ---------------------------------------------------------------------------

ROUTING_BROADCAST = "broadcast"
ROUTING_ADDRESSED = "addressed"

# ---------------------------------------------------------------------------
# Unattended node modes
# ---------------------------------------------------------------------------

UNATTENDED_RELAY     = "relay"
UNATTENDED_PUBLISHER = "publisher"
UNATTENDED_ARCHIVE   = "archive"

# ---------------------------------------------------------------------------
# Transfer parameters
# ---------------------------------------------------------------------------

DEFAULT_TTL          = 20      # Default hop count for new content
MAX_TTL              = 64      # Maximum permitted TTL
MIN_MTU              = 128     # Minimum acceptable MTU (bytes)
TARGET_MTU           = 512     # Requested MTU during negotiation
TRANSFER_HEADER_SIZE = 40      # file_id(32) + seq(4) + total(4) bytes
MANIFEST_HEADER_SIZE = 4       # total_length field (uint32 LE)
SLIDING_WINDOW       = 4       # Max unacknowledged chunks

# ---------------------------------------------------------------------------
# Reputation defaults
# ---------------------------------------------------------------------------

DEFAULT_BLOCKING_THRESHOLD    = -10
BASE_VOUCH_SCORE              = 3
MAX_VOUCH_ACCUMULATION        = 6
RELAY_INTEGRITY_DISTRUST      = -15
RELAY_TAMPER_PENALTY          = -20
VOUCH_DECAY_HALF_LIFE_DAYS    = 45
VOUCH_EXPIRE_DAYS             = 180
ENDORSEMENT_HOP_LIMIT         = 3

# ---------------------------------------------------------------------------
# WiFi upgrade
# ---------------------------------------------------------------------------

WIFI_UPGRADE_THRESHOLD_BYTES  = 1_000_000   # 1 MB: propose WiFi if pending > this

WIFI_MODE_PERIPHERAL_AP  = 0x01
WIFI_MODE_CENTRAL_AP     = 0x02
WIFI_MODE_MULTIPEER      = 0x03
WIFI_MODE_LOCAL_NETWORK  = 0x04

WIFI_PROPOSE  = 0x01
WIFI_ACCEPT   = 0x02
WIFI_DECLINE  = 0x03
