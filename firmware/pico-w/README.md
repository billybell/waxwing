# Waxwing Mesh — Pico W Firmware

**Status:** Not yet implemented. This is the reference node implementation.

## Hardware

- Raspberry Pi Pico W
- MicroSD card module (SPI)
- LiPo battery + charging board (for portable use)

## Software Stack

- MicroPython (recommended for initial implementation; C SDK for production)
- `bluetooth` module for BLE GATT server/client
- `network` module for WiFi
- `uos` / `sdcard` module for SD card storage

## BLE Roles

The Pico W firmware must operate simultaneously as:
- **GATT Peripheral** — advertising the Waxwing service UUID, accepting connections from other nodes and companion apps
- **GATT Central** — scanning for other Waxwing nodes and initiating connections

The CYW43439 chip (BLE 4.2) supports multi-role operation.

## Implementation Plan

1. [ ] BLE GATT server — advertise Waxwing service, expose all characteristics
2. [ ] SD card storage — file storage, manifest cache, reputation ledger
3. [ ] Transport Identity — generate Ed25519 keypair on first boot, persist to flash
4. [ ] Manifest generation — build CBOR manifest from SD card contents
5. [ ] File transfer — chunked NOTIFY sender (Peripheral role)
6. [ ] BLE Central — scan for peers, connect, read manifest, request files
7. [ ] Propagation logic — tier assignment, TTL management, eviction
8. [ ] Reputation ledger — CBOR store on SD card
9. [ ] WiFi — home network auto-connect, IP advertisement via BLE
10. [ ] WiFi transfer — TCP server for Waxwing Wire Transfer

## See Also

- `../../PROTOCOL.md` — full protocol specification
- `../../protocol/TRANSFER.md` — wire transfer details
