# Waxwing C Firmware — Implementation Plan

## Current Status

**Working:**
- ✅ C firmware builds successfully (GCC 14.3 toolchain)
- ✅ Identity load/generate with flash persistence
- ✅ Device identity bytes-to-hex and TPK base64url encoding
- ✅ USB serial output (using `tud_connected()` check before printing)
- ✅ LED heartbeat placeholder

**Known Issues:**
- ⚠️ USB serial requires waiting for `tud_connected()` before printing (USB takes ~1-2s to enumerate)
- ⚠️ **Never use `sleep_ms()` in main loop** - blocks BLE stack; use timestamp-based timing with `to_ms_since_boot(get_absolute_time())`

**Not Implemented:**
- ❌ BLE GATT server (advertising, connections, read/write/notifies)
- ❌ File operations (list, read, write, chunked transfers)
- ❌ CBOR encoding/decoding
- ❌ Manifest generation
- ❌ Connection handling (on_connect, on_disconnect)
- ❌ Error handling and LED patterns

---

## Phase 1: BLE GATT Server (Core Functionality)

### 1.1 BLE Stack Integration
**Files to create:** `src/ble.h`, `src/ble.c`

**Responsibilities:**
- Initialize TinyUSB BLE stack (btstack from pico-sdk)
- Register 128-bit GATT service (CE575800-494E-4700-8000-00805F9B34FB)
- Register all 14 characteristics with proper UUIDs
- Configure characteristic properties (READ, WRITE, NOTIFY)
- Build advertisement data (31-byte limit):
  - `adv_data`: Flags + 128-bit UUID (21 bytes)
  - `resp_data`: Complete local name "WX:XXXXXXXX" (13 bytes)
- Start advertising with 500ms interval

**Key APIs:**
```c
bool ble_init(void);
void ble_start_advertising(const char *node_name);
void ble_stop_advertising(void);
void ble_process(void);  // Call in main loop
bool ble_is_connected(void);
```

**Callbacks to wire:**
- `on_connect(conn_handle)` — set connected flag, refresh manifest
- `on_disconnect(conn_handle)` — clear state, restart advertising
- `on_write(chr_idx, data, len)` — handle characteristic writes
- `on_read_req(chr_idx)` — respond with characteristic data
- `on_notify_complete(conn_handle, chr_idx)` — track sent bytes

**Reference:** `waxwing/ble.py` (MicroPython)

### 1.2 Device Identity Characteristic
**Status:** Partial (read handler needs implementation)

**Responsibilities:**
- Build Device Identity CBOR payload (181 bytes)
- Respond to reads with full payload or chunks
- Include: protocol version, name, TPK, fingerprint, caps, manifest_count, mode

**Reference:** `waxwing/ble.py:256`, `main.py:404-405`

### 1.3 File Response Characteristic (NOTIFY)
**Status:** Not implemented

**Responsibilities:**
- Handle chunked file reads (244 bytes per notification)
- Track send offset, window size
- Implement backpressure (wait for ACKs)
- Send `OP_CHUNK_ACK` after each chunk

**Reference:** `waxwing/ble.py:391-394`

---

## Phase 2: File Operations

### 2.1 File Storage Layer
**Files to create:** `src/filestore.h`, `src/filestore.c`

**Responsibilities:**
- Mount SPIFFS/LittleFS on internal flash
- Create `/files` directory
- Implement file listing (paginated)
- Implement file read (full and chunked)
- Implement file write (full and chunked)
- Implement file deletion
- Track free space, enforce quotas

**API:**
```c
bool filestore_init(void);
bool filestore_list(int offset, char *out_buf, size_t len);
bool filestore_read(const char *filename, uint8_t *out, size_t *len);
bool filestore_write(const char *filename, const uint8_t *data, size_t len);
bool filestore_chunked_write_start(const char *filename);
bool filestore_chunked_write_append(const uint8_t *data, size_t len);
bool filestore_chunked_write_finalize(void);
bool filestore_delete(const char *filename);
size_t filestore_free_bytes(void);
```

**Reference:** `waxwing/filestore.py`

### 2.2 File Command Characteristic
**Status:** Not implemented

**Responsibilities:**
- Parse opcodes: `ls`, `read`, `write_start`, `write_chunk`, `write_end`, `delete`
- Send responses via File Response characteristic
- Handle errors gracefully

**Reference:** `main.py:100-315`

---

## Phase 3: CBOR Encoding/Decoding

### 3.1 CBOR Library Integration
**Options:**
- Use `tinycbor` (embedded-friendly, BSD license)
- Use `cbor-small` (minimal footprint)
- Implement minimal subset (encode dict, list, bytes, string, uint)

**Responsibilities:**
- Encode device identity payload
- Decode incoming file commands
- Encode manifest entries

**Reference:** `waxwing/cbor.py`

---

## Phase 4: Manifest Generation

### 4.1 Manifest System
**Files to create:** `src/manifest.h`, `src/manifest.c`

**Responsibilities:**
- Compute SHA-256 for files (incremental for large files)
- Generate manifest entries (filename, truncated hash, size)
- Store manifest in flash
- Serve manifest via BLE
- Update manifest on file changes

**Reference:** `waxwing/filestore.py:_file_hash`, `waxwing/messages.py`

---

## Phase 5: Connection Management

### 5.1 Session Handling
**Responsibilities:**
- Track connection state (connected/disconnected)
- Store peer handle
- Clear chunked state on connect
- Handle stale writes on disconnect

**Reference:** `main.py:45-90`

---

## Phase 6: Crypto Implementation

### 6.1 Crypto Services
**Files to modify:** `src/identity.c`

**Current Issues:**
- Placeholder SHA-256 (line 29-35)
- Pseudo-random number generator (line 14-25)

**Required:**
- Integrate mbedTLS (already in pico-sdk)
- Implement proper SHA-256
- Implement true random (hardware RNG from Pico SDK)
- Implement Ed25519 key generation

**Reference:** `waxwing/identity.py`, `src/identity.c`

---

## Phase 7: LED Patterns

### 7.1 LED State Machine
**Status:** Placeholder (`main.c:35-40`)

**Patterns:**
- Idle (advertising): 1s on / 1s off
- Connected: 100ms on / 100ms off
- Error: 3 rapid flashes, 2s pause

**Reference:** `main.py:29-36`

---

## Phase 8: Testing & Integration

### 8.1 Test Strategy
- Unit tests for identity generation
- Unit tests for file operations
- Integration tests with iOS companion app
- Serial log verification

---

## Suggested Implementation Order

1. **BLE stack integration** — Make Pico advertise and accept connections
2. **Device identity read** — Return real identity via BLE
3. **File listing** — Paginated list via BLE
4. **File read (full)** — Read small text files
5. **File read (chunked)** — Handle large files
6. **File write** — Store files from companion
7. **Manifest generation** — Hash and serve manifests
8. **Crypto** — Replace placeholders with real crypto
9. **Edge cases** — Error handling, reconnection, cleanup

---

## Technical Notes

### Pico W Hardware
- **RP2040** — Dual-core ARM Cortex-M0+ @ 133MHz
- **Flash** — 2MB (1.5MB usable after MicroPython)
- **BLE** — BTstack in pico-sdk
- **GPIO** — 25 for LED (built-in)

### Build System
- CMake with pico-sdk
- Toolchain: ARM GNU 14.3.1
- Output: `build/waxwing_mesh.uf2`

### Memory Considerations
- BLE notifications limited to MTU-3 (~244 bytes default)
- Flash space limited (~1.5MB total, 32KB reserve)
- No malloc in interrupt handlers

---

## Files to Create

```
src/
├── ble.h              # NEW — BLE API
├── ble.c              # NEW — BLE implementation
├── filestore.h        # NEW — File storage API
├── filestore.c        # NEW — File storage implementation
├── manifest.h         # NEW — Manifest API
├── manifest.c         # NEW — Manifest implementation
├── identity.c         # MODIFY — Replace crypto placeholders
├── identity.h         # Existing
├── constants.h        # Existing — Already has file storage limits
├── utils.c            # Existing — Utilities
├── utils.h            # Existing
└── main.c             # MODIFY — Wire up all components
```

---

## Milestone: Parity with Phase 1 MicroPython

**Definition of Done:**
- [ ] Pico W advertises as "WX:XXXXXXXX"
- [ ] iOS app discovers and connects
- [ ] Device identity readable via BLE
- [ ] File listing via BLE
- [ ] File read via BLE (full and chunked)
- [ ] File write via BLE (chunked)
- [ ] Manifest generated and served
- [ ] LED indicates connection state

**Current Phase 1 MicroPython Features:**
- BLE advertising and GATT server ✅
- Device identity (read only) ✅
- File operations (list, read) ✅
- Connection tracking ✅
- LED heartbeat ✅

**Additional MicroPython Features (beyond Phase 1):**
- Chunked file writes ⏭
- Manifest generation ⏭
- File deletion ⏭
- Error handling ⏭

---

Generated: 2026-04-18
