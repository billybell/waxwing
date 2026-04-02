# Waxwing Mesh — Flipper Zero App

**Status:** Not yet implemented. Second firmware target after Pico W.

## Hardware

- Flipper Zero (nRF52840 co-processor handles BLE)
- microSD card (built-in slot)

## Notes

The Flipper Zero has built-in SD card storage, making it convenient as a portable node. The nRF52840 provides BLE 5.0 support.

Flipper apps are written in C using the Flipper SDK. The app will need to implement the GATT server on the nRF52840 co-processor.

Storage is limited compared to a Pico W + large SD card, so eviction policy tuning will be important for the Flipper target.

## Implementation Plan

1. [ ] BLE GATT server on nRF52840
2. [ ] SD card file storage
3. [ ] Transport Identity keypair
4. [ ] Manifest exchange
5. [ ] File transfer (Peripheral role first)
6. [ ] BLE Central scanning and connection

## See Also

- `../../PROTOCOL.md` — full protocol specification
