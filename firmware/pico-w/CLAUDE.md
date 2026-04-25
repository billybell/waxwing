# Waxing Pico W Firmware

## Building

```bash
# Clean stale CMake artifacts first — these end up in the parent dir and
# confuse subsequent builds into generating Makefiles there instead of build/
rm -rf build/ CMakeCache.txt cmake_install.cmake pico_flash_region.ld
bash build.sh
```

Output: `build/waxwing_mesh.uf2` (Pico W BOOTSEL flash target)

**Never** run `cmake ..` from inside `build/` — it detects the stale cache in the parent and generates all Makefiles there, then `make` fails with "no makefile found". Always use `bash build.sh` which handles the clean slate.

## BLE + CBOR

- `ble.c` serialises identity into a **single** CBOR map (major type 5)
- Keys: `"protocol"`, `"v"`, `"tpk"` (byte string), `"caps"`, `"firmware"`, `"firmware_ver"`, `"attended"`, `"unattended_mode"`, `"manifest_count"`, `"timestamp"`
- Multi-char keys use `0x60|len` for ≤23 chars, `0x78+len` otherwise

## Directory structure

```
src/         — C code (main.c, ble.c, identity.c, utils.c, constants.h, etc.)
waxwing/     — Python/MicroPython BLE stack (cbor.py, messages.py, identity.py)
```
