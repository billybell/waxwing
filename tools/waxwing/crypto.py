"""
Waxwing Mesh Cryptographic Utilities
Ed25519 key generation, signing, and verification.
BIP-39 mnemonic derivation for Content Identity keypairs.

Dependencies: cryptography, mnemonic (pip install cryptography mnemonic)
"""

import hashlib
import hmac
import os
import struct
from typing import Optional

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)
from cryptography.hazmat.primitives.serialization import (
    Encoding,
    PublicFormat,
    PrivateFormat,
    NoEncryption,
)
from cryptography.exceptions import InvalidSignature


# ---------------------------------------------------------------------------
# Transport Identity  (device-scoped, persisted on node)
# ---------------------------------------------------------------------------

def generate_transport_keypair() -> tuple[bytes, bytes]:
    """
    Generate a new Transport Identity keypair.
    Returns (private_key_bytes, public_key_bytes) — both 32 bytes.
    Store the private key in non-volatile storage; share only the public key.
    """
    private_key = Ed25519PrivateKey.generate()
    private_bytes = private_key.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
    public_bytes  = private_key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    return private_bytes, public_bytes


def load_transport_keypair(private_bytes: bytes) -> tuple[Ed25519PrivateKey, Ed25519PublicKey]:
    """Load a Transport Identity keypair from stored private key bytes."""
    private_key = Ed25519PrivateKey.from_private_bytes(private_bytes)
    return private_key, private_key.public_key()


def sign(private_key_bytes: bytes, payload: bytes) -> bytes:
    """Sign payload with an Ed25519 private key. Returns 64-byte signature."""
    private_key = Ed25519PrivateKey.from_private_bytes(private_key_bytes)
    return private_key.sign(payload)


def verify(public_key_bytes: bytes, payload: bytes, signature: bytes) -> bool:
    """
    Verify an Ed25519 signature. Returns True if valid, False if invalid.
    Never raises — invalid signatures return False.
    """
    try:
        public_key = Ed25519PublicKey.from_public_bytes(public_key_bytes)
        public_key.verify(signature, payload)
        return True
    except (InvalidSignature, ValueError):
        return False


# ---------------------------------------------------------------------------
# Content Identity  (person-scoped, portable, optionally off-device)
# ---------------------------------------------------------------------------

def derive_content_keypair(mnemonic: str, passphrase: str = "") -> tuple[bytes, bytes]:
    """
    Derive a Content Identity keypair from a BIP-39 mnemonic phrase.

    Uses the same derivation as BIP-32: PBKDF2-HMAC-SHA512 over the
    mnemonic words with salt "mnemonic" + passphrase, 2048 iterations.
    The first 32 bytes of the 64-byte seed become the Ed25519 private key.

    SECURITY: The private key is returned in memory. The caller is responsible
    for zeroing it after use (assign None, call gc.collect() in CPython).
    Never persist the private key to disk in high-risk deployments.

    Args:
        mnemonic:   BIP-39 mnemonic phrase (12, 18, or 24 space-separated words)
        passphrase: Optional additional passphrase (empty string = none)

    Returns:
        (private_key_bytes, public_key_bytes) — both 32 bytes
    """
    seed = hashlib.pbkdf2_hmac(
        hash_name   = "sha512",
        password    = mnemonic.encode("utf-8"),
        salt        = ("mnemonic" + passphrase).encode("utf-8"),
        iterations  = 2048,
        dklen       = 64,
    )
    private_bytes = seed[:32]
    private_key   = Ed25519PrivateKey.from_private_bytes(private_bytes)
    public_bytes  = private_key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    return private_bytes, public_bytes


def generate_mnemonic(strength: int = 128) -> str:
    """
    Generate a new BIP-39 mnemonic phrase.
    strength=128 → 12 words; strength=256 → 24 words.
    Requires the 'mnemonic' package.
    """
    try:
        from mnemonic import Mnemonic
        mnemo = Mnemonic("english")
        return mnemo.generate(strength=strength)
    except ImportError:
        raise ImportError(
            "The 'mnemonic' package is required for mnemonic generation. "
            "Install it with: pip install mnemonic"
        )


# ---------------------------------------------------------------------------
# Signature payload builders
# These construct the exact bytes that each message type signs over,
# matching the definitions in PROTOCOL.md.
# ---------------------------------------------------------------------------

import cbor2


def forwarding_declaration_payload(
    file_id:   bytes,   # 32-byte SHA-256 of file content
    fwd_tpk:   bytes,   # 32-byte Transport Public Key of declaring node
    action:    int,     # ACTION_RECOMMEND_STRONG, ACTION_RECOMMEND, or ACTION_PASS_ALONG
    timestamp: int,     # Unix timestamp
) -> bytes:
    """
    Build the canonical CBOR payload for a Forwarding Declaration signature.
    See PROTOCOL.md §9.4.
    """
    return cbor2.dumps([file_id, fwd_tpk, action, timestamp])


def file_metadata_payload(
    file_id:   bytes,
    timestamp: int,
    mime:      str,
    size:      int,
    title:     str,
    tags:      list[str],
    routing:   str,
    recipient: Optional[bytes],
) -> bytes:
    """
    Build the canonical CBOR payload for a content creator signature.
    See PROTOCOL.md §5.2.
    """
    return cbor2.dumps([
        file_id,
        timestamp,
        mime,
        size,
        title or "",
        tags or [],
        routing,
        recipient,
    ])


def endorsement_payload(
    endorsed_tpk:    bytes,
    timestamp:       int,
    encounter_count: int,
    attest_hash:     Optional[bytes] = None,
) -> bytes:
    """
    Build the canonical CBOR payload for a transport endorsement signature.
    See GAMIFICATION.md §5.2.
    """
    payload = [endorsed_tpk, timestamp, encounter_count]
    if attest_hash is not None:
        payload.append(attest_hash)
    return cbor2.dumps(payload)


def attestation_payload_a(
    session_nonce:       bytes,
    timestamp:           int,
    node_a_tpk:          bytes,
    node_b_tpk:          bytes,
    node_a_fingerprint:  bytes = b"",
) -> bytes:
    """
    Build Node A's attestation signature payload. See GAMIFICATION.md §3.3.
    """
    return cbor2.dumps([session_nonce, timestamp, node_a_tpk, node_b_tpk, node_a_fingerprint])


def attestation_payload_b(
    session_nonce:       bytes,
    timestamp:           int,
    node_a_tpk:          bytes,
    node_b_tpk:          bytes,
    node_b_fingerprint:  bytes = b"",
) -> bytes:
    """
    Build Node B's attestation signature payload. See GAMIFICATION.md §3.3.
    """
    return cbor2.dumps([session_nonce, timestamp, node_a_tpk, node_b_tpk, node_b_fingerprint])


def rating_payload(
    file_id:   bytes,
    action:    int,
    timestamp: int,
) -> bytes:
    """
    Build the canonical CBOR payload for a rating signature.
    See PROTOCOL.md §3.3.7.
    """
    return cbor2.dumps([file_id, action, timestamp])
