# Waxwing Mesh

> *Cedar waxwings are known for one remarkable behaviour: they pass berries beak-to-beak down a line of birds, one at a time, until every bird has eaten. No coordinator. No hierarchy. Just community.*

Waxwing Mesh is an open protocol and application suite for opportunistic, privacy-preserving file sharing over Bluetooth Low Energy — with optional WiFi upgrade for large transfers. Devices running Waxwing Mesh form a delay-tolerant network (DTN): when two devices come within range of each other, they exchange content automatically, without any infrastructure, accounts, or internet connection required.

Content spreads the way waxwings share berries — peer to peer, device to device, carried by people moving through the world.

---

## What It Is

A **Waxwing node** is a small, portable device (a Raspberry Pi Pico W with an SD card, a Flipper Zero, a CardPuter) that you carry with you. It advertises via BLE, connects promiscuously to other Waxwing nodes it encounters, compares content libraries, and exchanges files neither device has seen before.

A **Waxwing companion app** (iOS) is how you interact with your node when you're near it — browsing content, consuming media, rating what you've seen, pushing your own content into the network, and configuring your node's preferences and subscriptions.

Together, they form a mesh that requires no servers, no accounts, and no internet — while still being able to use WiFi opportunistically when available.

---

## Design Principles

**Device-first.** Your phone is a window into your node, not the node itself. The mesh runs on dedicated hardware. This protects your privacy (seizing a phone yields little), extends battery life, and sidesteps platform restrictions on background BLE.

**Review-gated propagation.** Content does not hop freely from device to device. A file is only forwarded after its owner has reviewed it. Bad content propagates at human speed, bounded by how many people actually open it — not machine speed.

**Explicit pass-along.** After reviewing content, the owner chooses what happens next: recommend it, pass it along for others who might care, keep it private, or reject it as spam. "Pass it along" is a first-class action — a small civic gesture that keeps niche content alive in the network without requiring the owner to personally endorse it. The recommendation system actively encourages this choice for content that isn't offensive but isn't the owner's interest.

**Two-layer identity.** Every device has a *transport identity* (used for spam filtering and reputation at the transfer layer) that is separate from any *content identity* (who created a file). Content identities are portable — derived from a memorised seed phrase or stored on a hardware token — so a creator's signing key never needs to reside on any single device.

**Niche content survives.** Propagation uses an "spread unless stopped" model rather than "spread only if popular." A "pass it along" action keeps content moving through the network even when most owners aren't personally interested — protecting minority viewpoints, regional content, and specialised topics from being filtered out by majority taste. Only explicit rejection stops a file from spreading further.

**Privacy by design.** Anonymous and pseudonymous content creation are first-class features, not afterthoughts. Device identities are rotatable. Creator identities can be entirely off-device. No telemetry, no central registry.

**Optional social layer.** Nodes can optionally track encounter history, exchange cryptographically signed sync attestations, and log WiFi fingerprints for location inference — powering a sync map, peer graph, and milestone badges in the companion app. All social features are opt-in and disabled by default. A node running default settings stores no location data and no encounter history.

---

## Hardware Targets

| Device | Role | Notes |
|---|---|---|
| Raspberry Pi Pico W | Reference mesh node | CYW43439 BLE 4.2 + WiFi; add SD card + LiPo for portable node |
| Flipper Zero | Mesh node | nRF52840 BLE; custom Flipper app; good for testing |
| M5Stack CardPuter ADV | Mesh node | ESP32-S3; BLE + WiFi; compact form factor |
| iOS (iPhone) | Companion app | Primary target; Swift app |
| Android | Companion app | Secondary target (planned) |

---

## Architecture

```
                    ┌─────────────────────────────────────────┐
                    │           Waxwing Mesh Network           │
                    │                                          │
  [Pico W Node] ←──BLE/WiFi──→ [Flipper Node] ←──BLE──→ [CardPuter Node]
        ↑                                                       ↑
        │ BLE (discovery)                                       │
        │ WiFi (content, when on home network)                  │
        ↓                                                       │
  [Your iPhone]                                          [Their iPhone]
  Companion App                                          Companion App
```

The mesh network is **device-to-device**. Companion apps connect only to their paired node — they are not mesh participants themselves. This keeps the mesh running even when your phone is locked in your pocket, and eliminates the iOS background BLE limitations that would otherwise severely constrain the protocol.

When a node returns to its home WiFi network, it connects automatically and advertises its local IP via BLE to the companion app. The companion app switches from BLE to WiFi for fast access to the content library.

---

## Content Flow

```
[Creator] → signs file with Content Identity keypair
         → pushes to their Node via companion app
              ↓
         Node advertises file in BLE manifest
              ↓
         Nearby Node connects, compares manifests
              ↓
         File transfers (BLE chunks, or WiFi if negotiated)
              ↓
         Recipient's companion app shows new content
              ↓
         User consumes and rates content
              ↓
         Rating recorded; file becomes eligible for forwarding
              ↓
         Next encounter: file propagates onward
```

---

## Repository Structure

```
Waxwing/
├── README.md               # This file
├── PROTOCOL.md             # Full protocol specification (start here)
├── protocol/
│   ├── GAMIFICATION.md     # Social layer: encounter ledger, sync attestation, geolocation, trust graph
│   └── schemas/
│       ├── file-metadata.schema.json
│       └── manifest.schema.json
├── firmware/
│   ├── pico-w/             # Raspberry Pi Pico W (C firmware)
│   ├── flipper/            # Flipper Zero – C firmware planned
│   └── cardputer/          # M5Stack CardPuter – C firmware planned
├── ios/
│   └── WaxwingCompanion/   # Native Swift companion app (iOS)
├── tools/                  # BLE sniffers, test harnesses, manifest generators
└── docs/
    ├── architecture.md     # Extended architecture discussion (planned)
    └── decisions/          # Architecture Decision Records (ADRs)
        ├── 001-device-first-architecture.md
        └── 002-gamification-opt-in.md
```

---

## Status

**Alpha – core functionality implemented.**

- ✅ **Pico W firmware (C)** – BLE advertising, Device Identity characteristic, file commands (`ls`, `read`, `write`, `delete`, `storage_info`), FAT filesystem, CBOR codec, Ed25519 transport identity via monocypher.
- ✅ **Host unit tests** – `./build-host/waxwing_test` passes (build with `-DWAXWING_HOST_BUILD=ON`).
- ✅ **iOS companion app (Swift)** – scans, connects, reads Device Identity, lists files, reads/writes files, displays images.
- ⏳ **Manifest generation & WiFi upgrade** – in progress.
- ⏳ **Reputation gossip & social layer** – planned (opt‑in).
- ⏳ Flipper Zero & CardPuter ports – planned.

---

## Protocol Version

This repository implements **Waxwing Mesh Protocol v0.1**. See `PROTOCOL.md` for the full specification.

---

## Name

The cedar waxwing (*Bombycilla cedrorum*) is a North American songbird known for passing berries beak‑to‑beak along a line of perched birds — a spontaneous act of community sharing with no leader and no queue manager. It is one of nature's clearest illustrations of a relay network.

---

## Licence

TBD — intended to be permissively open source. Contributions welcome once the initial implementation is underway.