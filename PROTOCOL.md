# Waxwing Mesh Protocol Specification

**Version:** 0.1 (Draft)
**Status:** Design phase — not yet implemented

This document is the normative specification for the Waxwing Mesh Protocol. All firmware implementations (Pico W, Flipper Zero, CardPuter) and the companion app must conform to this specification. Where behaviour is not specified, implementations should fail safely and conservatively.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Roles and Responsibilities](#2-roles-and-responsibilities)
3. [BLE GATT Service](#3-ble-gatt-service)
4. [Identity Model](#4-identity-model)
5. [File Metadata](#5-file-metadata)
6. [Manifest Exchange](#6-manifest-exchange)
7. [File Transfer Protocol](#7-file-transfer-protocol)
8. [Propagation Model](#8-propagation-model)
9. [Rating System](#9-rating-system)
10. [Reputation Model](#10-reputation-model)
11. [WiFi Upgrade Negotiation](#11-wifi-upgrade-negotiation)
12. [Companion App Protocol](#12-companion-app-protocol)
13. [Subscription System](#13-subscription-system)
14. [Unattended Mode](#14-unattended-mode)
15. [Security Considerations](#15-security-considerations)
16. [UUID Registry](#16-uuid-registry)
17. [Encoding](#17-encoding)
18. [Social Layer](#18-social-layer)

---

## 1. Overview

Waxwing Mesh is a delay-tolerant, opportunistic file-sharing protocol operating over Bluetooth Low Energy (BLE), with optional upgrade to WiFi for large transfers. Devices running the protocol advertise a custom GATT service, exchange content manifests when within range, and transfer files using a chunked, resumable protocol.

The protocol is designed to operate without infrastructure, accounts, or internet connectivity. All state is local. Reputation and ratings propagate as gossip alongside content metadata.

### 1.1 Design Goals

- **Interoperability.** A common GATT service works across all supported hardware and operating systems.
- **Privacy.** Device identity is separable from creator identity. Neither need be linkable to a real person.
- **Resilience.** No single point of failure. No central server. Content survives node loss.
- **Spam resistance.** Rating-gated forwarding prevents bad content from propagating at machine speed.
- **Niche content preservation.** Content is not filtered by popularity — only by explicit rejection.

### 1.2 Terminology

- **Node** — A hardware device participating in the Waxwing Mesh (Pico W, Flipper, CardPuter, etc.)
- **Companion** — A mobile app (iOS/Android) paired to a specific node
- **Peer** — Another node encountered during mesh operation
- **Content** — A file (text, audio, or video) stored on a node
- **Manifest** — The list of content a node has available to share
- **Transport Identity** — The keypair identifying a device at the transfer layer
- **Content Identity** — The keypair identifying the creator of a piece of content
- **TTL** — Time-to-live; the number of additional hops a file is permitted to make

---

## 2. Roles and Responsibilities

### 2.1 Node (Mesh Participant)

A node:

- Maintains a persistent Transport Identity keypair
- Advertises the Waxwing GATT service via BLE
- Scans for nearby nodes advertising the Waxwing service
- Connects to discovered peers (acting as GATT Central)
- Accepts connections from scanning peers (acting as GATT Peripheral)
- Stores content on local storage (SD card or flash)
- Maintains a reputation ledger for known creator identities
- Maintains a local propagation queue with priority tiers
- Connects to home WiFi when available and announces IP via BLE to paired companion

### 2.2 Companion App (Owner Interface)

A companion app:

- Pairs with exactly one node (its "home node")
- Connects to the home node via BLE (discovery) or WiFi (content access)
- Browses, consumes, and rates content on the home node
- Pushes content from the phone into the mesh via the home node
- Configures the home node (WiFi credentials, subscriptions, interest tags, blocklist)
- Is **not** a mesh participant — it does not connect to arbitrary nodes

### 2.3 Unattended Node

An unattended node operates without an active owner available to review content. It is a permanent or semi-permanent mesh participant — appropriate for businesses, community spaces, relay points, or any situation where a device is deployed rather than carried.

#### Accountability and provenance

Because no human is present to review content, **unattended nodes do not generate ratings and do not bear content reputation.** Expecting them to would be meaningless — there is no human judgment behind such a rating. Instead, accountability passes through them transparently: content carries a **Forwarding Declaration** signed by the last attended node whose human owner reviewed and chose to share it (see Section 9.4). When your node receives content via an unattended relay, it evaluates the *forwarder's* reputation, not the relay's. The unattended node is just a carrier.

This means the accountability question is always: *"Which attended human chose to share this, and do I trust their judgment?"* — not *"Do I trust this relay box?"*

#### Relay integrity

Unattended nodes do have one reputation axis: **relay integrity** — do they faithfully forward the content and signed metadata they receive, including the Forwarding Declarations and reputation gossip attached to it? An unattended node that strips provenance, modifies content, or selectively withholds signed reputation updates is behaving badly. This is tracked as a separate reputation score (see Section 10.5).

Crucially, signed reputation updates cannot be forged by a relay — they are signed by the node that generated them, and any node holding them can verify that signature. A relay can *withhold* a reputation update, but it cannot fabricate a positive one or neutralise a negative one. Network redundancy — the same updates travelling via multiple paths through different relays — limits the practical impact of selective withholding.

#### Unattended node variants

Configured via the companion app at deployment time and advertised in the BLE capability flags:

- **Relay** — receives and forwards content, subject to automated reputation filters. The most common deployment: a community hub that extends network reach.
- **Publisher** — only pushes content outward; does not collect content from peers. Useful for official channels or digital bulletin boards. When a peer encounters a Publisher, it receives the Publisher's manifest but the Publisher does not request the peer's manifest in return.
- **Archive** — receives and stores content above the reputation threshold but does not forward it to other peers. Useful for preservation, journalism archives, or backup nodes.

Unattended mode MUST be advertised in the BLE capability flags (see Section 11.1) so that scanning nodes know not to expect ratings or Forwarding Declarations from this device, and can apply appropriate trust expectations.

### 2.4 Dual Role Operation

Nodes must be capable of operating simultaneously as both GATT Peripheral (advertising, accepting connections) and GATT Central (scanning, initiating connections). This is sometimes called "Observer + Broadcaster" or "Central + Peripheral" multi-role operation.

When two nodes meet, one takes the Central role (initiates connection) and one takes the Peripheral role (accepts). Role selection uses a simple tiebreaker: the device with the lexicographically lower Transport Identity public key becomes Central. Both devices know their own and the peer's public key from the advertisement payload.

---

## 3. BLE GATT Service

### 3.1 Service UUID

All Waxwing nodes advertise the following primary service UUID in their BLE advertisement:

```
Waxwing Service UUID: CE575800-494E-4700-8000-00805F9B34FB
```

The UUID encodes "WX" (0x57, 0x58) and "ING" (0x49, 0x4E, 0x47) as a mnemonic. Nodes MUST include this UUID in their advertisement's service UUID list so that scanning devices (including iOS in background mode) can identify them.

### 3.2 Advertisement Payload

The BLE advertisement MUST include:

| Field | Value |
|---|---|
| Flags | LE General Discoverable, BR/EDR Not Supported |
| Complete 128-bit Service UUIDs | Waxwing Service UUID |
| Complete Local Name | `WX:` + first 8 hex chars of Transport Public Key |
| Service Data (optional) | Protocol version byte + capability flags byte |

Example local name: `WX:A3F7C201`

The short Transport Public Key prefix in the local name allows scanning peers to perform the Central/Peripheral role tiebreaker before connecting.

### 3.3 Characteristics

All characteristics use 128-bit UUIDs with the Waxwing base. See Section 15 for the full UUID registry.

#### 3.3.1 Device Identity (`CE575801-...`)

**Properties:** READ

Returns a CBOR-encoded structure identifying this node:

```
{
  "v": 1,                          // Protocol version (uint)
  "tpk": <bytes>,                  // Transport public key (Ed25519, 32 bytes)
  "name": <string>,                // Human-readable node name (optional, max 32 chars)
  "caps": <uint>,                  // Capability flags (see Section 11.1)
  "manifest_count": <uint>,        // Number of items currently in manifest
  "attended": <bool>,              // true = owner actively reviews content; false = unattended relay/publisher/archive
  "unattended_mode": <string>,     // "relay" | "publisher" | "archive" — present only if attended = false
  "protocol": "waxwing-mesh"
}
```

The `attended` flag is the first thing a scanning node should check. An unattended node will never generate ratings or Forwarding Declarations. A node receiving content from an unattended peer knows to look at the embedded Forwarding Declaration for accountability rather than attributing reputation to the relay itself.

#### 3.3.2 Manifest Chunk (`CE575802-...`)

**Properties:** READ, INDICATE

The manifest is potentially large and is read in chunks. The Central reads this characteristic repeatedly, passing an offset, until it has received the complete manifest. INDICATE is used to notify the Central when the manifest has changed (new content arrived or content expired).

The format of each chunk read:

Request (written to Transfer Control first to set offset):
```
[MANIFEST_READ opcode: 1 byte][offset: 4 bytes LE uint32]
```

Response (read from this characteristic):
```
[total_length: 4 bytes LE uint32][chunk_data: up to MTU-7 bytes]
```

The manifest itself is a CBOR array of file metadata objects (see Section 5). Nodes SHOULD cache a serialised manifest and rebuild it only when content changes.

#### 3.3.3 Transfer Request (`CE575803-...`)

**Properties:** WRITE, WRITE WITHOUT RESPONSE

Written by the Central to request a specific file. Format:

```
[opcode: 1 byte][file_id: 32 bytes][resume_offset: 4 bytes LE uint32]
```

Opcodes:
- `0x01` REQUEST — request a file, optionally from a byte offset (for resume)
- `0x02` CANCEL — cancel an in-progress transfer
- `0x03` MANIFEST_READ — set manifest read offset (see 3.3.2)

#### 3.3.4 Transfer Data (`CE575804-...`)

**Properties:** NOTIFY

Used by the Peripheral to stream file chunks to the Central. Each notification:

```
[file_id: 32 bytes][seq: 4 bytes LE uint32][total: 4 bytes LE uint32][data: remaining bytes]
```

- `file_id` — SHA-256 hash of the complete file content
- `seq` — zero-indexed chunk sequence number
- `total` — total number of chunks for this transfer
- `data` — raw file bytes for this chunk

Chunk size is negotiated via MTU. After MTU negotiation, chunk data length = MTU - 3 (ATT overhead) - 40 (header) bytes. At the default 23-byte MTU this yields very small chunks; implementations MUST negotiate MTU to at least 128 bytes and SHOULD negotiate to 512 bytes (BLE 4.2+) or 251 bytes (BLE 4.0/4.1).

#### 3.3.5 Transfer Control (`CE575805-...`)

**Properties:** WRITE, NOTIFY

Bidirectional control channel. Written by either party.

Format:
```
[opcode: 1 byte][file_id: 32 bytes][payload: variable]
```

Opcodes (Central → Peripheral):
- `0x10` ACK_CHUNK — acknowledge receipt of chunk at seq N: `[seq: 4 bytes]`
- `0x11` NACK_CHUNK — request retransmit of chunk at seq N: `[seq: 4 bytes]`
- `0x12` PAUSE — pause transmission (flow control)
- `0x13` RESUME — resume transmission
- `0x14` COMPLETE — transfer complete, checksum verified

Opcodes (Peripheral → Central, via NOTIFY):
- `0x20` TRANSFER_START — about to begin sending chunks: `[total_size: 4 bytes][total_chunks: 4 bytes]`
- `0x21` TRANSFER_DONE — all chunks sent
- `0x22` ERROR — transfer error: `[error_code: 1 byte]`

#### 3.3.6 Reputation Exchange (`CE575806-...`)

**Properties:** READ, WRITE

Used to exchange two categories of reputation gossip in a single interaction: **creator reputation** (content quality signals) and **transport endorsements** (node trust vouches). Both are included in the same CBOR payload; a receiving node processes whichever categories are relevant to it.

Format (READ response / WRITE payload) — CBOR map:
```
{
  "creator_rep": [           // Creator reputation entries (content quality)
    {
      "cpk": <bytes>,        // Creator public key (32 bytes)
      "score": <int>,        // Aggregate reputation score (signed int)
      "ratings": <uint>,     // Number of ratings contributing to score
      "updated": <uint>      // Unix timestamp of most recent rating
    },
    ...                      // Up to 50 entries, sorted by absolute score descending
  ],

  "endorsements": [          // Transport endorsements (node trust vouches)
                             // Omitted entirely if social.endorsements is disabled on sender
    {
      "endorser_tpk": <bytes>,   // Vouching node's Transport Public Key (32 bytes)
      "endorsed_tpk": <bytes>,   // Vouched-for node's Transport Public Key (32 bytes)
      "timestamp": <uint>,       // Unix timestamp of endorsement creation/update
      "encounter_count": <uint>, // Number of syncs between endorser and endorsed
      "hops_remaining": <uint>,  // Remaining gossip hops (decremented before forwarding; discard at 0)
      "attest_hash": <bytes>,    // SHA-256 of backing SyncAttestation (omitted if social.attestation disabled)
      "endorser_sig": <bytes>    // Ed25519 signature over (endorsed_tpk + timestamp + encounter_count + attest_hash)
    },
    ...                      // Up to 50 entries, prioritised per GAMIFICATION.md §5.3
  ]
}
```

Nodes MUST verify each endorsement's `endorser_sig` before recording it. Invalid signatures are silently discarded. Endorsements with `hops_remaining = 0` are stored locally but MUST NOT be included in outgoing Reputation Exchange payloads.

#### 3.3.7 Rating Submission (`CE575807-...`)

**Properties:** WRITE

Written exclusively by the paired companion app (authenticated via the Device Config characteristic handshake). Submits a rating for a piece of content.

Format — CBOR:
```
{
  "file_id": <bytes>,          // 32-byte SHA-256 file hash
  "creator_pk": <bytes>,       // Creator public key (32 bytes); null for anonymous content
  "action": <int>,             // Review action: +2 Recommend (strong), +1 Recommend, 0 Pass Along, -1 Hold, -2 Reject
  "timestamp": <uint>,         // Unix timestamp of review
  "sig": <bytes>               // Ed25519 signature over (file_id + action + timestamp), signed with node transport key
}
```

Only actions with reputation effect (+2, +1, −2) are gossiped to peer nodes via Reputation Exchange. Hold (−1) and Pass Along (0) are recorded locally for propagation tier assignment but are NOT gossiped — they are private decisions. The signature allows nodes receiving a gossiped rating to verify it originated from a real device that reviewed the file.

#### 3.3.8 WiFi Handoff (`CE575809-...`)

**Properties:** READ, WRITE, NOTIFY

Used during WiFi upgrade negotiation (see Section 11). Format varies by negotiation phase.

#### 3.3.9 Device Config (`CE57580A-...`)

**Properties:** WRITE, INDICATE

Authenticated channel between a node and its paired companion. Access requires a challenge-response handshake using the companion's registered public key. See Section 12.

---

## 4. Identity Model

Waxwing Mesh separates identity into two independent layers. They have different purposes, different lifetimes, and deliberately cannot be linked to each other by design.

### 4.1 Transport Identity

- **Purpose:** Mesh-layer identification, spam filtering, reputation at the device level.
- **Scope:** Tied to a specific node. Changes when the node is replaced.
- **Storage:** Private key stored on the node. Never leaves the node.
- **Algorithm:** Ed25519
- **Public key size:** 32 bytes
- **Usage:** Signs Transfer Control messages; used in reputation ledger for "known spammer nodes."

Transport Identity keypairs are generated on first boot and persisted in non-volatile storage. They MAY be deliberately rotated (generating a new keypair) to shed a negative reputation, analogous to getting a new device.

### 4.2 Content Identity

- **Purpose:** Authorship attribution, creator reputation, subscriptions.
- **Scope:** Tied to a person or pseudonym, not a device.
- **Storage:** Private key is **never required to be stored on any device.** Options:
  - Derived on-demand from a BIP-39 mnemonic (12–24 memorised words)
  - Stored on a hardware security token (e.g., Flipper Zero acting as a signing device)
  - Optionally stored on the companion app (convenience mode; reduced privacy)
- **Algorithm:** Ed25519
- **Public key size:** 32 bytes
- **Usage:** Signs file metadata at creation time. The signature travels with the file permanently. Anyone can verify the signature; no one can determine which device was used.

### 4.3 Content Identity Derivation

When deriving a Content Identity from a mnemonic phrase, implementations MUST use the following derivation:

```
seed = BIP-39 mnemonic → 512-bit seed (PBKDF2-HMAC-SHA512, "mnemonic" + passphrase)
keypair = Ed25519 key from seed[0:32]
```

This is identical to BIP-32 HD wallet seed derivation, allowing users familiar with cryptocurrency wallets to understand and audit the process. The companion app MAY implement a "sign and forget" mode where the mnemonic is entered, the key is derived in memory, used to sign, and then zeroed — the private key never touches persistent storage.

### 4.4 Pseudonymity

A Content Identity public key is a pseudonym. It is:
- Consistent: all content from the same creator shares the same public key
- Verifiable: anyone can check that a file's signature is valid for the claimed key
- Unlinkable: the key cannot be linked to a real person, device, or Transport Identity without additional information

A creator may publish their public key out-of-band (printed QR code, social media, etc.) to allow others to subscribe to or verify their content without the network needing to know who they are.

---

## 5. File Metadata

Every file in the Waxwing Mesh is accompanied by a metadata record. This record is signed by the creator's Content Identity keypair and travels with the file permanently.

### 5.1 Metadata Schema

CBOR-encoded:

```
{
  "id": <bytes>,              // SHA-256 of raw file content (32 bytes) — the canonical file ID
  "v": 1,                     // Metadata schema version

  // Creator fields (signed)
  "cpk": <bytes>,             // Creator public key (32 bytes); null if anonymous
  "csig": <bytes>,            // Ed25519 signature (64 bytes) over signed_payload (see 5.2)
  "timestamp": <uint>,        // Unix timestamp of creation (seconds)
  "mime": <string>,           // MIME type: "text/plain", "audio/mp4", "video/mp4", etc.
  "size": <uint>,             // File size in bytes
  "title": <string>,          // Optional human-readable title (max 128 chars)
  "tags": [<string>],         // Optional content tags, e.g. ["news", "en", "politics"]
  "lang": <string>,           // Optional BCP-47 language code, e.g. "en", "fr"

  // Routing fields
  "routing": <string>,        // "broadcast" or "addressed"
  "recipient": <bytes>,       // Target Content Public Key if addressed; omitted if broadcast

  // Propagation fields
  "ttl": <uint>,              // Remaining hop count (decremented at each forward)
  "origin_tpk": <bytes>,      // Transport public key of the node that first introduced this file to the mesh (32 bytes)

  // Provenance chain (see Section 9.4)
  "fwd_decl": {               // Most recent Forwarding Declaration from an attended node; absent if none yet
    "fwd_tpk": <bytes>,       // Transport public key of the attended node that issued this declaration (32 bytes)
    "action": <int>,          // +2/+1 (Recommend) or 0 (Pass Along) — the human's review decision
    "timestamp": <uint>,      // Unix timestamp when the attended node reviewed and decided
    "sig": <bytes>            // Ed25519 signature over (file_id + fwd_tpk + action + timestamp) (64 bytes)
  }
}
```

### 5.2 Signature Payload

The creator signature `csig` is computed over:

```
signed_payload = CBOR([
  id,          // file content hash
  timestamp,   // creation time
  mime,        // MIME type
  size,        // file size
  title,       // title (empty string if absent)
  tags,        // tags array (empty array if absent)
  routing,     // routing type
  recipient    // recipient key or null
])
```

Any node may verify the signature using the creator's public key `cpk`. Files with invalid signatures MUST NOT be forwarded.

### 5.3 Anonymous Content

If a creator wishes to publish without any attribution, `cpk` is set to `null` and `csig` is omitted. Anonymous content is valid and MUST be accepted and forwarded under the same propagation rules. The file ID alone serves as a deduplication key.

Anonymous content has no creator reputation to track — it propagates as long as the file's own received ratings remain above the block threshold.

### 5.4 File ID and Deduplication

The file ID is the SHA-256 hash of the raw file content (not the metadata). This allows any two nodes to determine they have the same file without transferring it. When two peers compare manifests, files are matched by ID. Metadata updates (e.g., a creator updating a title) result in a new file ID only if the content changes.

---

## 6. Manifest Exchange

When two nodes connect, the Central reads the Peripheral's manifest to discover what content is available. The exchange follows this sequence:

```
Central                                 Peripheral
  |                                         |
  |── READ Device Identity ────────────────>|
  |<── {tpk, caps, manifest_count, ...} ───|
  |                                         |
  |── WRITE Transfer Control               |
  |   [MANIFEST_READ, offset=0] ──────────>|
  |                                         |
  |── READ Manifest Chunk ─────────────────>|
  |<── [total_length, chunk_data...] ───────|
  |   (repeat until all chunks received)    |
  |                                         |
  |  [Central computes diff vs own manifest]|
  |                                         |
  |── WRITE Transfer Request               |
  |   [REQUEST, file_id, resume=0] ────────>|
  |                                         |
  |<── NOTIFY Transfer Control             |
  |    [TRANSFER_START, total, chunks] ─────|
  |                                         |
  |<── NOTIFY Transfer Data                |
  |    [file_id, seq, total, data...] ──────|
  |   (chunks arrive until done)            |
  |                                         |
  |── WRITE Transfer Control               |
  |   [COMPLETE, file_id] ─────────────────>|
  |                                         |
```

### 6.1 Manifest Filtering

Before requesting a file, the Central MUST apply local filters:

1. **Reputation filter:** If `cpk` is not null and the creator's reputation score is below the blocking threshold, skip.
2. **Duplicate filter:** If the file ID already exists in local storage, skip.
3. **Routing filter:** If `routing` is `"addressed"` and `recipient` does not match this node's Transport Public Key or any registered Content Identity, skip (but still forward — see Section 8).
4. **TTL filter:** If `ttl` is 0, skip (do not request or forward).
5. **Storage filter:** If local storage is below a configurable minimum free threshold, skip (or prioritise by subscription/tier).

### 6.2 Bidirectional Exchange

After the Central has finished requesting files from the Peripheral, the roles for content exchange reverse: the Peripheral may also request files from the Central. This is accomplished by the Central exposing its own manifest for reading (all nodes are always simultaneously Peripheral-capable) and the Peripheral initiating read requests.

In practice, both devices read each other's manifests before either starts requesting transfers, then each requests what it needs in parallel where the connection supports it.

---

## 7. File Transfer Protocol

### 7.1 MTU Negotiation

Immediately after connection establishment, both devices MUST initiate MTU negotiation requesting 512 bytes (BLE 4.2+) or the maximum supported by the radio. The effective data payload per chunk is:

```
chunk_data_size = negotiated_mtu - 3 (ATT overhead) - 40 (Waxwing header)
```

At MTU=512: chunk_data_size = 469 bytes
At MTU=251: chunk_data_size = 208 bytes
At MTU=128: chunk_data_size = 85 bytes (minimum acceptable)

### 7.2 Chunked Transfer

Files are divided into fixed-size chunks (fixed per transfer, based on negotiated MTU). Chunks are numbered from 0. The Peripheral sends chunks sequentially via NOTIFY on Transfer Data. The Central acknowledges receipt via ACK_CHUNK on Transfer Control.

A sliding window of up to 4 unacknowledged chunks is permitted to improve throughput. The Peripheral pauses after 4 unacknowledged chunks and waits for an ACK.

### 7.3 Resumable Transfers

If a transfer is interrupted (connection lost, timeout), the Central may resume by writing a Transfer Request with a non-zero `resume_offset` equal to the byte offset of the last successfully received and verified chunk. The Peripheral resumes from that offset.

Interrupted transfers are stored as partial files in a temporary area and completed on subsequent encounters with the same or any other node holding the same file (identified by file ID).

### 7.4 Transfer Integrity

On receipt of the final chunk (seq == total - 1), the Central computes the SHA-256 hash of the reassembled file and compares it to the `id` field in the file metadata. On match, the Central writes COMPLETE to Transfer Control. On mismatch, the Central writes ERROR and discards the partial file.

### 7.5 Transfer Prioritisation

When multiple files are queued for transfer, they are sent in priority order:

1. Files subscribed to by the receiving peer (highest priority)
2. Files rated positively by the sender's owner
3. Files rated neutrally (unrated)
4. Metadata-only exchange (reputation gossip, manifest only)

---

## 8. Propagation Model

Waxwing Mesh uses a **"spread unless stopped"** epidemic routing model with priority tiers. The gate for forwarding is **review** (the owner has opened and seen the content), not a specific rating value. After review, the owner's chosen action determines the priority tier.

### 8.1 Propagation Tiers

Every file in a node's storage is assigned one of five propagation tiers:

| Tier | Condition | Forwarding Behaviour |
|---|---|---|
| **Subscribed** | Creator `cpk` is in owner's subscription list | Forward eagerly at highest priority; always retained in storage |
| **Recommended** | Owner chose Recommend after review (+1 or +2) | Forward at high priority |
| **Passed Along** | Owner explicitly chose Pass Along after review (0) | Forward at low priority; niche content path |
| **Held** | Owner chose Hold — not passing on (−1) | Do not forward; no reputation penalty; content stays locally |
| **Blocked** | Owner chose Reject (−2); OR creator reputation below threshold | Never forward; apply reputation penalty to creator |

**Unreviewed content** (arrived on the node but not yet opened by the owner) is stored locally and queued for review. It is **not forwarded** until the owner has seen it. This is the core spam-resistance property: nothing propagates faster than human review speed.

**The Pass Along action** is a deliberate civic gesture — "I'm not personally interested, but I recognise this may matter to someone else." It is the primary mechanism by which niche content (minority viewpoints, regional topics, specialised subjects) continues to circulate without requiring majority endorsement. The recommendation system actively surfaces the Pass Along option for content that does not match the owner's interests but comes from a non-blocked creator.

**The Held action** gives owners a private veto. They have seen the content and chosen not to forward it, but without penalising the creator. Content held by most owners will slow to a stop naturally; content held by only some will continue through the owners who chose Pass Along or Recommend.

### 8.2 TTL Management

Each file carries a TTL (hop count). The initial TTL is set by the creator (default: 20 hops; max: 64 hops). Each time a node forwards a file to a peer, it decrements the TTL in the copy it sends. Nodes MUST NOT forward files with TTL = 0.

Subscribed content sent by the receiving peer ignores TTL (the local node re-publishes it with its own TTL rather than decrementing).

### 8.3 Review Queue

Unreviewed content is held in a review queue on the node. The companion app surfaces this queue to the owner as a feed — similar to an inbox. The UX should make reviewing quick: a brief preview (title, creator, tags, size) and five clearly labelled actions. The recommendation system may pre-sort the queue to surface content most likely to interest the owner, reducing the cognitive load of reviewing content they'll ultimately Pass Along or Hold.

A node MAY implement a configurable maximum queue depth. When the queue is full, newly arrived unreviewed content is stored but the owner is notified. Storage eviction pressure (see Section 8.4) applies only to reviewed content; unreviewed content is retained until the owner acts on it or the queue overflow policy triggers.

### 8.4 Addressed Content

Files with `routing: "addressed"` are intended for a specific recipient (identified by their Content Identity public key). Intermediate nodes MUST still store and forward addressed content even if they are not the recipient — the network acts as a carrier, not a router. The recipient is identified when the content reaches a node whose companion app has registered that Content Identity.

### 8.5 Storage Pressure

When a node's storage reaches a configurable high-water mark (default: 90% full), it begins evicting files in reverse priority order:

1. Blocked files first
2. Held files, oldest first (by creation timestamp)
3. Passed Along files, oldest first
4. Recommended files, oldest first
5. Subscribed files last (only evicted under extreme pressure)

The node's owner may configure minimum protected storage allocations per tier.

---

## 9. Rating System

### 9.1 Review Actions

After opening and consuming a piece of content, the owner selects one of five actions. These actions determine the propagation tier (Section 8.1) and optionally contribute to creator reputation gossip.

| Action | Value | Tier | Reputation Effect | Propagates As |
|---|---|---|---|---|
| **Recommend** | +2 or +1 | Recommended | Positive contribution | High priority |
| **Pass Along** | 0 | Passed Along | None | Low priority |
| **Hold** | −1 | Held | None | Not forwarded |
| **Reject** | −2 | Blocked | Negative contribution | Never forwarded |

There is no implicit "neutral" action from simply not rating. An unreviewed file has no tier and is not forwarded. The owner must actively open the content and choose an action.

**Pass Along** is designed to be frictionless — a single tap. The companion app UX should make it the default suggested action for content that falls outside the owner's interest profile (as inferred from subscriptions, tags, and prior ratings) but comes from a non-blocked creator. The intent is to lower the barrier to keeping niche content alive in the network.

**Hold** is a private action. It is not gossiped to other nodes and does not affect creator reputation. It simply means: "I have reviewed this and I am not passing it on. I make no judgement about its value to others." This is the appropriate action for content that is simply outside someone's interests, not offensive.

**Reject** is reserved for content the owner considers genuinely harmful, spam, or abusive. It triggers a reputation penalty for the creator and is gossiped to peers so the penalty can propagate through the network.

### 9.2 Rating Propagation

Ratings are not kept private. When a node forwards a file's metadata, it MAY attach a compact rating record:

```
{
  "tpk": <bytes>,       // Transport public key of the rater's node
  "rating": <int>,      // -2 to +2
  "timestamp": <uint>,  // When the rating was submitted
  "sig": <bytes>        // Ed25519 signature over (file_id + rating + timestamp) with tpk private key
}
```

Receiving nodes incorporate these ratings into their local reputation calculations, weighted by the rater's own reputation score. Ratings from nodes with negative reputation are down-weighted; ratings from subscribed or positively-reputed nodes are up-weighted.

### 9.3 Review as the Forwarding Gate

A file is **eligible to be forwarded** only after the owner has reviewed it and chosen an action that permits forwarding (Recommend or Pass Along). Files that have arrived but not yet been reviewed by the owner are stored locally but not forwarded — they wait in the review queue.

This is the core spam-resistance property. No content propagates faster than the speed at which humans review it. A spam campaign that floods many nodes simultaneously must still be reviewed by a human at each node before it can spread further. The first reviewers to Reject it will propagate that reputation signal, and subsequent nodes may see the creator's reputation fall below their blocking threshold before even opening the content.

### 9.4 Forwarding Declarations

When an attended node reviews content and chooses Recommend or Pass Along, it produces a **Forwarding Declaration**: a compact, signed record stating *"I reviewed this content and chose to forward it."* This declaration is embedded in the file metadata (see Section 5.1) and travels with the file through all subsequent relays, including unattended ones.

```
ForwardingDeclaration {
  "fwd_tpk": <bytes>,     // Transport public key of this attended node (32 bytes)
  "action": <int>,        // +2 or +1 (Recommend) or 0 (Pass Along)
  "timestamp": <uint>,    // Unix timestamp of the review decision
  "sig": <bytes>          // Ed25519 signature over CBOR([file_id, fwd_tpk, action, timestamp])
}
```

**Only attended nodes produce Forwarding Declarations.** Unattended nodes relay whatever declaration is already embedded in the content. When an attended node receives content from an unattended relay, the embedded declaration shows which attended human last vouched for it — the relay is invisible in this accountability chain.

**Each Recommend or Pass Along overwrites the previous declaration.** The declaration in a file's metadata always reflects the *most recent* human review decision. A chain of attended nodes each passing the content along produces a series of declarations, but only the latest is embedded; earlier ones are not retained in transit. The full chain exists only on nodes that logged it locally.

This design means accountability is always current: if the most recent forwarder turns out to be untrustworthy, their reputation penalty applies regardless of how many trusted humans vouched for it earlier.

**Hold and Reject do not produce declarations** — the node is not forwarding the content, so there is nothing to sign.

#### Evaluating content from an unattended node

When a node receives content via an unattended relay, it evaluates the embedded `fwd_decl`:

- If `fwd_decl` is present: look up `fwd_tpk` in the local reputation ledger. Apply that attended node's reputation tier to the content. If `fwd_tpk` is unknown, treat as Vouched if the relay itself is vouched, or Unknown otherwise.
- If `fwd_decl` is absent: the content has not yet been reviewed by any attended node. Treat it as lower priority — it has cleared no human filter yet. The owner may still review it, but it sits at the bottom of the queue.

#### Reputation consequences

If a node reviews content from an unattended relay and chooses Reject:
- The rating record is signed and gossiped as normal (targeting the creator's public key for content reputation)
- The `fwd_tpk` in the forwarding declaration receives a separate relay accountability penalty (see Section 10.5) — the attended node that vouched for this content bears responsibility for that endorsement
- The unattended relay node itself receives no content reputation penalty; it is not accountable for human review decisions

---

## 10. Reputation Model

### 10.1 Creator Reputation

Each node maintains a local reputation ledger:

```
{creator_public_key → ReputationRecord}

ReputationRecord {
  score: int,            // Aggregate weighted score
  rating_count: uint,    // Number of ratings contributing
  last_updated: uint,    // Unix timestamp
  blocked: bool          // Manually blocked by owner
}
```

### 10.2 Score Calculation

When a rating for creator C is received from rater R:

```
rater_weight = clamp(reputation_ledger[R.tpk].score, -1.0, 1.0)  // -1 to +1
contribution = rating_value * (1.0 + rater_weight * 0.5)
score[C] += contribution
```

Ratings from a rater with neutral reputation (score = 0) contribute their face value. Ratings from trusted raters (high positive reputation) are amplified up to 1.5x. Ratings from untrusted raters (negative reputation) are down-weighted toward 0.

### 10.3 Blocking Threshold

The default blocking threshold is a score of **-10**. Creators below this threshold have their content silently dropped and never forwarded. The threshold is configurable per node by the owner.

### 10.4 Transport Identity Reputation

Separate from creator reputation, nodes track two transport-layer reputation scores for every known Transport Public Key — one for **attended nodes** (content accountability) and one for **unattended nodes** (relay integrity). The same TPK can only ever be one type; the `attended` flag in Device Identity is fixed at deployment.

**Attended node transport reputation** accumulates from Forwarding Declaration accountability: when an attended node's declaration is embedded in content that later gets Rejected, that node's transport reputation takes a penalty proportional to the rating. When content it declared is Recommended by downstream reviewers, its transport reputation gains. Nodes MAY refuse to accept content whose `fwd_tpk` falls below a configurable minimum transport reputation, even if the content creator's reputation is fine.

**Unattended node relay integrity** is a separate score tracking whether a relay behaves faithfully. See Section 10.5.

### 10.5 Relay Integrity Reputation

Unattended nodes are evaluated on one axis: **do they faithfully relay what they receive?** This includes:

- Preserving `fwd_decl` fields in content metadata without modification
- Forwarding signed rating records and endorsements received from peers
- Not selectively withholding negative reputation updates about their feeder nodes

**What a relay cannot do:** forge, modify, or neutralise any signed data structure. Every rating, endorsement, and Forwarding Declaration is signed by the node that generated it. An unattended relay that attempts to modify these will produce invalid signatures that receiving nodes will detect and discard — and the detection itself is evidence of tampering.

**What a relay can do:** withhold. A relay can simply not forward a rating update about its feeder node. This is the primary relay integrity risk.

**Detecting withholding probabilistically:** When Node A gives a negative rating to attended Node V and that rating is signed, it will travel through many paths in the network. If Node B encounters unattended relay U — which has synced with V — and finds that U has never forwarded A's negative rating about V, while other nodes have (proving the rating exists in the network), B can infer that U is selectively withholding. This is logged as a relay integrity strike against U.

The relay integrity score is initialised at 0 and adjusted as follows:

| Observation | Score change |
|---|---|
| Relay forwarded a rating that matches one independently received via another path | +1 (consistency) |
| Relay demonstrably withheld a known-existing rating | −5 |
| Relay forwarded intact Forwarding Declarations (verified by sampling) | +1 per verified encounter |
| Relay forwarded content with a tampered or invalid signature | −20 (immediate distrust) |

Nodes with relay integrity below a configurable threshold (default: −15) are treated as untrusted relays. Content from them is still accepted but with lowest priority, and their endorsement forwarding is treated as potentially incomplete.

### 10.6 Transitive Trust via Attestation Vouching

The base reputation model operates on direct experience only — a node starts with zero trust and accumulates it through repeated positive encounters. This means a freshly-encountered node is always an unknown quantity, even if it is well-known to other nodes you trust.

The attestation mechanism (see `protocol/GAMIFICATION.md §3`) solves this without any additional signing ceremony. A sync attestation already proves that two specific nodes were physically co-located and completed a protocol handshake. When a trusted peer shares its attestation records during Reputation Exchange, those records constitute a **vouching chain**: physical proximity is hard to fake, and a device that has physically met a node you trust is meaningfully less likely to be a Sybil bot or spam injector than a device with no history at all.

**This feature requires only `social.attestation` to be enabled.** Geolocation (`social.wifi_fingerprint`) is entirely optional. The trust value of an attestation comes from the cryptographic proof of physical co-location, not from knowing *where* that encounter happened.

#### Trust Levels

Each node maintains a trust level for every known Transport Public Key, separate from and in addition to content reputation scores:

| Level | Name | How Acquired | Effect |
|---|---|---|---|
| 0 | **Unknown** | Default for any unseen TPK | Lowest queue priority; content held pending review |
| 1 | **Vouched** | A trusted node holds a verified attestation with this TPK | Slightly elevated queue priority; benefit of the doubt on initial encounter |
| 2 | **Encountered** | This node has directly synced with the TPK at least once | Standard queue priority; content reputation begins accumulating |
| 3 | **Known** | Repeated direct syncs; content consistently rated positively | Elevated queue priority; reputation score amplifies incoming ratings |
| 4 | **Trusted** | Long history of positive encounters; high reputation score | Highest queue priority; this node's vouches for others carry full weight |

A node's trust level is raised by direct positive experience and lowered by direct negative experience. Vouched status (Level 1) can only be conferred by a node at Level 3 or 4 — a Vouched node cannot vouch for further nodes. This prevents trust inflation cascades.

#### Vouching Rules

```
vouch_weight = clamp(voucher_trust_score / TRUST_FULL_THRESHOLD, 0.0, 1.0)
               // e.g., if TRUST_FULL_THRESHOLD = 20 and voucher score = 15, weight = 0.75

vouched_initial_score = BASE_VOUCH_SCORE * vouch_weight
               // BASE_VOUCH_SCORE = +3 (configurable); full-trust voucher grants +3

hop_limit = 1   // Vouching does not chain: Vouched nodes cannot vouch for others
```

A node receiving multiple independent vouches for the same TPK accumulates them, up to a configurable maximum (default: +6, i.e., two full-trust vouches). This prevents a well-connected node from granting unlimited trust to an unknown device simply by being encountered by many trusted peers.

#### How Vouches Are Exchanged

During the Reputation Exchange characteristic read/write, nodes include an optional **attestation summary list**: compact records of their recent attested syncs. Each summary is:

```
AttestationSummary {
  "peer_tpk": <bytes>,         // Transport Public Key of the peer they attested with (32 bytes)
  "timestamp": <uint>,         // Timestamp of the attested sync
  "attest_hash": <bytes>,      // SHA-256 of the full attestation record (16 bytes, truncated)
  "voucher_sig": <bytes>       // Ed25519 signature over (peer_tpk + timestamp + attest_hash)
                               // signed with the sharing node's transport key
}
```

The `voucher_sig` is essential: it proves the sharing node genuinely attested with `peer_tpk`, rather than simply claiming it did. A receiving node verifies the signature before recording the vouch. Without a valid signature, the vouch is discarded.

The full attestation record is not transmitted during routine Reputation Exchange — only the summary. If a node wants to verify the full attestation (e.g., for attestation export or deeper provenance), it may request it separately via the Sync Attestation characteristic.

#### Vouch Decay

Vouched trust decays over time if never reinforced by direct encounter. A Vouched node that remains at Level 1 (never directly synced with the vouching node) has its initial score halved every 90 days. After 180 days without direct encounter, it returns to Unknown (Level 0) and the initial score is zeroed. This prevents stale vouches from accumulating indefinitely.

Vouches reinforced by direct encounter (the node is subsequently synced with) are converted to Encountered status (Level 2) and are no longer subject to decay.

#### Trust Does Not Override Content Reputation

Vouching affects only the **initial trust level** and **queue priority** for an unencountered device. It does not override content reputation scoring. A Vouched device that consistently introduces content that gets Rejected will accumulate a negative content reputation score through the standard mechanism and eventually be blocked regardless of its vouched status.

The two systems are complementary: vouching answers "is this a real, physically-present node?" while content reputation answers "does this node introduce content worth reviewing?"

Full social layer specification, including the community graph visualisation of the trust web: `protocol/GAMIFICATION.md §5`.

### 10.7 Reputation Summary by Node Type

| Reputation Axis | Attended Node | Unattended Node |
|---|---|---|
| Creator reputation | Tracked (content they introduce) | Not tracked |
| Forwarding Declaration accountability | Tracked (vouches they sign) | Not applicable — no declarations |
| Relay integrity | Not tracked (owners review; no relay concerns) | Tracked (primary accountability axis) |
| Trust level (for vouching) | Accumulated from direct encounters | N/A — unattended nodes do not vouch |
| Endorsements generated | Yes, if social.endorsements enabled | No |

---

## 11. WiFi Upgrade Negotiation

### 11.1 Capability Flags

The `caps` field in Device Identity is a bitmask:

| Bit | Capability |
|---|---|
| 0 | WiFi Client (can connect to an AP) |
| 1 | WiFi AP (can create a soft access point) |
| 2 | Multipeer Connectivity (iOS AWDL — iOS devices only) |
| 3 | Local Network (both devices on same infrastructure WiFi) |
| 4 | Unattended Mode — no human owner reviewing content |
| 5 | Unattended Publisher — only pushes content, does not collect |
| 6 | Unattended Archive — collects content but does not forward |

Bits 4, 5, and 6 are mutually exclusive with each other but not with bits 0–3. A Publisher (bit 5) implies Unattended (bit 4). An Archive (bit 6) implies Unattended (bit 4). A Relay sets only bit 4.

Scanning nodes that have configured `sync.unattended_nodes: never` MUST skip any peer with bit 4 set without connecting. This filtering at scan time avoids unnecessary connection overhead.

### 11.2 Upgrade Decision

After manifest exchange, if the total size of pending transfers exceeds a configurable threshold (default: 1 MB) AND both devices have compatible WiFi capabilities, the Central MAY propose a WiFi upgrade.

Compatibility matrix:

| Central caps | Peripheral caps | WiFi mode |
|---|---|---|
| WiFi Client | WiFi AP | Peripheral creates AP; Central connects |
| WiFi AP | WiFi Client | Central creates AP; Peripheral connects |
| Multipeer | Multipeer | iOS Multipeer Connectivity session |
| Local Network | Local Network | Both connect to existing AP; exchange IPs |

### 11.3 Upgrade Protocol

```
Central writes to WiFi Handoff characteristic:
  [PROPOSE opcode: 0x01][preferred_mode: 1 byte][max_speed_mbps: 1 byte]

Peripheral responds via NOTIFY:
  [ACCEPT: 0x02][mode: 1 byte][ap_ssid: variable][ap_password: variable][port: 2 bytes]
  OR
  [DECLINE: 0x03]

If ACCEPT with AP mode:
  Central connects to the specified AP
  Central connects TCP to Peripheral's IP on specified port
  File transfer proceeds over TCP using Waxwing Wire Transfer (see protocol/TRANSFER.md)
  On completion, Central disconnects WiFi and BLE resumes
```

WiFi credentials (SSID + password) are generated randomly per session and transmitted only over the encrypted BLE connection. They MUST NOT be reused across sessions.

### 11.4 Waxwing Wire Transfer

Over WiFi/TCP, files are transferred using a simple framing protocol defined in `protocol/TRANSFER.md`. The same chunking, sequencing, and integrity verification rules apply. TCP eliminates packet loss concerns; chunk size increases to 65535 bytes. Expected throughput: 2–10 MB/s depending on hardware.

---

## 12. Companion App Protocol

### 12.1 Pairing

A companion app pairs with a node via a QR-code or NFC exchange of the node's Transport Public Key. The app generates its own Ed25519 keypair (the "companion keypair") and registers it with the node via Device Config after completing a challenge-response authentication.

Once paired, the node recognises the companion by its public key and grants access to privileged characteristics (Rating Submission, Device Config).

### 12.2 Authentication Challenge

```
App writes to Device Config:
  [AUTH_CHALLENGE_REQUEST: 0x01]

Node indicates on Device Config:
  [AUTH_CHALLENGE: 0x02][nonce: 32 random bytes]

App writes to Device Config:
  [AUTH_RESPONSE: 0x03][companion_pubkey: 32 bytes][sig: Ed25519 signature over nonce]

Node indicates:
  [AUTH_OK: 0x04] or [AUTH_FAIL: 0x05]
```

A session token (randomly generated 16-byte value) is then valid for the duration of the BLE connection and MUST be prepended to all subsequent Device Config writes.

### 12.3 Home WiFi Configuration

The companion app may push home WiFi credentials to the node via Device Config (after authentication). The node stores these credentials and automatically connects to the home network when in range, then notifies the companion of its IP address via NOTIFY on Device Config:

```
[WIFI_CONNECTED: 0x10][ip_address: 4 bytes IPv4 or 16 bytes IPv6][port: 2 bytes]
```

The companion app then connects to the node over the local network for fast content access, using the same Waxwing Wire Transfer protocol as the WiFi upgrade path.

---

## 13. Subscription System

### 13.1 Subscriptions

A subscription is a Content Identity public key in the node's subscription list. Subscribed content is:
- Fetched at highest priority (Subscribed tier)
- Never evicted from storage under normal pressure
- Not subject to reputation filtering (the owner has explicitly opted in)

Subscriptions are configured via the companion app and stored on the node.

### 13.2 Interest Tags

In addition to explicit subscriptions, nodes maintain a weighted interest tag list:

```
{"news": 1.0, "audio": 0.8, "en": 1.0, "tech": 0.6, ...}
```

When storage pressure requires prioritising which unsubscribed Neutral-tier content to fetch, files whose tags overlap with the interest profile score higher. This is the first layer of the recommendation system.

### 13.3 Social Recommendations (Future)

Phase 2 of the recommendation system uses the rating gossip that travels with content. As ratings from other nodes accumulate, a node can observe which creators and tags correlate with content that well-reputed peers rated positively. This correlation becomes a soft subscription signal.

Phase 3 adds local collaborative filtering: a simple preference model built from the owner's own rating history, used to predict ratings for unseen content and adjust fetch priority accordingly.

Neither phase requires a server or any communication outside the mesh.

---

## 14. Unattended Mode

### 14.1 Overview

An unattended node suspends the human-review gate and replaces it with automated reputation filtering. This is the one explicit exception to the rule that content is only forwarded after a human has reviewed it. The trade-off is deliberate: unattended nodes extend the network's reach and enable permanent deployment, but they introduce risk of spam amplification if misconfigured or if their transport reputation is not managed carefully.

Unattended mode is configured via the companion app and stored in node configuration on the device. The mode MUST be reflected in the BLE advertisement capability flags (Section 11.1) so peer nodes can make informed decisions before connecting.

### 14.2 Automated Content Filtering

In place of human review, an unattended node applies a configurable filter stack to all incoming content. Each filter is evaluated in order; the first matching filter determines the content's fate.

| Priority | Filter | Condition | Action |
|---|---|---|---|
| 1 | Creator blocklist | Creator `cpk` is explicitly blocked by this node | **Block** — discard, apply reputation penalty |
| 2 | Transport blocklist | Offering peer's transport reputation is below block threshold | **Refuse manifest** — disconnect |
| 3 | Creator reputation | Creator reputation score < `filter.min_creator_score` | **Hold** — store locally, do not forward |
| 4 | Content age | File `timestamp` is older than `filter.max_age_days` | **Hold** |
| 5 | TTL floor | File `ttl` < `filter.min_ttl` | **Hold** |
| 6 | Tag filter | File tags do not match `filter.required_tags` (if configured) | **Hold** |
| 7 | Storage ceiling | Free storage below `filter.min_free_bytes` | **Hold** — apply storage eviction first |
| 8 | Default | All filters passed | **Pass Along** — store and forward (Relay) or store only (Archive) |

**Default filter thresholds:**

| Parameter | Default | Description |
|---|---|---|
| `filter.min_creator_score` | `0` | Neutral or better; blocks content from creators with net-negative reputation |
| `filter.max_age_days` | `30` | Do not relay old content |
| `filter.min_ttl` | `1` | Do not relay content on its last hop |
| `filter.required_tags` | `[]` | Empty = accept any tags |
| `filter.min_free_bytes` | `10 MB` | Reserve a safety margin on storage |

All thresholds are configurable by the node operator via the companion app. Operators who set `filter.min_creator_score` very low (accepting most content) accept greater risk of forwarding content that downstream humans will reject, which will penalise the node's transport reputation.

### 14.3 No Positive Ratings Generated

Unattended nodes do not generate Recommend actions. They have no owner to assess quality. They MAY generate automatic Reject actions in one circumstance: if a creator's reputation drops below the block threshold *after* the node has already stored content from that creator, the node retroactively downgrades that content to Blocked tier and updates its rating record to signal the rejection downstream. This prevents an unattended node from continuing to forward content from a creator who has since been widely rejected.

### 14.4 Transport Reputation Accountability

An unattended node's transport reputation is the network's primary mechanism for holding it accountable. The feedback loop operates as follows:

1. Unattended node auto-forwards content from creator C (creator score ≥ threshold)
2. Downstream human nodes review the content and some Reject it
3. Rejections reduce creator C's reputation score in the downstream nodes' ledgers
4. Downstream nodes gossip the reduced score back through the network
5. Eventually the unattended node receives the gossip and creator C's score falls below threshold
6. Future content from C is now auto-Held; the unattended node stops forwarding it
7. Simultaneously, the unattended node's own transport reputation is penalised — it forwarded content that was widely rejected

If an unattended node's transport reputation falls below the peer blocking threshold, other nodes will refuse to sync with it entirely. The node is effectively quarantined from the network until its operator intervenes via the companion app, inspects what went wrong, raises the reputation thresholds, and manually resets the transport identity (accepting a fresh start with no reputation, positive or negative).

This creates a natural incentive gradient: operators who want their unattended nodes to remain useful members of the network are motivated to set conservative thresholds and monitor their node's reputation periodically.

### 14.5 Peer Opt-Out and Preference Settings

Other nodes control how they interact with unattended nodes via a configuration parameter:

| Setting | `sync.unattended_nodes` | Behaviour |
|---|---|---|
| `always` | Accept all unattended nodes whose transport reputation is above the peer block threshold |
| `trusted_only` | **(default)** Only sync with unattended nodes whose transport reputation is in the Good or Trusted tier (score > 5) |
| `never` | Never sync with unattended nodes; filter at scan time using capability flags |
| `ask` | Companion app prompts the owner when an unattended node is nearby |

The default `trusted_only` provides a reasonable balance: new, unknown unattended nodes (neutral reputation, score = 0) are skipped until they have established a positive track record. Nodes run by known, trusted operators will accumulate reputation quickly and become accessible to all peers using the default setting.

Nodes that have opted out of syncing with unattended nodes entirely (`never`) lose no content — any content an unattended node holds will also be held by the human-operated nodes that contributed it. The opt-out affects reach and convenience, not access.

### 14.6 Unattended Node in the Community Graph

Unattended nodes appear in the community graph with a distinct visual indicator (a different node shape — a square or diamond rather than a circle) so they are immediately distinguishable from human-operated nodes. Their reputation tier colour coding is the same. Tapping an unattended node in the graph shows its variant (Relay / Publisher / Archive), its configured filter thresholds (if shared in its Device Identity), and its current transport reputation tier.

Unattended nodes are excluded from the Leaderboard by default, as competing against automated nodes is not meaningful for human participants. They appear in a separate "Infrastructure Nodes" section of the Statistics screen, showing their contribution to network reach and bytes relayed.

### 14.7 Publisher Node Behaviour

A Publisher node (capability bit 5) is a specialised unattended node that only distributes content it has been loaded with — it does not collect content from peers. When a peer connects to a Publisher:

1. The peer reads the Publisher's manifest normally and may request files
2. The Publisher does NOT request the peer's manifest
3. The connection proceeds as a one-way transfer: Publisher → Peer

Publishers are loaded with content via their paired companion app, which pushes files directly. A Publisher's content is typically signed with a specific Content Identity (a channel, organisation, or creator) so subscribers can follow it by public key.

Use cases: official announcements, curated news feeds, event information kiosks, community notice boards.

### 14.8 Archive Node Behaviour

An Archive node (capability bit 6) collects content above its reputation threshold but does not forward it to other peers. It is a storage endpoint, not a relay. Archives are useful for:
- Journalists preserving source material
- Community libraries of mesh content
- Research nodes collecting network data

Archive nodes do participate in manifest exchange normally — they advertise their holdings, and peers can request files from them. The difference is that the Archive will not in turn forward content it receives to subsequent peers.

---

## 15. Security Considerations

> *Note: sections 15–18 were previously numbered 14–17.*

### 14.1 Threat Model

Waxwing Mesh is designed to operate in adversarial environments. The expected threats include:

- **Surveillance:** Authorities monitoring BLE advertisements to identify device owners.
- **Seizure:** Physical confiscation of nodes or phones.
- **Spam:** Bad actors flooding the network with unwanted content.
- **Impersonation:** Claiming false authorship of content.
- **Sybil attack:** Creating many node identities to inflate reputation scores.
- **Unattended node abuse:** Deploying unattended relay nodes with permissive thresholds to amplify spam at machine speed.

### 14.2 Mitigations

**Against surveillance via BLE:**
Transport Identity public keys in advertisements are pseudonymous and rotatable. The advertisement payload contains no personally identifying information. Nodes SHOULD periodically rotate their BLE MAC address (standard BLE privacy feature) in addition to allowing Transport Identity rotation.

**Against seizure of nodes:**
Content Identity private keys need not reside on any device. Creator identity is unlinkable from device identity. Seizing a node yields: a list of content it has stored, and the transport identity of the node — but not the identity of its owner, not the identity of any content creator, and not the content of encrypted addressed messages.

**Against spam:**
Rating-gated forwarding and the reputation system combine to limit spam propagation. A spammer's content will be rated negatively by early recipients, their creator reputation will drop, and within a small number of hops their content will be blocked network-wide.

**Against impersonation:**
All content is signed with the creator's Content Identity keypair. Signature verification is mandatory before forwarding. Content with invalid or missing signatures (for non-anonymous content) MUST be rejected.

**Against Sybil attacks on reputation:**
Reputation scores from unknown nodes are treated with low initial trust. Reputation weighting is proportional to the rater's own accumulated reputation. Creating many new nodes does not yield proportionally more influence — new nodes start at zero reputation.

**Against unattended node abuse:**
Unattended nodes are explicitly flagged in their BLE advertisement, allowing peers to apply different trust rules or opt out entirely. The default peer setting (`sync.unattended_nodes: trusted_only`) means new unattended nodes with zero reputation are skipped until they build a positive track record. An unattended node used to amplify spam will accumulate negative transport reputation from downstream rejections and will eventually be quarantined by the network. The operator must physically intervene to reset it, raising the cost of this attack vector above casual use.

### 14.3 Limitations

- BLE itself is not encrypted at the Waxwing layer; it relies on BLE's built-in link-layer encryption when pairing is used. Mesh connections (node-to-node) use unauthenticated BLE connections; file content and metadata are not additionally encrypted in v0.1.
- Addressed content (for a specific recipient) does not provide end-to-end encryption in v0.1. End-to-end encryption for addressed content is planned for v0.2.
- The reputation system cannot prevent determined Sybil attacks at scale; it merely raises the cost.

---

## 16. UUID Registry

All Waxwing UUIDs use the base suffix `-494E-4700-8000-00805F9B34FB` (encoding "ING\0" as an ASCII mnemonic for "Waxwing").

| Name | UUID | Properties |
|---|---|---|
| Waxwing Service | `CE575800-494E-4700-8000-00805F9B34FB` | — |
| Device Identity | `CE575801-494E-4700-8000-00805F9B34FB` | READ |
| Manifest Chunk | `CE575802-494E-4700-8000-00805F9B34FB` | READ, INDICATE |
| Transfer Request | `CE575803-494E-4700-8000-00805F9B34FB` | WRITE, WRITE_NR |
| Transfer Data | `CE575804-494E-4700-8000-00805F9B34FB` | NOTIFY |
| Transfer Control | `CE575805-494E-4700-8000-00805F9B34FB` | WRITE, NOTIFY |
| Reputation Exchange | `CE575806-494E-4700-8000-00805F9B34FB` | READ, WRITE |
| Rating Submission | `CE575807-494E-4700-8000-00805F9B34FB` | WRITE |
| WiFi Handoff | `CE575809-494E-4700-8000-00805F9B34FB` | READ, WRITE, NOTIFY |
| Device Config | `CE57580A-494E-4700-8000-00805F9B34FB` | WRITE, INDICATE |
| Sync Attestation | `CE57580B-494E-4700-8000-00805F9B34FB` | WRITE, NOTIFY (social opt-in) |
| Encounter Ledger | `CE57580C-494E-4700-8000-00805F9B34FB` | READ auth (social opt-in) |
| Endorsement Exchange | `CE57580D-494E-4700-8000-00805F9B34FB` | READ, WRITE (social opt-in) |

---

## 17. Encoding

All structured data in Waxwing Mesh characteristics is encoded using **CBOR** (Concise Binary Object Representation, RFC 8949). CBOR is compact, self-describing, and has good library support across all target platforms (MicroPython, C, Dart, Swift, Kotlin).

Where binary precision matters (public keys, signatures, file IDs), values are encoded as CBOR byte strings (`bstr`), not base64 text strings.

Integer fields use the smallest CBOR encoding that fits the value (CBOR's natural behaviour).

Boolean fields use CBOR `true`/`false`.

Optional absent fields are omitted from the map entirely (not encoded as `null`) to save space.

The maximum size of any single characteristic read or write is bounded by the BLE ATT MTU. Multi-chunk reads (e.g., manifest) use the offset mechanism described in Section 3.3.2.

---

---

## 18. Social Layer

The optional social and gamification layer is specified separately in `protocol/GAMIFICATION.md`. It covers:

- **Encounter Ledger** — per-peer record of bytes exchanged, file counts, and content quality signals
- **Sync Attestation** — mutually-signed cryptographic proof of physical co-location and data exchange
- **WiFi Geolocation Fingerprinting** — AP scanning for location inference without GPS
- **Companion App Social Screens** — sync map, network graph, peer detail, statistics, attestation export
- **Privacy Opt-Out** — per-feature flags, all disabled by default

All social features are additive and backward-compatible. Nodes with social features disabled are fully interoperable with nodes that have them enabled.

Two additional characteristics support the social layer (see `GAMIFICATION.md §7`):
- Sync Attestation: `CE57580B-494E-4700-8000-00805F9B34FB`
- Encounter Ledger: `CE57580C-494E-4700-8000-00805F9B34FB`

---

*End of Waxwing Mesh Protocol Specification v0.1*

*This is a living document. Breaking changes will increment the major version. Additive changes will increment the minor version. All implementations must advertise the protocol version they implement in the Device Identity characteristic.*
