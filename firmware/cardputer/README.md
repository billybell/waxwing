# Waxwing Mesh — CardPuter ADV Firmware

**Status:** Hello-world bring-up complete. Toolchain verified on hardware.

## Hardware

- M5Stack CardPuter ADV (ESP32-S3, 8 MB flash, OPI PSRAM)
- Built-in keyboard and small display
- microSD card slot

## Toolchain

PlatformIO + Arduino framework, using the `m5stack/M5Cardputer` library. Install PlatformIO with `brew install platformio`.

```bash
cd firmware/cardputer
bash build.sh                # build
bash build.sh -t upload      # build + flash over USB-C
pio device monitor           # serial monitor @ 115200
```

If the bootloader isn't detected, hold **G0** while pressing **Reset** and re-run upload.

## Notes

The ESP32-S3 provides BLE 5.0 and WiFi. The built-in display makes the CardPuter useful for showing mesh status, transfer progress, and simple content preview. The keyboard could support basic content creation (short text notes) directly on the device.

## Implementation Plan

0. [x] Hello-world bring-up (PlatformIO + display)
1. [x] BLE GATT server
2. [x] SD card file storage (single-shot + chunked, /files + /system, .meta sidecars)
3. [x] Transport Identity (Ed25519 via monocypher, persisted on SD `/system/identity.bin`)
2. [ ] SD card file storage
3. [ ] Transport Identity keypair (stored in NVS flash)
4. [ ] Manifest exchange
5. [ ] File transfer
6. [ ] WiFi home network + AP mode for WiFi upgrade
7. [ ] Display: mesh status, transfer progress, content list
8. [ ] Optional: keyboard-based text note creation

## See Also

- `../../PROTOCOL.md` — full protocol specification
