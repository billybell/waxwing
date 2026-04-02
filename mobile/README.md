# Waxwing Mesh — Companion App (Flutter)

**Status:** Not yet implemented.

## Overview

The companion app is the owner's interface to their Waxwing node. It is **not** a mesh participant — it connects only to its paired home node. It is built with Flutter for iOS (primary) and Android (secondary).

## Key Screens

### Core (always available)
- **Feed** — browse content on the home node; consume text, audio, video; submit review actions (Recommend / Pass Along / Hold / Reject)
- **Publish** — push content from phone (photos, voice memos, text notes) into the mesh
- **Subscriptions** — manage followed creator public keys and interest tags
- **Reputation** — view and manage the reputation ledger; blocklist management
- **Node Status** — mesh activity, storage usage, recent exchanges, transfer history
- **Settings** — home WiFi configuration, node pairing, content identity management, social feature opt-ins

### Social Layer (opt-in, disabled by default)
- **Community Graph** — interactive force-directed graph of your local Waxwing community; nodes coloured by reputation tier (green/blue/grey/amber/red); solid edges = direct sync, dashed = endorsement chain; up to 3 hops out from your node; tap any node for detail; filter by hop depth, reputation, or content tag
- **Leaderboard** — ranked view of your known community across six categories: Most Active (bytes shared), Most Trusted (effective reputation), Most Connected (network footprint), Most Generous (pass-along rate), Explorer (locations synced), and Newcomer (recent first encounters); your own node always shown with rank
- **Sync Map** — map view of approximate locations where sync events occurred; dots coloured by encounter type; requires geolocation opt-in
- **Sync History** — reverse-chronological list of sync sessions with transfer stats, location, and attestation status
- **Peer Detail** — per-peer stats: encounter count, bytes sent/received, average content quality, trust path chain, content tags, encounter timeline
- **Statistics** — lifetime totals, pass-along rate, unique nodes encountered, network reach estimate, milestone badges
- **Attestation Export** — export a cryptographically signed sync record as a self-contained JSON document for third-party verification; each link in the trust path is independently verifiable with a standard Ed25519 library

## BLE Connection

The app connects to the home node via BLE using the paired node's Transport Public Key (scanned as a QR code during initial pairing). All BLE operations use the Waxwing GATT service defined in `PROTOCOL.md`.

When the node reports its home WiFi IP via Device Config NOTIFY, the app automatically switches to the Waxwing Wire Transfer protocol over local WiFi for fast content access.

## Content Identity Management

The app supports three content identity modes:

1. **Mnemonic mode** — user enters BIP-39 phrase, key is derived in memory, content is signed, key is zeroed. Highest privacy; no key stored on device.
2. **Stored mode** — Content Identity keypair stored in iOS Secure Enclave / Android Keystore. Convenient; reduced privacy if device is compromised.
3. **Hardware token mode** — signing delegated to a connected hardware token (e.g., Flipper Zero) via BLE. Highest security; private key never touches the phone.

## Dependencies (planned)

- `flutter_blue_plus` — BLE GATT client
- `cbor2` or equivalent Dart CBOR library
- `bip39` Dart package — mnemonic generation and seed derivation
- `ed25519` Dart package — key generation and signing
- `just_audio` — audio playback
- `video_player` — video playback
- `flutter_map` + `openstreetmap` — sync map display (no API key required)
- `geolocator` — iOS Core Location access for WiFi-based geolocation
- `fl_chart` — bytes exchanged charts and statistics visualisations
- `graphview` — force-directed community graph layout and rendering

## See Also

- `../PROTOCOL.md` — full protocol specification
- `../protocol/GATT.md` — BLE characteristic detail
