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

Portable code and host tests live one level up in `firmware/core/`,
shared by every firmware target. Pico-W-specific bits stay here:

```
../core/         — platform-independent business logic (CBOR, commands,
                   identity, filestore interface, hal_crypto interface)
../core/tests/   — host-only unit tests + RAM-backed mocks
src/hw/pico-w/   — Pico W port (main, BLE, FatFS-on-flash filestore,
                   diskio, hal_crypto implementation)
src/thirdparty/  — vendored FatFS and monocypher
```

## On-device storage layout

The FatFS volume on QSPI flash uses two directories that the firmware
code keeps strictly separate:

- `/files/` — user content. Reachable via the BLE file commands (`ls`,
  `read`, `write`, `delete`, `read_start/chunk`, `write_start/chunk/end`,
  `read_meta`, `write_meta`). Anything in here is exposable to peers.
- `/system/` — firmware-private blobs (transport identity, manifest
  version counter, future encounter records, attestations). Reachable
  only via the `fs_system_*` API. No `cmd_*` in `commands.c` ever
  resolves a name into this directory. Exposing system data to a peer
  must go through a deliberate, purpose-built command.

## Mesh-mode lifecycle (Phase 3)

Boot order: `fs_init` → identity load → `manifest_counter_init` →
`peer_table_init` → `ble_init` → `ble_client_init` → `mesh_state_init`.

Run-loop tick: `ble_process()` → `mesh_state_tick(now_ms())` returns an
action that the driver in `main.c` translates into
`ble_start_advertising` / `ble_stop_advertising` /
`ble_client_start_scan` / `ble_client_stop_scan`. Connections (inbound
or outbound) freeze the FSM via `mesh_state_on_connected`; disconnects
resume it.

**Manifest counter invariant**: any successful mutation of `/files/`
bumps `manifest_counter_bump()` exactly once. The bump callsites are
deliberately scoped to two callers:

- `commands.c`: `cmd_write` / `cmd_write_end` / `cmd_delete` after
  successful filestore work (companion-driven mutations).
- `peer_sync.c`: after every successful `fs_chunked_finish` (peer-pull
  mutations).

`main.c` calls `ble_set_manifest_version(manifest_counter_get())` after
either path so the next advertisement frame on air carries the new
byte. The setter is idempotent on unchanged values.

**Two-connection btstack**: `MAX_NR_HCI_CONNECTIONS=2` so an inbound
companion connection and an outbound peer-sync connection can coexist
in the controller's tables. By construction `mesh_state` never enters
SCANNING while we're already CONNECTED, so we don't actually run two
concurrent connections — but both slots must exist so the controller
doesn't reject the connect.
