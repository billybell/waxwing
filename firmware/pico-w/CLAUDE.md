# Waxwing Pico W Firmware

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

- `src/hw/pico-w/ble.c` serialises identity into a **single** CBOR map (major type 5)
- Keys: `"protocol"`, `"v"`, `"tpk"` (byte string), `"caps"`, `"firmware"`, `"firmware_ver"`, `"attended"`, `"unattended_mode"`, `"manifest_count"`, `"timestamp"`
- Multi-char keys use `0x60|len` for ≤23 chars, `0x78+len` otherwise

## Directory structure

```
src/core/        — platform-independent business logic (CBOR, commands,
                   identity, filestore interface, hal_crypto interface)
src/hw/pico-w/   — Pico W port (main, BLE, FatFS-on-flash filestore,
                   diskio, hal_crypto implementation)
src/thirdparty/  — vendored FatFS and monocypher
tests/           — host-only unit tests + RAM-backed mocks
```

## On-device storage layout

The FatFS volume on QSPI flash uses two directories that the firmware
code keeps strictly separate:

- `/files/` — user content. Reachable via the BLE file commands (`ls`,
  `read`, `write`, `delete`, `read_start/chunk`, `write_start/chunk/end`,
  `read_meta`, `write_meta`). Anything in here is exposable to peers.
- `/system/` — firmware-private blobs (transport identity today; future
  encounter records, attestations, etc.). Reachable only via the
  `fs_system_*` API. No `cmd_*` in `commands.c` ever resolves a name into
  this directory. Exposing system data to a peer must go through a
  deliberate, purpose-built command.
