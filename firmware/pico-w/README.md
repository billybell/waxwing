# Waxwing Mesh — Pico W Firmware

**Status:** Phase 1 complete — BLE advertising + Device Identity characteristic.

## Hardware

- Raspberry Pi Pico W
- MicroSD card module (SPI) — planned Phase 2
- LiPo battery + charging board (for portable use)

## Software Stack

- MicroPython (RP2040 build with CYW43439 BLE support)
- `bluetooth` module — BLE GATT peripheral
- `network` module — WiFi (Phase 3+)
- `sdcard` / `uos` — SD card storage (Phase 2)

## BLE Roles

The Pico W firmware operates as a **GATT Peripheral** in Phase 1 — advertising
the Waxwing service UUID and responding to Device Identity reads.  Phase 2 adds
**GATT Central** (scan + connect to peers) for full mesh participation.

The CYW43439 chip (BLE 4.2) supports multi-role operation.

## Quick Start

### Prerequisites

```bash
pip install mpremote
```

Flash MicroPython to your Pico W first:
https://micropython.org/download/RPI_PICO_W/

### Deploy

```bash
cd firmware/pico-w
./deploy.sh                    # auto-detect Pico W
./deploy.sh /dev/cu.usbmodem* # macOS explicit port
./deploy.sh /dev/ttyACM0      # Linux explicit port
```

### Monitor

```bash
mpremote repl
```

You should see:

```
=== Waxwing Mesh Firmware ===
Loading identity...
[identity] Generating new Transport Identity ...
[identity] Saved to /waxwing_identity.bin
[ble] GATT service registered (12 characteristics)
[ble] Device Identity updated (158 bytes)
[ble] Advertising as WX:AABBCCDD
[main] Ready — entering main loop
```

### Test with ble_scanner

From the repo root on your Mac/Linux machine:

```bash
python tools/ble_scanner.py          # scan + print matching nodes
python tools/ble_scanner.py --connect # connect and read Device Identity
```

## File Structure

```
firmware/pico-w/
├── main.py              # Entry point, LED heartbeat, main loop
├── deploy.sh            # mpremote sync + reset script
└── waxwing/
    ├── __init__.py
    ├── constants.py     # UUIDs, capability flags, opcodes
    ├── cbor.py          # Minimal CBOR encoder/decoder (no cbor2 needed)
    ├── identity.py      # Transport Identity: generate, persist, load
    ├── messages.py      # Device Identity CBOR builder/parser
    └── ble.py           # BLE advertising + GATT server (WaxwingBLE class)
```

## LED Patterns

| Pattern | Meaning |
|---------|---------|
| Slow blink (1 s on / 1 s off) | Advertising, waiting for connection |
| Fast blink (100 ms on / 100 ms off) | Peer connected |
| 3 rapid flashes | Fatal error — check serial output |

## Implementation Phases

### Phase 1 ✅ — Identity + Advertisement
- [x] BLE GATT server — advertise Waxwing service UUID, register all 12 characteristics
- [x] Transport Identity — generate SHA-256 placeholder keypair on first boot, persist to flash
- [x] Device Identity characteristic — CBOR-encoded identity payload
- [x] Connect / disconnect handling — restarts advertising after disconnect
- [x] LED heartbeat — slow/fast blink indicating state
- [x] Deploy script — `mpremote` sync + soft reset

### Phase 2 — Manifest + Storage (planned)
- [ ] SD card driver — mount, read/write files
- [ ] Manifest generation — CBOR manifest from SD card contents
- [ ] Manifest characteristic — chunked read protocol
- [ ] Real Ed25519 keypair — replace SHA-256 placeholder
- [ ] BLE Central — scan for peers, read their manifests

### Phase 3 — File Transfer (planned)
- [ ] Chunked BLE transfer — NOTIFY sender, sliding window ACK
- [ ] Propagation logic — tier assignment, TTL, eviction
- [ ] Reputation ledger — CBOR store on SD card

### Phase 4 — WiFi (planned)
- [ ] Home network auto-connect, IP advertisement via BLE
- [ ] WiFi Wire Transfer — TCP server for payloads > 1 MB

## Crypto Note

Phase 1 uses `SHA-256(private_key)` as a placeholder for the Transport Public
Key.  This is **not** a real Ed25519 keypair and provides no security.  Phase 2
will replace this with a proper Ed25519 implementation.  The identity file
format (32-byte private + 32-byte public) is compatible with the real keypair
layout, so no migration is needed.

## See Also

- `../../PROTOCOL.md` — full protocol specification
- `../../protocol/GAMIFICATION.md` — social layer spec
