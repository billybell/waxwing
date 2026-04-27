# Waxwing Mesh — Pico W Firmware

**Status:** Phase 2 implemented — BLE advertising, Device Identity, file commands (ls, read, write, delete, storage_info), FAT filesystem, CBOR codec, Ed25519 transport identity via monocypher.

## Hardware

- Raspberry Pi Pico W
- MicroSD card module (SPI) — for file storage
- LiPo battery + charging board (for portable use)

## Software Stack

- C firmware using Raspberry Pi Pico SDK
- FatFS filesystem (via thirdparty/fatfs)
- Monocypher Ed25519 crypto (via thirdparty/monocypher)
- Minimal CBOR encoder/decoder (core/cborencode.c, core/cbor_decode.c)
- BLE stack via pico_btstack_cyw43
- Hardware RNG and SHA-256 via Pico SDK

## BLE Roles

The Pico W firmware operates as a **GATT Peripheral** — advertising the Waxwing service UUID and responding to Device Identity, File Command (WRITE), and File Response (READ+NOTIFY) characteristics.

The CYW43439 chip (BLE 4.2) supports multi-role operation; future work may add GATT Central for peer discovery.

## Quick Start

### Prerequisites

- Arm GNU Toolchain for bare‑arm (e.g., ArmGNUToolchain 14.3)
- Pico SDK installed and `PICO_SDK_PATH` environment variable set
- CMake (≥3.13)

### Build

```bash
# From the repository root
mkdir -p build-host
cd build-host
cmake .. -DWAXWING_HOST_BUILD=ON   # build host unit tests
cmake --build . --target waxwing_test
./waxwing_test                     # run tests (or: ctest --output-on-failure)

# Build Pico W firmware (uf2)
cd ..
cmake -S . -B build -DWAXWING_HOST_BUILD=OFF
cmake --build build
# Output: build/waxwing_mesh.uf2
```

You can also use the provided `build.sh` script:

```bash
bash firmware/pico-w/build.sh   # builds firmware/pico-w/build/waxwing_mesh.uf2
```

### Deploy

Drag the generated `waxwing_mesh.uf2` onto the Pico W (BOOTSEL mode) or use `picotool`.

### Monitor

```bash
# Optional: view serial output (115200 baud)
screen /dev/ttyACM0 115200
```

You should see boot messages, BLE advertising, etc.

## File Structure

```
firmware/pico-w/
├── build.sh                  # Build script (see above)
├── src/
│   ├── core/                 # Platform‑independent business logic
│   │   ├── cborencode.c/.h
│   │   ├── cbor_decode.c/.h
│   │   ├── commands.c/.h     # Command dispatch over CBOR (ls, read, write, etc.)
│   │   ├── constants.h       # UUIDs, capability flags, protocol constants
│   │   ├── filestore.h       # File storage interface
│   │   └── identity.c/.h     # Transport identity (Ed25519) persistence
│   ├── hw/pico-w/            # Pico W hardware‑specific wiring
│   │   ├── ble.c/.h          # BLE GATT server (advertising, characteristics)
│   │   ├── diskio.c/.h       # RP2040 QSPI flash‑sector remapping for FatFS
│   │   ├── filestore_fatfs.c # FatFS‑on‑QSPI implementation of filestore.h
│   │   ├── hal_crypto_pico.c # Crypto glue using monocypher + hardware RNG
│   │   ├── lwipopts.h        # lwIP config for CYW43 driver
│   │   ├── main.c            # Entry point: init, BLE poll loop, LED
│   │   ├── btstack_config.h  # btstack configuration
│   │   └── waxwing_ble.gatt  # GATT definition (compiled to ATT server)
│   └── thirdparty/           # Vendored dependencies
│       ├── fatfs/            # FAT filesystem (custom ffconf.h)
│       └── monocypher/       # Ed25519 crypto library
└── tests/                    # Host‑only test code
    ├── test.h                # Simple test framework
    ├── test_runner.c
    ├── test_commands.c       # Tests for commands.c
    ├── test_identity.c
    ├── mock_filestore.c/.h   # RAM‑backed filestore for tests
    ├── stub_ble.c            # BLE MTU stub for host tests
    └── stub_hal_crypto.c     # Crypto stub for host tests
```

## LED Patterns

| Pattern | Meaning |
|---|---|
| Slow blink (1 s on / 1 s off) | Advertising, waiting for connection |
| Fast blink (100 ms on / 100 ms off) | Peer connected |
| 3 rapid flashes | Fatal error — check serial output |

## Implementation Phases

### Phase 1 ✅ — Identity + Advertisement
- [x] BLE GATT server — advertise Waxwing service UUID, register all 12 characteristics
- [x] Transport Identity — generate Ed25519 keypair (hardware RNG + monocypher), persist to flash
- [x] Device Identity characteristic — CBOR‑encoded identity payload (includes firmware version)
- [x] Connect / disconnect handling — restarts advertising after disconnect
- [x] LED heartbeat — slow/fast blink indicating state
- [x] Deploy script — `mpremote` sync + soft reset (legacy) / `build.sh` (C)

### Phase 2 ✅ — Manifest + Storage (implemented)
- [x] SD card driver — mount FAT filesystem, read/write files
- [x] File listing (`ls`) with pagination based on BLE MTU
- [x] File read (full and chunked) — respects MTU, supports resumable transfers
- [x] File write (full and chunked) — open/append/close protocol
- [x] File deletion
- [x] Storage info (`storage_info`) — free/used bytes, file count
- [x] Manifest generation — not yet implemented (planned for Phase 3)
- [x] Real Ed25519 keypair — replaced SHA‑256 placeholder with proper crypto

### Phase 3 🔲 — File Transfer (in progress)
- [ ] Chunked BLE transfer — NOTIFY sender, sliding window ACK
- [ ] Propagation logic — tier assignment, TTL, eviction
- [ ] Reputation ledger — CBOR store on SD card

### Phase 4 🔲 — WiFi (planned)
- [ ] Home network auto‑connect, IP advertisement via BLE
- [ ] WiFi Wire Transfer — TCP server for payloads > 1 MB

## Crypto Note

The firmware now uses a **real Ed25519 keypair** for transport identity:
- Private key generated from the hardware RNG (via `pico_rng_get_random`) and stored in flash.
- Public key derived with monocypher; signing/verification also uses monocypher.
- The identity file format (32‑byte private + 32‑byte public) matches the layout expected by the protocol and companion app.
- No migration is needed; the existing identity file on flash will be regenerated on first boot with the new algorithm.
