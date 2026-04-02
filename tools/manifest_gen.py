#!/usr/bin/env python3
"""
Waxwing Mesh — Test Manifest Generator

Generates synthetic Waxwing content and manifests for testing firmware and
the BLE scanner before real content is available. Outputs CBOR files that
a firmware simulator or test harness can serve from a GATT Manifest Chunk
characteristic.

Also handles keypair generation and BIP-39 mnemonic derivation.

Usage:
    # Generate a manifest with 5 random text files, sign with a new keypair
    python manifest_gen.py --count 5 --mime text/plain --out test_manifest.cbor

    # Generate mixed content (text, audio, video stubs)
    python manifest_gen.py --count 10 --mixed --out test_manifest.cbor

    # Generate and display a new Transport Identity keypair
    python manifest_gen.py --keygen

    # Generate a new BIP-39 mnemonic and derive a Content Identity keypair
    python manifest_gen.py --mnemonic

    # Sign an existing manifest with a keypair from a mnemonic
    python manifest_gen.py --count 3 --mnemonic-sign "word1 word2 ... word12"

Requirements:
    pip install cbor2 cryptography mnemonic
"""

import argparse
import base64
import hashlib
import json
import os
import random
import struct
import sys
import time
from pathlib import Path
from typing import Optional

import cbor2

from waxwing.constants import (
    ROUTING_BROADCAST,
    DEFAULT_TTL,
    ACTION_RECOMMEND_STRONG, ACTION_RECOMMEND, ACTION_PASS_ALONG,
)
from waxwing.messages import (
    build_file_metadata,
    build_forwarding_declaration,
    build_manifest_payload,
)
from waxwing.crypto import (
    generate_transport_keypair,
    generate_mnemonic,
    derive_content_keypair,
    sign,
    file_metadata_payload,
)

# ---------------------------------------------------------------------------
# Sample content pools for realistic-looking test data
# ---------------------------------------------------------------------------

SAMPLE_TITLES = [
    "Community meeting notes — town square",
    "Field recording: morning birds, eastern park",
    "Statement on the recent water supply situation",
    "Short story: The Last Relay",
    "Market prices this week",
    "Safety update for the northern district",
    "Interview with a local teacher",
    "Map update: new trail markers",
    "Recipe collection: preserved foods",
    "Music: traditional songs, new recording",
    "Weather observations, past 30 days",
    "Notice: road closure near the bridge",
    "Oral history: the 1987 flood",
    "Workshop guide: basic radio operation",
    "Community health bulletin",
    "Voice message from the eastern collective",
    "Photographs: spring planting season",
    "Technical notes: mesh node deployment",
    "Call for volunteers: harvest week",
    "Translation: emergency phrases in three languages",
]

SAMPLE_TAGS_POOL = [
    "news", "community", "health", "weather", "food", "safety", "culture",
    "technical", "history", "music", "map", "announcement", "agriculture",
    "en", "fr", "es", "ar", "zh",
]

MIME_TYPES = {
    "text":  "text/plain",
    "audio": "audio/mp4",
    "video": "video/mp4",
    "image": "image/jpeg",
}

TYPICAL_SIZES = {
    "text/plain":  (512,    65_536),        # 0.5 KB – 64 KB
    "audio/mp4":   (500_000, 10_000_000),   # 0.5 MB – 10 MB
    "video/mp4":   (5_000_000, 100_000_000),# 5 MB – 100 MB
    "image/jpeg":  (50_000, 2_000_000),     # 50 KB – 2 MB
}


# ---------------------------------------------------------------------------
# Content generation
# ---------------------------------------------------------------------------

def random_file_content(mime: str, size_override: Optional[int] = None) -> bytes:
    """
    Generate plausible fake file content for testing.
    For text/plain, generates readable ASCII. For binary types, random bytes.
    """
    if mime == "text/plain":
        words = [
            "the", "and", "a", "to", "of", "in", "is", "it", "that", "was",
            "for", "on", "with", "as", "at", "this", "but", "from", "not",
            "we", "our", "they", "have", "will", "be", "or", "by", "more",
            "community", "relay", "node", "message", "content", "share",
            "waxwing", "network", "local", "device", "trust", "freedom",
        ]
        size   = size_override or random.randint(*TYPICAL_SIZES["text/plain"])
        result = []
        while sum(len(w) + 1 for w in result) < size:
            result.append(random.choice(words))
            if random.random() < 0.15:
                result.append("\n")
        return " ".join(result).encode("utf-8")[:size]
    else:
        lo, hi = TYPICAL_SIZES.get(mime, (1024, 65536))
        size   = size_override or random.randint(lo, hi)
        return os.urandom(size)


def build_test_item(
    mime:                str,
    transport_priv_key:  bytes,
    transport_pub_key:   bytes,
    content_priv_key:    Optional[bytes] = None,
    content_pub_key:     Optional[bytes] = None,
    action:              int              = ACTION_PASS_ALONG,
    timestamp_offset:    int              = 0,
) -> tuple[dict, bytes]:
    """
    Build a single test file metadata record plus its content.
    Returns (metadata_dict, file_content_bytes).
    """
    title   = random.choice(SAMPLE_TITLES)
    tags    = random.sample(SAMPLE_TAGS_POOL, k=random.randint(1, 4))
    content = random_file_content(mime)
    file_id = hashlib.sha256(content).digest()
    ts      = int(time.time()) - timestamp_offset

    # Creator signature (if Content Identity provided)
    creator_pk  = None
    creator_sig = None
    if content_priv_key and content_pub_key:
        creator_pk = content_pub_key
        payload    = file_metadata_payload(
            file_id   = file_id,
            timestamp = ts,
            mime      = mime,
            size      = len(content),
            title     = title,
            tags      = tags,
            routing   = ROUTING_BROADCAST,
            recipient = None,
        )
        creator_sig = sign(content_priv_key, payload)

    # Forwarding Declaration from the attended node
    fwd_decl = build_forwarding_declaration(
        fwd_tpk            = transport_pub_key,
        action             = action,
        timestamp          = ts,
        transport_priv_key = transport_priv_key,
        file_id            = file_id,
    )

    metadata = build_file_metadata(
        file_content = content,
        mime         = mime,
        origin_tpk   = transport_pub_key,
        title        = title,
        tags         = tags,
        routing      = ROUTING_BROADCAST,
        ttl          = DEFAULT_TTL,
        creator_pk   = creator_pk,
        creator_sig  = creator_sig,
        fwd_decl     = fwd_decl,
        timestamp    = ts,
    )

    return metadata, content


def generate_manifest(
    count:               int,
    mime:                str             = "text/plain",
    mixed:               bool            = False,
    transport_priv_key:  Optional[bytes] = None,
    transport_pub_key:   Optional[bytes] = None,
    content_priv_key:    Optional[bytes] = None,
    content_pub_key:     Optional[bytes] = None,
) -> tuple[list[dict], list[bytes]]:
    """
    Generate `count` test metadata records plus their file contents.
    Returns (metadata_list, content_list).
    """
    if not transport_priv_key:
        transport_priv_key, transport_pub_key = generate_transport_keypair()

    mimes       = list(MIME_TYPES.values()) if mixed else [mime]
    metadata_l  = []
    content_l   = []

    actions = [ACTION_RECOMMEND_STRONG, ACTION_RECOMMEND, ACTION_PASS_ALONG,
               ACTION_PASS_ALONG, ACTION_PASS_ALONG]  # weighted toward Pass Along

    for i in range(count):
        item_mime    = random.choice(mimes)
        action       = random.choice(actions)
        offset       = random.randint(0, 7 * 24 * 3600)  # up to 7 days ago

        meta, content = build_test_item(
            mime               = item_mime,
            transport_priv_key = transport_priv_key,
            transport_pub_key  = transport_pub_key,
            content_priv_key   = content_priv_key,
            content_pub_key    = content_pub_key,
            action             = action,
            timestamp_offset   = offset,
        )
        metadata_l.append(meta)
        content_l.append(content)

    return metadata_l, content_l


# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

def fmt_size(n: int) -> str:
    if n < 1024:       return f"{n} B"
    if n < 1_048_576:  return f"{n/1024:.1f} KB"
    return f"{n/1_048_576:.1f} MB"

def b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).decode().rstrip("=")


def print_keypair_info(label: str, priv: bytes, pub: bytes) -> None:
    print(f"\n  {label}")
    print(f"    Public key  : {b64u(pub)}")
    print(f"    Private key : {b64u(priv)}")
    print(f"    Key ID      : WX:{pub.hex()[:8].upper()}")


def print_manifest_summary(metadata_list: list[dict]) -> None:
    from waxwing.constants import ACTION_LABELS
    print(f"\n  Generated {len(metadata_list)} item(s):\n")
    for i, m in enumerate(metadata_list):
        file_id   = m.get("id", b"").hex()[:12] + "…"
        title     = m.get("title", "(untitled)")[:45]
        mime      = m.get("mime", "?")
        size      = fmt_size(m.get("size", 0))
        fwd       = m.get("fwd_decl", {})
        action    = ACTION_LABELS.get(fwd.get("action"), "?")
        signed    = "signed" if m.get("cpk") else "anonymous"
        print(f"  [{i+1:2}] {title}")
        print(f"       {file_id}  {mime}  {size}  {action}  {signed}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Waxwing Mesh — test manifest and keypair generator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python manifest_gen.py --keygen
  python manifest_gen.py --mnemonic
  python manifest_gen.py --count 5 --out test_manifest.cbor
  python manifest_gen.py --count 10 --mixed --sign --out signed_manifest.cbor
  python manifest_gen.py --count 3 --mnemonic-sign "word1 word2 ... word12" --out manifest.cbor
        """,
    )

    parser.add_argument("--keygen",       action="store_true",
                        help="Generate and display a new Transport Identity keypair")
    parser.add_argument("--mnemonic",     action="store_true",
                        help="Generate a new BIP-39 mnemonic and derive a Content Identity keypair")
    parser.add_argument("--count",        type=int, default=5,
                        help="Number of test items to generate (default: 5)")
    parser.add_argument("--mime",         type=str, default="text/plain",
                        choices=list(MIME_TYPES.values()) + list(MIME_TYPES.keys()),
                        help="MIME type for generated content (default: text/plain)")
    parser.add_argument("--mixed",        action="store_true",
                        help="Generate a mix of text, audio, video, and image items")
    parser.add_argument("--sign",         action="store_true",
                        help="Sign generated content with a new Content Identity keypair")
    parser.add_argument("--mnemonic-sign", type=str, default=None, metavar="WORDS",
                        help="Sign content using a Content Identity derived from this mnemonic")
    parser.add_argument("--out",          type=str, default=None,
                        help="Write manifest CBOR to this file (prints to stdout if omitted)")
    parser.add_argument("--save-keys",    type=str, default=None, metavar="FILE",
                        help="Save generated keypairs to a JSON file for reuse")
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    print("\nWaxwing Mesh — Test Manifest Generator")
    print("=" * 40)

    # --- Keypair generation only ---
    if args.keygen:
        priv, pub = generate_transport_keypair()
        print_keypair_info("New Transport Identity keypair", priv, pub)
        print("\n  Store the private key securely. Share only the public key.")
        return

    # --- Mnemonic generation only ---
    if args.mnemonic and not args.count:
        mnemonic = generate_mnemonic(strength=128)
        priv, pub = derive_content_keypair(mnemonic)
        print(f"\n  BIP-39 mnemonic (12 words):")
        print(f"    {mnemonic}")
        print_keypair_info("Derived Content Identity keypair", priv, pub)
        print("\n  SECURITY: Memorise the mnemonic. Delete any written copy after use.")
        return

    # --- Manifest generation ---

    # Transport Identity
    transport_priv, transport_pub = generate_transport_keypair()
    print_keypair_info("Transport Identity (for this test manifest)", transport_priv, transport_pub)

    # Content Identity (optional)
    content_priv: Optional[bytes] = None
    content_pub:  Optional[bytes] = None

    if args.mnemonic_sign:
        content_priv, content_pub = derive_content_keypair(args.mnemonic_sign)
        print_keypair_info("Content Identity (from mnemonic)", content_priv, content_pub)
    elif args.sign or args.mnemonic:
        if args.mnemonic:
            mnemonic = generate_mnemonic(strength=128)
            print(f"\n  New BIP-39 mnemonic (12 words):")
            print(f"    {mnemonic}")
            content_priv, content_pub = derive_content_keypair(mnemonic)
        else:
            content_priv, content_pub = generate_transport_keypair()
        print_keypair_info("Content Identity", content_priv, content_pub)

    # Resolve MIME type shorthand
    mime = MIME_TYPES.get(args.mime, args.mime)

    # Generate
    print(f"\n  Generating {args.count} item(s) "
          f"({'mixed types' if args.mixed else mime})…")
    metadata_list, content_list = generate_manifest(
        count              = args.count,
        mime               = mime,
        mixed              = args.mixed,
        transport_priv_key = transport_priv,
        transport_pub_key  = transport_pub,
        content_priv_key   = content_priv,
        content_pub_key    = content_pub,
    )

    print_manifest_summary(metadata_list)

    # Serialise
    manifest_cbor = build_manifest_payload(metadata_list)
    print(f"\n  Manifest CBOR size: {fmt_size(len(manifest_cbor))}")

    if args.out:
        out_path = Path(args.out)
        out_path.write_bytes(manifest_cbor)
        print(f"  Written to: {out_path.resolve()}")
    else:
        print(f"\n  CBOR (hex, first 128 bytes): {manifest_cbor[:128].hex()}…")
        print(f"  Use --out <file> to save the full manifest.")

    # Save keys if requested
    if args.save_keys:
        keys = {
            "transport_private_key": b64u(transport_priv),
            "transport_public_key":  b64u(transport_pub),
        }
        if content_priv and content_pub:
            keys["content_private_key"] = b64u(content_priv)
            keys["content_public_key"]  = b64u(content_pub)
        Path(args.save_keys).write_text(json.dumps(keys, indent=2))
        print(f"  Keys saved to: {args.save_keys}")
        print("  WARNING: This file contains private keys. Keep it secure.")

    print()


if __name__ == "__main__":
    main()
