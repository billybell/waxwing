# waxwing/messages.py
# CBOR message builders for Waxwing Mesh (Pico W / MicroPython).
#
# Phase 1 implements only the Device Identity characteristic payload.
# The schema mirrors PROTOCOL.md §3 (Device Identity Characteristic).
#
# Wire format (CBOR map):
#   {
#     "protocol":       str,        # always "waxwing-mesh"
#     "v":              int,        # protocol version
#     "tpk":            bytes,      # raw 32-byte Transport Public Key (CBOR byte string)
#     "caps":           int,        # capability flags bitmask
#     "firmware":       str,        # firmware name, e.g. "pico-w"
#     "firmware_ver":   str,        # semver, e.g. "0.1.0"
#     "attended":       bool,       # false for unattended nodes
#     "unattended_mode":str | null, # "relay" | "publisher" | "archive" | null
#     "manifest_count": int,        # number of items available (0 in Phase 1)
#     "timestamp":      int,        # seconds since epoch (0 if no RTC)
#   }

import time
from . import cbor
from .constants import (
    PROTOCOL_NAME,
    PROTOCOL_VERSION,
    FIRMWARE_NAME,
    FIRMWARE_VERSION,
    PICO_W_CAPS,
    UNATTENDED_RELAY,
)
def build_device_identity(identity, manifest_count=0,
                          attended=False,
                          unattended_mode=UNATTENDED_RELAY,
                          caps=PICO_W_CAPS):
    """
    Build and return the CBOR-encoded Device Identity payload.

    Parameters
    ----------
    identity : dict
        The dict returned by identity.load_or_generate().
    manifest_count : int
        Number of content items the node has available.
    attended : bool
        True only when a companion app is currently paired and connected.
    unattended_mode : str or None
        One of the UNATTENDED_* constants, or None if attended.
    caps : int
        Capability flags bitmask.

    Returns
    -------
    bytes
        CBOR-encoded device identity payload.
    """
    # tpk is sent as a raw 32-byte CBOR byte string (major type 2),
    # matching the PC-side tools/waxwing/messages.py convention.
    tpk_bytes = identity["pub"]

    # Attempt to read wall-clock seconds.  RTC may not be set so we fall
    # back to monotonic milliseconds / 1000 (gives seconds-since-boot).
    try:
        ts = int(time.time())
    except Exception:
        ts = time.ticks_ms() // 1000

    payload = {
        "protocol":        PROTOCOL_NAME,
        "v":               PROTOCOL_VERSION,
        "tpk":             tpk_bytes,
        "caps":            caps,
        "firmware":        FIRMWARE_NAME,
        "firmware_ver":    FIRMWARE_VERSION,
        "attended":        attended,
        "unattended_mode": unattended_mode if not attended else None,
        "manifest_count":  manifest_count,
        "timestamp":       ts,
    }

    return cbor.dumps(payload)


def parse_device_identity(data):
    """
    Decode a CBOR Device Identity payload.

    Returns the decoded dict, or raises ValueError on parse failure.
    """
    try:
        obj = cbor.loads(data)
    except Exception as e:
        raise ValueError("messages: failed to parse device identity: {}".format(e))

    if not isinstance(obj, dict):
        raise ValueError("messages: device identity is not a CBOR map")

    required = ("protocol", "v", "tpk", "caps", "firmware", "attended")
    for key in required:
        if key not in obj:
            raise ValueError("messages: missing required key '{}'".format(key))

    return obj
