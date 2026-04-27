# Waxwing Pico W C Firmware — Implementation Status

## Current Status

**Working:**
- ✅ C firmware builds successfully (GCC 14.3 toolchain, Pico SDK)
- ✅ Identity load/generate with flash persistence (Ed25519 via hardware RNG + monocypher)
- ✅ Device identity bytes-to-hex and TPK base64url encoding
- ✅ USB serial output (using `tud_connected()` check before printing)
- ✅ LED heartbeat (slow blink advertising, fast blink connected)
- ✅ BLE GATT server (advertising, connections, read/write/notifies) — Device Identity (READ), File Command (WRITE), File Response (READ+NOTIFY)
- ✅ File operations (list, read, write, delete, storage_info) — FAT filesystem, CBOR‑encoded requests/responses, MTU‑aware pagination
- ✅ CBOR encoding/decoding (core/cborencode.c, core/cbor_decode.c)
- ✅ Connection handling (on_connect, on_disconnect) — restart advertising, clear state
- ✅ Error handling and LED patterns (basic)
- ✅ Crypto implementation — Ed25519 transport identity using monocypher + hardware RNG (hal_crypto.h / hal_crypto_pico.c)
- ✅ Host unit tests — `./build-host/waxwing_test` passes (build with `-DWAXWING_HOST_BUILD=ON`)

**Known Issues:**
- ⚠️ USB serial requires waiting for `tud_connected()` before printing (USB takes ~1-2s to enumerate) — still relevant for debug output
- ⚠️ **Never use `sleep_ms()` in main loop** — blocks BLE stack; use timestamp‑based timing with `to_ms_since_boot(get_absolute_time())`

**Not Implemented / In Progress:**
- ⏳ Manifest generation — CBOR manifest of stored files (planned for next phase)
- ⏳ WiFi upgrade — automatic home‑network connect and IP advertisement via BLE; TCP fallback for large transfers
- ⏳ Reputation gossip — exchange of creator ratings and transport endorsements via Reputation Exchange characteristic
- ⏳ Social layer (opt‑in) — encounter ledger, sync attestation, geolocation fingerprinting (see protocol/GAMIFICATION.md)
- ⏳ Flipper Zero and CardPuter ports — legacy code; slated for removal/replacement

## Next Steps

1. **Manifest Generation** – implement a function that walks the FAT filesystem, computes SHA‑256 for each file, builds a CBOR array of file metadata, and serves it via a new Manifest Characteristic (or reuse File Response with a specific command).
2. **WiFi Upgrade** – implement Wi‑Fi station mode, DHCP, and automatic reconnection to known SSIDs; advertise IP via BLE Device Config characteristic; implement a simple TCP server (Waxwing Wire Transfer) for payloads >1 MB.
3. **Reputation Gossip** – add the Reputation Exchange characteristic (READ/WRITE) and implement the CBOR gossip format defined in the protocol spec.
4. **Social Layer (Opt‑in)** – add optional characteristics for Sync Attestation, Encounter Ledger, and Endorsement Exchange; implement Wi‑Fi geolocation fingerprinting if desired.
5. **Cleanup** – remove the legacy directory (`firmware/pico-w/waxwing/`) and any associated build scripts.
6. **Documentation** – update READMEs and protocol docs as features land.

## Build & Test

To build the firmware:
```bash
bash firmware/pico-w/build.sh   # produces build/waxwing_mesh.uf2
```

To run host unit tests:
```bash
mkdir -p build-host
cd build-host
cmake .. -DWAXWING_HOST_BUILD=ON
cmake --build . --target waxwing_test
./waxwing_test   # or: ctest --test-dir build-host --output-on-failure
```

## Flashing

Drag the generated `waxwing_mesh.uf2` onto the Pico W (BOOTSEL mode) or use `picotool`.

## References

- Protocol specification: `PROTOCOL.md`
- Social layer (opt‑in): `protocol/GAMIFICATION.md`
- Hardware abstraction plan: `HAL_PLAN.md`