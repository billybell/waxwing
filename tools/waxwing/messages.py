"""
Waxwing Mesh Message Builders and Parsers
CBOR encoding/decoding for all GATT characteristic payloads.

All parse_* functions accept raw bytes and return a plain dict.
All build_* functions accept typed arguments and return bytes ready to write.
"""

import hashlib
import time
from typing import Optional

import cbor2

from .constants import (
    PROTOCOL_VERSION, PROTOCOL_NAME,
    CAP_WIFI_CLIENT, CAP_WIFI_AP, CAP_MULTIPEER, CAP_LOCAL_NETWORK, CAP_UNATTENDED,
    ROUTING_BROADCAST, DEFAULT_TTL,
    ACTION_PASS_ALONG,
    REQ_REQUEST, REQ_CANCEL, REQ_MANIFEST_READ,
    CTRL_ACK_CHUNK, CTRL_NACK_CHUNK, CTRL_PAUSE, CTRL_RESUME, CTRL_COMPLETE,
    MANIFEST_HEADER_SIZE,
)


# ---------------------------------------------------------------------------
# Device Identity  (CHAR_DEVICE_IDENTITY — READ)
# ---------------------------------------------------------------------------

def build_device_identity(
    transport_public_key: bytes,
    name:                 Optional[str]  = None,
    caps:                 int            = 0,
    manifest_count:       int            = 0,
    attended:             bool           = True,
    unattended_mode:      Optional[str]  = None,
) -> bytes:
    """
    Build a Device Identity CBOR payload for the CHAR_DEVICE_IDENTITY characteristic.
    See PROTOCOL.md §3.3.1.
    """
    doc = {
        "v":               PROTOCOL_VERSION,
        "tpk":             transport_public_key,
        "caps":            caps,
        "manifest_count":  manifest_count,
        "attended":        attended,
        "protocol":        PROTOCOL_NAME,
    }
    if name:
        doc["name"] = name[:32]
    if not attended and unattended_mode:
        doc["unattended_mode"] = unattended_mode
    return cbor2.dumps(doc)


def parse_device_identity(raw: bytes) -> dict:
    """
    Parse a Device Identity CBOR payload. Returns a dict with string keys.
    Raises ValueError on malformed input.
    """
    try:
        doc = cbor2.loads(raw)
    except Exception as e:
        raise ValueError(f"Failed to decode Device Identity CBOR: {e}") from e

    if not isinstance(doc, dict):
        raise ValueError("Device Identity must be a CBOR map")
    if doc.get("protocol") != PROTOCOL_NAME:
        raise ValueError(f"Unexpected protocol field: {doc.get('protocol')!r}")
    if "tpk" not in doc:
        raise ValueError("Device Identity missing required field 'tpk'")

    return doc


# ---------------------------------------------------------------------------
# File Metadata  (embedded in manifests and transferred with files)
# ---------------------------------------------------------------------------

def build_file_metadata(
    file_content:   bytes,
    mime:           str,
    origin_tpk:     bytes,
    title:          Optional[str]        = None,
    tags:           Optional[list[str]]  = None,
    routing:        str                  = ROUTING_BROADCAST,
    recipient:      Optional[bytes]      = None,
    ttl:            int                  = DEFAULT_TTL,
    creator_pk:     Optional[bytes]      = None,
    creator_sig:    Optional[bytes]      = None,
    fwd_decl:       Optional[dict]       = None,
    timestamp:      Optional[int]        = None,
) -> dict:
    """
    Build a file metadata dict from file content and parameters.
    The file ID (SHA-256 of content) is computed automatically.
    Returns a plain dict; use cbor2.dumps() to serialise for transmission.
    """
    file_id   = hashlib.sha256(file_content).digest()
    ts        = timestamp or int(time.time())

    doc: dict = {
        "id":         file_id,
        "v":          1,
        "timestamp":  ts,
        "mime":       mime,
        "size":       len(file_content),
        "routing":    routing,
        "ttl":        ttl,
        "origin_tpk": origin_tpk,
    }

    if title:
        doc["title"] = title[:128]
    if tags:
        doc["tags"] = [t[:32] for t in tags[:16]]
    if creator_pk:
        doc["cpk"]  = creator_pk
    if creator_sig:
        doc["csig"] = creator_sig
    if recipient and routing == "addressed":
        doc["recipient"] = recipient
    if fwd_decl:
        doc["fwd_decl"] = fwd_decl

    return doc


def parse_file_metadata(raw_or_dict) -> dict:
    """
    Parse and lightly validate a file metadata record.
    Accepts either raw CBOR bytes or an already-decoded dict (from a manifest).
    """
    if isinstance(raw_or_dict, bytes):
        try:
            doc = cbor2.loads(raw_or_dict)
        except Exception as e:
            raise ValueError(f"Failed to decode file metadata CBOR: {e}") from e
    else:
        doc = raw_or_dict

    required = {"id", "v", "timestamp", "mime", "size", "routing", "ttl", "origin_tpk"}
    missing  = required - set(doc.keys())
    if missing:
        raise ValueError(f"File metadata missing required fields: {missing}")

    if len(doc["id"]) != 32:
        raise ValueError("File metadata 'id' must be 32 bytes (SHA-256)")

    return doc


# ---------------------------------------------------------------------------
# Forwarding Declaration  (embedded in file metadata as "fwd_decl")
# ---------------------------------------------------------------------------

def build_forwarding_declaration(
    fwd_tpk:             bytes,
    action:              int,
    timestamp:           int,
    transport_priv_key:  bytes,
    file_id:             bytes,
) -> dict:
    """
    Build and sign a Forwarding Declaration.
    The signature covers (file_id, fwd_tpk, action, timestamp) as a CBOR array.
    See PROTOCOL.md §9.4.
    """
    from .crypto import sign, forwarding_declaration_payload

    payload = forwarding_declaration_payload(file_id, fwd_tpk, action, timestamp)
    sig     = sign(transport_priv_key, payload)

    return {
        "fwd_tpk":   fwd_tpk,
        "action":    action,
        "timestamp": timestamp,
        "sig":       sig,
    }


def verify_forwarding_declaration(fwd_decl: dict, file_id: bytes) -> bool:
    """
    Verify the signature on a Forwarding Declaration.
    Returns True if valid, False if invalid or missing fields.
    """
    from .crypto import verify, forwarding_declaration_payload

    try:
        payload = forwarding_declaration_payload(
            file_id,
            fwd_decl["fwd_tpk"],
            fwd_decl["action"],
            fwd_decl["timestamp"],
        )
        return verify(fwd_decl["fwd_tpk"], payload, fwd_decl["sig"])
    except (KeyError, Exception):
        return False


# ---------------------------------------------------------------------------
# Manifest  (CHAR_MANIFEST_CHUNK — READ)
# ---------------------------------------------------------------------------

def build_manifest_payload(metadata_list: list[dict]) -> bytes:
    """
    Serialise a list of file metadata dicts to CBOR for manifest transmission.
    The list should be sorted by timestamp descending (newest first).
    Returns the full CBOR payload; callers must chunk it for BLE transmission.
    """
    sorted_list = sorted(metadata_list, key=lambda m: m.get("timestamp", 0), reverse=True)
    return cbor2.dumps(sorted_list)


def parse_manifest_payload(raw: bytes) -> list[dict]:
    """Parse a complete manifest CBOR payload into a list of metadata dicts."""
    try:
        result = cbor2.loads(raw)
    except Exception as e:
        raise ValueError(f"Failed to decode manifest CBOR: {e}") from e
    if not isinstance(result, list):
        raise ValueError("Manifest must be a CBOR array")
    return [parse_file_metadata(item) for item in result]


def build_manifest_chunk_response(full_payload: bytes, offset: int, mtu: int) -> bytes:
    """
    Build a single chunk response for a manifest read at the given byte offset.
    Format: [total_length: 4 bytes LE][chunk_data: up to mtu-4-3 bytes]
    The -3 is for ATT overhead (handle + opcode).
    """
    chunk_size   = mtu - MANIFEST_HEADER_SIZE - 3
    chunk_data   = full_payload[offset : offset + chunk_size]
    total_length = len(full_payload)
    return total_length.to_bytes(4, "little") + chunk_data


# ---------------------------------------------------------------------------
# Transfer Request  (CHAR_TRANSFER_REQUEST — WRITE)
# ---------------------------------------------------------------------------

def build_transfer_request(file_id: bytes, resume_offset: int = 0) -> bytes:
    """Build a Transfer Request WRITE payload to request a file."""
    return bytes([REQ_REQUEST]) + file_id + resume_offset.to_bytes(4, "little")


def build_cancel_request(file_id: bytes) -> bytes:
    return bytes([REQ_CANCEL]) + file_id + (0).to_bytes(4, "little")


def build_manifest_read_request(offset: int) -> bytes:
    """Build a manifest read offset command for CHAR_TRANSFER_REQUEST."""
    return bytes([REQ_MANIFEST_READ]) + offset.to_bytes(4, "little")


# ---------------------------------------------------------------------------
# Transfer Control  (CHAR_TRANSFER_CONTROL — WRITE / NOTIFY)
# ---------------------------------------------------------------------------

def build_ack_chunk(file_id: bytes, seq: int) -> bytes:
    return bytes([CTRL_ACK_CHUNK]) + file_id + seq.to_bytes(4, "little")


def build_nack_chunk(file_id: bytes, seq: int) -> bytes:
    return bytes([CTRL_NACK_CHUNK]) + file_id + seq.to_bytes(4, "little")


def build_transfer_complete(file_id: bytes) -> bytes:
    return bytes([CTRL_COMPLETE]) + file_id + (0).to_bytes(4, "little")


def build_transfer_start_notify(file_id: bytes, total_size: int, total_chunks: int) -> bytes:
    from .constants import CTRL_TRANSFER_START
    return (bytes([CTRL_TRANSFER_START]) + file_id
            + total_size.to_bytes(4, "little")
            + total_chunks.to_bytes(4, "little"))


def parse_transfer_control(raw: bytes) -> dict:
    """Parse a Transfer Control message into a dict with 'opcode', 'file_id', 'payload'."""
    if len(raw) < 33:
        raise ValueError(f"Transfer Control too short: {len(raw)} bytes")
    return {
        "opcode":  raw[0],
        "file_id": raw[1:33],
        "payload": raw[33:],
    }


# ---------------------------------------------------------------------------
# Transfer Data chunk  (CHAR_TRANSFER_DATA — NOTIFY)
# ---------------------------------------------------------------------------

def build_transfer_data_chunk(file_id: bytes, seq: int, total: int, data: bytes) -> bytes:
    """Build a Transfer Data NOTIFY payload for one chunk."""
    return (file_id
            + seq.to_bytes(4, "little")
            + total.to_bytes(4, "little")
            + data)


def parse_transfer_data_chunk(raw: bytes) -> dict:
    """Parse a Transfer Data NOTIFY into file_id, seq, total, data."""
    if len(raw) < 40:
        raise ValueError(f"Transfer Data chunk too short: {len(raw)} bytes")
    return {
        "file_id": raw[:32],
        "seq":     int.from_bytes(raw[32:36], "little"),
        "total":   int.from_bytes(raw[36:40], "little"),
        "data":    raw[40:],
    }


# ---------------------------------------------------------------------------
# Reputation Exchange  (CHAR_REPUTATION_EXCHANGE — READ / WRITE)
# ---------------------------------------------------------------------------

def build_reputation_exchange(
    creator_rep:   Optional[list[dict]] = None,
    endorsements:  Optional[list[dict]] = None,
) -> bytes:
    """
    Build a Reputation Exchange payload.
    creator_rep:  list of {cpk, score, ratings, updated}
    endorsements: list of {endorser_tpk, endorsed_tpk, timestamp,
                           encounter_count, hops_remaining, endorser_sig,
                           attest_hash (optional)}
    """
    doc = {}
    if creator_rep:
        doc["creator_rep"] = creator_rep[:50]
    if endorsements:
        doc["endorsements"] = endorsements[:50]
    return cbor2.dumps(doc)


def parse_reputation_exchange(raw: bytes) -> dict:
    """Parse a Reputation Exchange CBOR payload."""
    try:
        doc = cbor2.loads(raw)
    except Exception as e:
        raise ValueError(f"Failed to decode Reputation Exchange CBOR: {e}") from e
    if not isinstance(doc, dict):
        raise ValueError("Reputation Exchange must be a CBOR map")
    return doc


# ---------------------------------------------------------------------------
# Rating Submission  (CHAR_RATING_SUBMISSION — WRITE, companion only)
# ---------------------------------------------------------------------------

def build_rating_submission(
    file_id:             bytes,
    creator_pk:          Optional[bytes],
    action:              int,
    transport_priv_key:  bytes,
) -> bytes:
    """
    Build and sign a Rating Submission payload.
    See PROTOCOL.md §3.3.7.
    """
    from .crypto import sign, rating_payload

    timestamp = int(time.time())
    payload   = rating_payload(file_id, action, timestamp)
    sig       = sign(transport_priv_key, payload)

    doc = {
        "file_id":    file_id,
        "creator_pk": creator_pk,
        "action":     action,
        "timestamp":  timestamp,
        "sig":        sig,
    }
    return cbor2.dumps(doc)
