# Waxwing Mesh — CardPuter ADV Firmware

**Status:** Not yet implemented. Third firmware target; hardware arriving in a few weeks.

## Hardware

- M5Stack CardPuter ADV (ESP32-S3)
- Built-in keyboard and small display
- microSD card slot

## Notes

The ESP32-S3 provides BLE 5.0 and WiFi. The built-in display makes the CardPuter useful for showing mesh status, transfer progress, and simple content preview. The keyboard could support basic content creation (short text notes) directly on the device.

Firmware can be written using the Arduino framework or ESP-IDF. MicroPython is also available for initial prototyping.

## Implementation Plan

1. [ ] BLE GATT server
2. [ ] SD card file storage
3. [ ] Transport Identity keypair (stored in NVS flash)
4. [ ] Manifest exchange
5. [ ] File transfer
6. [ ] WiFi home network + AP mode for WiFi upgrade
7. [ ] Display: mesh status, transfer progress, content list
8. [ ] Optional: keyboard-based text note creation

## See Also

- `../../PROTOCOL.md` — full protocol specification
