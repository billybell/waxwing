# Waxwing Mesh Pico W C Firmware

This document describes the current state of the C firmware for the Raspberry Pi Pico W.
The C implementation is now the primary firmware and supersedes the earlier prototype.

**Current Status:** The firmware implements BLE advertising, Device Identity, file commands (`ls`, `read`, `write`, `delete`, `storage_info`), FAT filesystem, CBOR codec, and Ed25519 transport identity via monocypher. See `README.md` for detailed build and usage instructions.

The code in this repository (under `firmware/pico-w/waxwing/` if present) is legacy and should be removed; it is not part of the active build.

For full details on building, flashing, and running the firmware, consult the main `README.md` in this directory.