# Waxwing Mesh — Companion App

**Status:** iOS companion app (native Swift) implemented; Android/Flutter planned.

## Overview

The companion app is the owner's interface to their Waxwing node. It is **not** a mesh participant — it connects only to its paired home node.

- **iOS:** A native Swift application located at `ios/WaxwingCompanion/`. It implements scanning, connecting, reading Device Identity, listing files, reading/writing files, displaying images, and submitting ratings.
- **Android / Flutter:** Not yet implemented (planned for future work). The iOS app can serve as a reference for any future cross‑platform effort.

## Key Screens (iOS)

- **Feed** — browse content on the home node; consume text, audio, video; submit review actions (Recommend / Pass Along / Hold / Reject)
- **Publish** — push content from phone (photos, voice memos, text notes) into the mesh
- **Subscriptions** — manage followed creator public keys and interest tags
- **Reputation** — view and manage the reputation ledger; blocklist management
- **Node Status** — mesh activity, storage usage, recent exchanges, transfer history
- **Settings** — home WiFi configuration, node pairing, content identity management, social feature opt-ins

## BLE Connection

The iOS app connects to the home node via BLE using the paired node's Transport Public Key (scanned as a QR code during initial pairing). All BLE operations use the Waxwing GATT service defined in `PROTOCOL.md`.

When the node reports its home WiFi IP via Device Config NOTIFY, the app automatically switches to the Waxwing Wire Transfer protocol over local WiFi for fast content access.

## Content Identity Management

The iOS app supports three content identity modes:

1. **Mnemonic mode** — user enters BIP‑39 phrase, key is derived in memory, content is signed, key is zeroed. Highest privacy; no key stored on device.
2. **Stored mode** — Content Identity keypair stored in iOS Secure Enclave. Convenient; reduced privacy if device is compromised.
3. **Hardware token mode** — signing delegated to a connected hardware token (e.g., Flipper Zero) via BLE. Highest security; private key never touches the phone.

## Dependencies (iOS)

- `CoreBluetooth` framework
- `Combine` framework
- In-house CBOR encoder/decoder (`CBOREncoder.swift`, `CBORDecoder.swift`) — minimal, matched to the firmware codec
- Apple `CryptoKit` for Ed25519
- `AVFoundation` for media playback
- `PhotosUI` / `PHPickerViewController` for photo selection
- `Keychain` / `UserDefaults` for settings and secrets

## See Also

- `../PROTOCOL.md` — full protocol specification
- `../ios/WaxwingCompanion/README.md` (if exists) — project‑specific setup
- `../ios/SETUP.md` — Xcode project setup guide