# Phase 3 Plan — Pico ↔ Pico Peer Sync

**Status:** draft, pre-implementation
**Goal:** Two Waxwing nodes, powered on within BLE range of each other, automatically discover one another and pull every file the other has into their own `/files/` until the receiver runs out of room. No phone required, no human in the loop, no review gate yet.

This document is the working plan: the design, the open questions I want your call on, the file-by-file changes, and the unit-test list to write in parallel.

---

## 1. Behaviour we want

### 1.1 Boot

1. Power on. Mount FatFS. Load identity.
2. **Companion grace window** — advertise only, scanning off, for ~10 s. If a phone connects in that window, treat the session as a normal companion session and stay peripheral-only until disconnect. Identity load is unchanged. The grace window exists so the user can grab the node out of a drawer, plug in power, and pair without a race against peer scanning.
3. After the window expires (or the companion disconnects), enter **mesh mode**.

### 1.2 Mesh mode

Alternate between two states with random dwell times in [1 s, 5 s]:

- **Advertise** — current GATT peripheral path, exactly as today, plus a 1-byte
  `manifest_version` counter in the service-data field (see §3.7).
- **Scan** — passive scan for the Waxwing service UUID. On a match, decide
  whether to connect based on what we've seen from this peer before (§3.8).
  If we connect: become Central, run a sync session, disconnect. Then resume
  the alternation.

Random per-cycle dwell defeats the obvious lockstep pathology where two devices alternate in phase and never overlap. A small jitter (±20 %) on top of the uniform draw is fine; uniform alone is good enough.

A connection from a phone (or another node, while we're peripheral) preempts the alternation: the timer freezes while connected and resumes from the next state on disconnect.

### 1.3 Sync session

Once a connection is up — either we initiated as Central or the peer did — exactly one peer reads the other peer's files. (See §2 below for the directional decision.) The reader walks the writer's `ls`, downloads each file it doesn't already have via `read_start` + `read_chunk`, also pulls the matching `.meta` sidecar via `read_meta`, and writes both to its own `/files/`. When done, disconnect. The whole thing reuses the existing file-command CBOR vocabulary on the existing characteristics — no new GATT entities.

---

## 2. Open question: bidirectional vs unidirectional per connection

**My recommendation: unidirectional per connection. The Central pulls; the Peripheral serves.**

Why:

- **Symmetry across the population, not within a session.** Over many encounters every node ends up both Central and Peripheral roughly equally because the Central role is decided by who happens to be scanning when the other is advertising. Lopsidedness in any single session is fine; the population-level flow is balanced.
- **No new protocol needed.** The Peripheral side already speaks the full file-command vocabulary. The Central side just needs to be a GATT client running the same commands the iOS app already runs. Bidirectional in one connection means the Peripheral also has to act as a *client* over the same connection — which means writing GATT-client code for the Peripheral too, against its own peer's File Command characteristic. Doable but doubles the surface area.
- **Concurrency is simpler.** One commands-in-flight queue, one direction of data, no contention over the File Response notify channel.
- **Disconnects are cheaper.** A failed mid-session disconnect just means "try again next encounter, from scratch." We don't have to reason about "I finished sending you mine, did you finish sending me yours?"
- **The companion app already proves the design.** The iOS app is a Central that pulls. Reusing the same code path on the Pico means the Central-side logic gets shared between two consumers (and the firmware exercises the same wire format the phone does).

The cost is one extra encounter to converge after a content drop: if A gets a new file at T=0 and meets B at T=10s, A → B happens immediately, but B → A's content has to wait for the next encounter where B is the Central. With 1–5 s random alternation this is sub-30 s in expectation. Cheap.

If you push back on this: bidirectional in one connection is also defensible, but I'd want a reason — e.g. "encounters are rare enough that we want to drain everything in one shot," which doesn't match the "two nodes on a desk" use case we're optimising for first.

**Decision needed before implementation starts.** Everything below assumes unidirectional / Central-pulls.

---

## 3. Design

### 3.1 New module: `core/peer_sync.{c,h}` (platform-independent)

Public API, called by the hw layer when a peer connection is established as Central:

```c
typedef struct peer_sync_session peer_sync_session_t;

peer_sync_session_t *peer_sync_start(const uint8_t peer_tpk[32]);
// Returns a session handle, or NULL if we already have an in-flight session
// or local resources are exhausted. peer_tpk is captured for logging only;
// no reputation check yet.

// Drives the session forward by one step. The caller is the hw layer's
// run loop. Returns:
//   PEER_SYNC_NEED_WRITE  — encode the next outgoing CBOR command into
//                            out_buf and ship it over the peer's File
//                            Command write characteristic.
//   PEER_SYNC_NEED_READ   — waiting for a notification from peer's File
//                            Response. Caller does nothing.
//   PEER_SYNC_DONE        — session complete; caller should disconnect.
//   PEER_SYNC_ERROR       — fatal; caller should disconnect.
typedef enum { PEER_SYNC_NEED_WRITE, PEER_SYNC_NEED_READ,
               PEER_SYNC_DONE, PEER_SYNC_ERROR } peer_sync_step_t;

peer_sync_step_t peer_sync_next_command(peer_sync_session_t *s,
                                        uint8_t *out_buf, size_t out_max,
                                        size_t *out_len);

// Feed an incoming notification (File Response payload) into the state machine.
peer_sync_step_t peer_sync_on_response(peer_sync_session_t *s,
                                       const uint8_t *resp, size_t resp_len);

void peer_sync_end(peer_sync_session_t *s);  // also frees the handle
```

Internal state (single in-flight session, allocated statically — no malloc on this MCU):

```
state ∈ { LIST_PAGE, READ_START, READ_CHUNK, READ_META, NEXT_FILE, DONE }
current page of ls + offset
current file name + total size + bytes received so far
bytes received accumulator (writes go through fs_chunked_*)
a "skip if already present" check before each read (fs_file_size != -1)
```

Why a state machine and not a thread / RTOS task: btstack on the Pico W is single-threaded and IRQ-driven; `peer_sync_next_command` is called from the existing `ble_process` poll loop, the same place we'd otherwise be tempted to block. Easy to test in host mode by feeding canned CBOR responses.

### 3.2 New module: `core/mesh_state.{c,h}` (platform-independent)

The advertise/scan alternation timer + role state. Pure logic, no hardware:

```c
typedef enum {
    MESH_GRACE,          // boot window, peripheral only
    MESH_ADVERTISING,    // mesh mode, currently advertising
    MESH_SCANNING,       // mesh mode, currently scanning
    MESH_CONNECTED,      // a connection (companion or peer) is active
} mesh_phase_t;

void mesh_state_init(uint32_t now_ms);

// Caller polls this from the run loop.
typedef enum {
    MESH_ACTION_NONE,
    MESH_ACTION_START_ADVERTISING,
    MESH_ACTION_START_SCANNING,
    MESH_ACTION_STOP_BOTH,    // entering CONNECTED
} mesh_action_t;

mesh_action_t mesh_state_tick(uint32_t now_ms);

void mesh_state_on_connected(uint32_t now_ms);
void mesh_state_on_disconnected(uint32_t now_ms);

mesh_phase_t mesh_state_phase(void);
```

The dwell-time draw is a function of a `uint32_t (*rng_u32)(void)` injected at init — production wires it to `hal_random_bytes`, tests inject a deterministic stub so the schedule is reproducible.

### 3.3 Hardware port additions (`hw/pico-w/ble.c` + a small new client surface)

Today `ble.c` only has a peripheral path. Add:

- **`gap_set_scan_parameters` + `gap_start_scan` / `gap_stop_scan`**, with a scan-result callback that filters on the Waxwing service UUID and surfaces the peer's BD address + advertised TPK to a new callback `ble_set_on_peer_seen(cb)`.
- **GATT client** for connecting to that peer. btstack provides `gap_connect`, `gatt_client_discover_*`, `gatt_client_write_value_of_characteristic_without_response`, and `gatt_client_listen_for_characteristic_value_updates`. Wire those into a tiny client API:

```c
// hw/pico-w/ble_client.{c,h}  -- new files, kept separate so the peripheral
//                                 path stays simple to read.
typedef void (*ble_client_on_connected_cb)(uint16_t conn_handle);
typedef void (*ble_client_on_response_cb)(const uint8_t *data, size_t len);
typedef void (*ble_client_on_disconnected_cb)(void);

bool ble_client_init(...);
bool ble_client_connect(bd_addr_t addr, uint8_t addr_type);
bool ble_client_send_command(const uint8_t *cmd, size_t cmd_len);  // writes CHAR_FILE_COMMAND
void ble_client_disconnect(void);
```

The client side runs `peer_sync_*` against incoming notifications; the existing peripheral side keeps doing what it does today (companion app and incoming peer pulls).

### 3.4 main.c rewiring

```
boot:
  fs_init / identity / ble_init (peripheral)
  ble_client_init                          // new
  mesh_state_init(now)                     // starts in GRACE
  ble_start_advertising()
  // (no scanning yet — grace window)

run loop tick:
  ble_process()
  switch (mesh_state_tick(now)):
    START_ADVERTISING: ble_start_advertising(); ble_client_stop_scan()
    START_SCANNING:    ble_stop_advertising();  ble_client_start_scan()
    STOP_BOTH:         (both already off because ble.c stops adv on connect)
    NONE:              nothing

ble_set_on_connect(...)         -> mesh_state_on_connected(now)
ble_set_on_disconnect(...)      -> mesh_state_on_disconnected(now)
ble_client_on_peer_seen(...)    -> ble_client_connect(addr) (only if mesh_state_phase() == SCANNING)
ble_client_on_connected(...)    -> peer_sync_start(peer_tpk); kick off first command
ble_client_on_response(buf,len) -> step = peer_sync_on_response(..); if NEED_WRITE then send next; if DONE/ERROR ble_client_disconnect()
ble_client_on_disconnected(..)  -> peer_sync_end(); mesh_state_on_disconnected(now)
```

### 3.5 Storage / dedup

The receiver needs to skip files it already has. Cheapest possible check that we'd be unembarrassed to keep: name match. We already publish 8-byte truncated hashes from `fs_list`, but nothing computes them on the receiver side yet, and "skip by name" is a perfectly fine first cut for the prototype.

If `fs_file_size(name) >= 0`, skip. Otherwise call `read_start`, then loop `read_chunk` and write chunks via `fs_chunked_start` / `fs_chunked_append` / `fs_chunked_finish` into the receiver's `/files/`. Pull `.meta` last via `read_meta` / `fs_write_meta`.

Out-of-space handling: if `fs_storage_info().free_out` drops below the file size we're about to pull, abort *that file* with `fs_chunked_abort`, log, move on. We do **not** delete anything to make room — per your direction.

### 3.6 Manifest counter (advertised hint)

Every node persists a single byte at `/system/manifest_version.bin`:

```
uint8_t manifest_version
```

It is incremented (with wrap-around — a `uint8_t` rolls every 256 changes, that
is intentional) any time the local `/files/` set changes:

- After a successful `cmd_write` / `cmd_write_end` / `cmd_delete` from the
  companion app's BLE commands (`commands.c` is the only place that touches
  `/files/` in response to peer or companion traffic).
- After `peer_sync` finalises a successful chunked pull (`fs_chunked_finish`).
- After `peer_sync` performs any other persistent change to `/files/` (none
  yet, but the bump-point lives in `peer_sync` so future churn is naturally
  covered).

The filestore stays oblivious — it doesn't bump the counter itself. Bumping
lives in the two callers (`commands.c` and `peer_sync.c`) so the rule is
"any successful user-files mutation increments the counter once." Easy to
audit, easy to test.

The advertised byte is read from the in-RAM cached value on every advert
refresh, not re-read from flash. Boot reads the persisted value once, all
subsequent bumps update RAM and write-through to flash.

#### Why one byte and not four

The counter is a **hint that lets a scanner skip a sync it would have done
later anyway**. It is never authoritative. Costs of a collision:

- One wasted connection: an `ls` round trip and a disconnect. Sub-second.
- Bounded by the backoff window: at worst, a collision means we wait the
  full backoff before syncing, which is exactly what we'd have done with no
  counter at all.

Saving three bytes in every advertisement is unambiguously worth the
occasional wasted handshake. Smaller adverts also keep more headroom for the
service-UUID block and play nicer with iOS background scanning.

#### `!=`, never `>`

Comparison is **`peer.version != table.version`**, never `>`. With wrap-around
the counter has no ordering — any inequality means "something changed since
we last talked, go sync." Reaching for `>` is the obvious bug; we'll have a
unit test that bumps a peer's counter through 256 increments back to its
original value and asserts intermediate inequalities trigger sync.

#### Wire layout

The advertisement currently emits flags + a `COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS` block. Replace the UUID block with a `SERVICE_DATA_128BIT_UUID` block carrying the same UUID plus the counter as its data:

```
0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
0x14, BLUETOOTH_DATA_TYPE_SERVICE_DATA_128BIT_UUID,
      <16 bytes UUID, little-endian>,
      <1 byte manifest_version>,
```

Total advertisement: 23 bytes (vs the current 22). Still well inside the 31-byte cap. Scan response is unchanged (still carries the local node name).

The companion-app scanner currently filters on the UUID announced via the standalone-UUID block; it will need a one-line change to accept the same UUID announced via service-data. iOS Core Bluetooth surfaces both forms in the same advertisement-data dictionary keyed by `CBAdvertisementDataServiceUUIDsKey` / `CBAdvertisementDataServiceDataKey`, so this is a small, mechanical edit.

### 3.7 Peer seen-table and connect decision

A small RAM-only LRU table of recently-encountered peers:

```c
typedef struct {
    uint8_t  tpk_prefix[8];   // first 8 bytes of peer TPK; advert-derivable
    uint8_t  last_version;
    uint32_t last_synced_ms;  // monotonic clock
} peer_seen_entry_t;

#define PEER_TABLE_CAP 32
```

Sized at 32 entries, ~13 bytes each, 416 bytes RAM total. Evicted LRU when full. Lives entirely in RAM for v0 — no `/system/` blob, no flash writes per encounter. We can revisit persistence later; the cost of a cold table at boot is one wasted handshake per nearby peer, bounded by the backoff window.

API (pure logic, host-testable):

```c
typedef enum {
    PEER_DECISION_CONNECT,   // never met, version changed, or backoff expired
    PEER_DECISION_SKIP,      // recently synced and version unchanged
} peer_decision_t;

void peer_table_init(void);
peer_decision_t peer_table_decide(const uint8_t tpk_prefix[8],
                                  uint8_t advertised_version,
                                  uint32_t now_ms);
void peer_table_record_sync(const uint8_t tpk_prefix[8],
                            uint8_t synced_version,
                            uint32_t now_ms);
```

`peer_table_decide` is what the scanner calls on every advertisement match. The scanner does *not* call `record_sync`; only `peer_sync` calls that, after either:

- a successful `PEER_SYNC_DONE` (record the peer's *advertised* version at the moment we connected — not a recomputed one, since we never asked the peer to re-advertise after the session), **or**
- a `PEER_SYNC_ERROR` that we want to back off from (record with a sentinel "failed" version equal to the advertised one + a special short-backoff flag, see below).

Decision logic:

```
on advertisement (tpk_prefix, advertised_version, now):
    entry = peer_table_lookup(tpk_prefix)
    if entry == NULL:
        return CONNECT             # never met
    if entry.last_version != advertised_version:
        return CONNECT             # they changed; might have new files
    age = now - entry.last_synced_ms
    threshold = entry.failed ? FAILED_BACKOFF_MS : SUCCESS_BACKOFF_MS
    if age >= threshold:
        return CONNECT             # long enough; retry
    return SKIP                     # caught up, recently
```

Constants (in `core/constants.h`):

```c
#define MESH_GRACE_MS               10000     // boot companion window
#define MESH_DWELL_MIN_MS            1000     // alternation lower bound
#define MESH_DWELL_MAX_MS            5000     // alternation upper bound
#define MESH_DWELL_JITTER_PCT          20     // ±20% on top of the draw
#define PEER_SUCCESS_BACKOFF_MS    600000     // 10 min after a clean sync
#define PEER_FAILED_BACKOFF_MS      30000     // 30 s after an error/disconnect
#define PEER_BACKOFF_JITTER_PCT        20     // ±20% so clocks don't realign
```

Why two backoffs: a successful sync that found nothing new (or pulled everything cleanly) is a strong signal the peer is caught up — wait the long window. A mid-session error or unexpected disconnect could mean the peer is still there with content we never finished pulling — retry sooner.

### 3.8 LED feedback

Distinguish the four phases on the existing onboard LED.

| Phase | Pattern |
|---|---|
| GRACE | very fast pulse (50 ms) for 10 s |
| ADVERTISING | slow blink (1 s) — current behaviour |
| SCANNING | double-blink (100 ms on, 100 ms off, 100 ms on, 700 ms off) |
| CONNECTED | fast blink (100 ms) — current behaviour |
| Sync in progress (sub-state of CONNECTED) | solid on |

---

## 4. File-by-file change list

### New files

| Path | Purpose |
|---|---|
| `src/core/peer_sync.h` / `.c` | Sync session state machine, host-testable |
| `src/core/mesh_state.h` / `.c` | Advertise/scan alternation FSM, host-testable |
| `src/core/manifest_counter.h` / `.c` | 1-byte counter; load/bump/persist via `fs_system_*` |
| `src/core/peer_table.h` / `.c` | RAM-only LRU of seen peers + connect decision |
| `src/hw/pico-w/ble_client.h` / `.c` | btstack GATT-client surface + scan |
| `tests/test_peer_sync.c` | State machine tests with canned responses |
| `tests/test_mesh_state.c` | Schedule tests with injected RNG and clock |
| `tests/test_manifest_counter.c` | Persistence + bump semantics + wrap-around |
| `tests/test_peer_table.c` | LRU + decision logic + backoff windows |
| `firmware/pico-w/PEER_SYNC_PLAN.md` | This document |

### Modified files

| Path | Change |
|---|---|
| `src/hw/pico-w/main.c` | Wire grace window, mesh tick, peer-seen → connect, sync driver, counter init |
| `src/hw/pico-w/ble.h/.c` | Replace UUID-list adv block with service-data block carrying `manifest_version`; add `ble_refresh_advertisement()` so the counter byte can update without a full re-init |
| `src/hw/pico-w/ble.h/.c` | Add `ble_set_on_peer_seen` + `ble_client_*` declarations |
| `src/core/commands.c` | Bump `manifest_counter` after successful `cmd_write` / `cmd_write_end` / `cmd_delete` |
| `src/core/constants.h` | Grace, dwell, jitter, two backoff windows, peer-table cap |
| `tests/test_runner.c` | Register the new test groups |
| `tests/mock_filestore.{c,h}` | (already extended for `/system/`) confirm system blob persistence path is exercised |
| `firmware/pico-w/CMakeLists.txt` | Add new sources to both `waxwing_mesh` and `waxwing_test` targets |
| `firmware/pico-w/README.md` | Document mesh mode, advertisement layout, LED table |
| `firmware/pico-w/CLAUDE.md` | Add mesh-mode lifecycle + manifest-counter invariant |
| `ios/.../BLEManager.swift` | One-line scan filter: accept the Waxwing UUID via `CBAdvertisementDataServiceDataKey` in addition to the existing UUID-list path |

### Untouched

- `src/core/commands.c`, `filestore.{h,c}`, identity, CBOR codec — **no protocol changes**. The whole sync session is built on the existing wire format.
- The iOS companion app — peer sync is a firmware-internal concern. The companion app continues to talk to its node exactly as before. (We may want to add a "peer sync log" in the companion app later, but not in this phase.)

---

## 5. Recommended implementation order

Each step ends green (host tests + firmware build).

1. **`manifest_counter` + tests.** Smallest possible thing. One byte, three operations (load, bump, get). Persisted via `fs_system_*`. No hardware, no other modules involved. Tests cover wrap-around, persistence across init/reinit, idempotent bump.
2. **`peer_table` + tests.** Pure logic. LRU ring, deterministic decision function. Tests cover: never-seen peer connects, version-changed peer connects, caught-up peer skips, expired backoff connects, failed-vs-success backoff thresholds, LRU eviction, the **`!=` semantics** wrap-around test (256 bumps back to original).
3. **`mesh_state` + tests.** Pure logic with injected RNG and clock. Get the alternation FSM right.
4. **`peer_sync` + tests.** Pure logic. State machine driven by canned CBOR responses against a mock filestore. Bumps `manifest_counter` on successful `fs_chunked_finish`.
5. **`ble_client` skeleton.** btstack scanning + service-data parser + connect + characteristic discovery + write-without-response + notify subscribe. Stub the response handling. Verifiable by flashing one Pico and watching it find and connect to another. No sync yet.
6. **Advertisement layout swap.** Replace UUID-list block with service-data block carrying the counter byte. Companion-app one-line scan filter update. Verify iOS still discovers the node.
7. **Wire `peer_sync` into `ble_client`.** Two Picos with one file each. Eyeball over USB serial.
8. **Wire `mesh_state` + `peer_table` into `main.c`.** Now they alternate, decide, sync, back off. Two-node soak: leave them running for an hour. Watch that they sync once, then go quiet until something changes.
9. **Polish.** LED states, log noise reduction, README and CLAUDE.md updates.

---

## 6. Unit tests to write in parallel

These should be drafted alongside (preferably *before*) the implementation that motivates them. All host-side, all in the existing `waxwing_test` target.

### `test_mesh_state.c`

| Test | What it proves |
|---|---|
| `test_mesh_starts_in_grace` | After init, phase is GRACE; `tick` returns NONE while still inside the grace window. |
| `test_mesh_grace_expires_to_advertising` | After grace duration elapses, `tick` returns START_ADVERTISING and phase flips to ADVERTISING. |
| `test_mesh_alternates_advertising_scanning` | Run a long stream of ticks against an injected clock; assert the sequence of actions matches the schedule the injected RNG produced. |
| `test_mesh_dwell_within_bounds` | All emitted dwells are in [1000 ms, 5000 ms]. |
| `test_mesh_companion_freezes_schedule` | A connected event mid-cycle freezes the timer; on disconnect, the timer resumes from a fresh advertise/scan choice. |
| `test_mesh_grace_skipped_if_companion_connects_immediately` | Companion connects during GRACE → on disconnect we are in mesh mode, not back in GRACE. (Decide: yes, that's the natural behaviour. Document it.) |
| `test_mesh_jitter_breaks_lockstep` | Two independent state machines with the same RNG seed but offset start times don't end up perfectly antiphase forever. (Soft test: run 1000 cycles, assert overlap occurs.) |

### `test_peer_sync.c`

| Test | What it proves |
|---|---|
| `test_peer_sync_empty_peer` | Peer's `ls` returns zero files → session ends in DONE on the first response. No `read_start` issued. |
| `test_peer_sync_single_file_full_pull` | One file, fits in one chunk. End-to-end: `ls → read_start → read_chunk → read_meta → done`. The mock filestore on the *receiver* side ends up with the exact bytes. |
| `test_peer_sync_multi_chunk_pull` | One file >> chunk size. Receiver assembles correctly. |
| `test_peer_sync_pagination` | Peer has more files than fit in one `ls` response; sync follows `next_offset`. |
| `test_peer_sync_dedup_by_name` | A file with that name already exists locally → no `read_start` is issued for it. |
| `test_peer_sync_partial_meta_ok` | Peer returns `error: not found` for a sidecar → we accept the file without meta and move on. (Decide: yes, meta is best-effort.) |
| `test_peer_sync_peer_error_mid_session` | Peer returns `error: not found` for a `read_chunk` we should have been able to make → session ends with ERROR, no half-written file is left in the receiver's `/files/` (chunked write was aborted). |
| `test_peer_sync_out_of_space` | Receiver's free space drops below incoming file size → that file is skipped, session continues with the next file. |
| `test_peer_sync_disconnect_mid_chunk` | `peer_sync_end` called while a chunked write is open → write is aborted, no orphan file remains. |
| `test_peer_sync_envelope_fits_mtu` | Every command emitted by `peer_sync_next_command` fits in `mtu - 3` for `mtu ∈ {23, 247, 517}`. |
| `test_peer_sync_bumps_counter_on_finish` | After a successful chunked pull, `manifest_counter_get()` returned a value 1 greater (mod 256) than before. |
| `test_peer_sync_no_bump_on_dedup_skip` | A file skipped because it already exists locally does **not** increment the counter. |
| `test_peer_sync_no_bump_on_aborted_pull` | A pull aborted mid-chunk by error does not increment the counter. |

### `test_manifest_counter.c`

| Test | What it proves |
|---|---|
| `test_counter_starts_at_zero` | First boot with no persisted blob → counter is 0. |
| `test_counter_persists` | Bump, reinit, value survives. |
| `test_counter_wraps_at_256` | 256 bumps from 0 → back to 0; no crash, no off-by-one. |
| `test_counter_bump_writes_through` | Bump updates RAM and writes to `/system/manifest_version.bin` in the same call. |
| `test_counter_load_corrupted_blob` | If the persisted blob is the wrong size, treat as 0 (do not refuse to boot). |

### `test_peer_table.c`

| Test | What it proves |
|---|---|
| `test_peer_unknown_connects` | Lookup miss → CONNECT. |
| `test_peer_version_changed_connects` | Same TPK, different `last_version` → CONNECT regardless of clock. |
| `test_peer_caught_up_skips_within_window` | Same TPK, same version, `now - last_synced < SUCCESS_BACKOFF` → SKIP. |
| `test_peer_caught_up_connects_after_window` | Same TPK, same version, age beyond SUCCESS_BACKOFF → CONNECT. |
| `test_peer_failed_uses_short_backoff` | Recorded after error → SKIP only until FAILED_BACKOFF, then CONNECT. |
| `test_peer_wraparound_inequality_triggers_sync` | Bump a peer's recorded version through 256 increments; assert each non-equal intermediate value triggers CONNECT. Catches a `>` instead of `!=` regression. |
| `test_peer_lru_evicts_oldest` | Fill table, add one more → oldest entry evicted, others survive. |
| `test_peer_record_updates_timestamp` | Subsequent `record_sync` for a known peer updates the slot in place, not a new entry. |

### Adjustments to existing tests

- `test_commands.c` already covers the firmware-side responses we'll be consuming. No new coverage needed there — peer_sync is simply a *consumer* of that wire format and is tested against canned responses, not against a live commands handler. (We *could* link both sides into a single test that runs commands + sync against the same in-memory filestore — that's a tempting integration test for step 4 above. Probably worth doing as a stretch.)

### Stretch integration test

`test_two_node_loopback.c` — wire one mock filestore as the "peer", one as the local node, drive `peer_sync` and `commands_handle` against each other in a tight loop. Asserts: after one full session, every file the peer had is now in the local mock with byte-identical content and meta. This is a strong regression net for the whole chain and will fall out of step 4 cheaply if we structure peer_sync's API to accept its responses directly (which the API above does).

---

## 7. Things we're explicitly *not* doing in this phase

- **No reputation, recommendations, or review gating.** Copy everything that fits.
- **No deletion or eviction.** Out-of-space stops new pulls; never deletes.
- **No record of what we've already deleted / refused.** Per your call.
- **No bidirectional sync in one connection.** See §2.
- **No companion-app awareness of peer sync.** Phone still owns its connection; the node just happens to also do mesh things when the phone isn't around.
- **No WiFi upgrade negotiation.** That's its own phase.
- **No protocol or wire-format changes.** Reuse the file commands as-is.
- **No new GATT characteristics.** Reuse the existing File Command (W) + File Response (N).

---

## 8. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Two devices in lockstep antiphase, never seeing each other | Random dwell + jitter; `test_mesh_jitter_breaks_lockstep` proves the schedule has overlap probability ≫ 0 over time. |
| Sync session wedges (peer stops responding mid-chunk) | Per-response watchdog inside `peer_sync` (mirror of the iOS app's 60 s watchdog). On timeout: ERROR → disconnect. Cycle resumes. |
| Receiver accumulates a half-written file after a disconnect | `peer_sync_end` always calls `fs_chunked_abort` for any in-flight write. Asserted by `test_peer_sync_disconnect_mid_chunk`. |
| Identity reused across role switches confuses peers | Identity is per-device, role-agnostic. We advertise the same TPK whether we're scanning or advertising. No change needed. |
| Companion app sees a "peer" advertising the same UUID and tries to connect | Already handled — the iOS scanner doesn't filter on attended/unattended caps, but the existing connect path works against either. Worth testing once on hardware; no firmware change needed. |
| btstack scanning while connected blows up | Only scan while `mesh_state_phase() == SCANNING`, and the FSM never enters SCANNING while a connection is active. Enforced in main.c, asserted by `test_mesh_companion_freezes_schedule`. |
| Two caught-up nodes spam each other with handshakes | `peer_table` SKIP path; `test_peer_caught_up_skips_within_window`. Backoff is the floor, the manifest counter is the optimisation. |
| Counter collision (true rollover ambiguity) | Bounded cost: one wasted handshake. Eventually corrected at the next backoff expiry. Acceptable; documented in §3.6. |
| Counter never bumps after a successful pull (silent regression) | `test_peer_sync_bumps_counter_on_finish` plus the symmetric "no bump on skip / abort" tests. |
| Cold peer table at boot causes a flurry of handshakes | Bounded by neighbours-in-range × 1 handshake. Each handshake either pulls files (good) or records a SKIP for the next ten minutes (also good). Self-corrects within one backoff window. |
| Companion app stops discovering nodes after the advertisement layout change | Caught at the iOS one-line filter update; verified manually by reconnecting after step 6 of §5. |

---

## 9. Decisions, settled

1. **Direction:** unidirectional, Central pulls. Confirmed.
2. **Grace duration:** 10 s.
3. **Dwell range:** 1–5 s uniform with ±20 % jitter.
4. **Dedup key:** filename only for v0; hash-based dedup deferred.
5. **LED states:** included.
6. **Manifest counter:** `uint8_t` advertised in service-data, `!=` semantics, wrap-around explicitly tolerated as a one-handshake collision cost.
7. **Backoff:** 10 min after a clean sync, 30 s after a failed one, ±20 % jitter on both. Manifest counter optimises the typical case; backoff is the correctness floor.

When you're ready to kick off, my first commit will be step 1 of §5 (`manifest_counter` + tests). One byte, one module, all host-side, ~150 lines including tests. Lands the persistence pattern we'll reuse for `peer_table`'s eventual flash-backed variant, and doesn't touch any of the BLE wiring yet. Each subsequent commit follows the order in §5.
