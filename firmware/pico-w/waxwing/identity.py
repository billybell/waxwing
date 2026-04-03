# waxwing/identity.py
# Transport Identity management for Waxwing Mesh (Pico W / MicroPython).
#
# Phase 1 — placeholder crypto only (os.urandom for private key,
# SHA-256 of private key as "public key").  This is NOT real Ed25519.
# Phase 2 will replace with a MicroPython Ed25519 implementation.
#
# Identity is persisted to IDENTITY_FILE on the Pico's internal flash
# so that the node retains the same Transport Public Key (tpk) across
# reboots.  The file format is a simple 64-byte binary record:
#
#   bytes  0-31 : private key  (32 bytes, random)
#   bytes 32-63 : public key   (32 bytes, SHA-256 of private key in Phase 1)
#
# The tpk fingerprint (first 4 bytes of public key, hex) is used to
# generate the BLE advertisement name "WX:AABBCCDD".

import os
import hashlib
from .constants import IDENTITY_FILE

_RECORD_SIZE = 64   # 32 priv + 32 pub


def _sha256(data):
    """Return SHA-256 digest bytes of data."""
    h = hashlib.sha256()
    h.update(data)
    return h.digest()


def _generate():
    """Generate a new (private_key, public_key) pair and return as bytes."""
    priv = os.urandom(32)
    # Phase 1 placeholder: pub = SHA-256(priv)
    # Phase 2: replace with real Ed25519 keypair derivation
    pub  = _sha256(priv)
    return priv, pub


def _save(priv, pub):
    """Persist identity to flash."""
    with open(IDENTITY_FILE, "wb") as f:
        f.write(priv)
        f.write(pub)


def _load():
    """Load identity from flash.  Returns (priv, pub) or raises OSError."""
    with open(IDENTITY_FILE, "rb") as f:
        data = f.read(_RECORD_SIZE)
    if len(data) != _RECORD_SIZE:
        raise ValueError("identity: corrupt file (expected {} bytes, got {})".format(
            _RECORD_SIZE, len(data)))
    return data[:32], data[32:]


def load_or_generate():
    """
    Load the persisted Transport Identity or generate + persist a new one.
    Returns a dict:
      {
        "priv":        bytes(32),   # private key (keep secret)
        "pub":         bytes(32),   # public key / tpk
        "tpk_hex":     str,         # 64-char lower-hex string of pub
        "fingerprint": str,         # 8-char lower-hex of pub[:4]  (for ad name)
        "node_name":   str,         # e.g. "WX:AABBCCDD"
      }
    """
    try:
        priv, pub = _load()
        # Sanity-check: regenerate pub from priv and compare
        expected_pub = _sha256(priv)
        if pub != expected_pub:
            raise ValueError("identity: public key mismatch — regenerating")
        print("[identity] Loaded existing Transport Identity")
    except (OSError, ValueError) as e:
        print("[identity] Generating new Transport Identity ({})".format(e))
        priv, pub = _generate()
        _save(priv, pub)
        print("[identity] Saved to {}".format(IDENTITY_FILE))

    tpk_hex     = _bytes_to_hex(pub)
    fingerprint = tpk_hex[:8]
    node_name   = "WX:" + fingerprint.upper()

    return {
        "priv":        priv,
        "pub":         pub,
        "tpk_hex":     tpk_hex,
        "fingerprint": fingerprint,
        "node_name":   node_name,
    }


def wipe():
    """
    Erase the persisted identity (for factory reset / testing).
    Next boot will generate a fresh keypair.
    """
    try:
        os.remove(IDENTITY_FILE)
        print("[identity] Identity wiped")
    except OSError:
        print("[identity] Nothing to wipe")


# ---------------------------------------------------------------------------
# Utility helpers
# ---------------------------------------------------------------------------

def _bytes_to_hex(b):
    """Convert bytes to lowercase hex string (MicroPython has no .hex() on bytes)."""
    return "".join("{:02x}".format(byte) for byte in b)


def tpk_to_base64url(pub_bytes):
    """
    Encode 32-byte public key as 43-char base64url string (no padding).
    Used in CBOR Device Identity payload ("tpk" field).
    """
    # Base64url alphabet
    _ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    result = []
    padding = 0
    acc = 0
    bits = 0
    for byte in pub_bytes:
        acc = (acc << 8) | byte
        bits += 8
        while bits >= 6:
            bits -= 6
            result.append(_ALPHABET[(acc >> bits) & 0x3F])
    if bits > 0:
        result.append(_ALPHABET[(acc << (6 - bits)) & 0x3F])
    return "".join(result)
