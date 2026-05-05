#include "core/peer_ledger.h"
#include "core/filestore.h"

#include <stdio.h>
#include <string.h>

#define BLOB_NAME           "peer_ledger.bin"
#define MAGIC0              'W'
#define MAGIC1              'X'
#define MAGIC2              'P'
#define MAGIC3              'L'
#define VERSION             1
#define HEADER_BYTES        16
#define SLOT_BYTES          80
#define BUFFER_BYTES        (HEADER_BYTES + PEER_LEDGER_CAP * SLOT_BYTES)

// In-RAM mirror of the persisted ledger. Deserialised at init; rewritten
// in full on every apply. The whole blob is small enough (~5 KB) that
// rewrite-in-full is cheaper than per-slot patching.
static peer_ledger_entry_t g_entries[PEER_LEDGER_CAP];
static uint8_t             g_used = 0;

// ---------------------------------------------------------------------------
// Little-endian serialization helpers
// ---------------------------------------------------------------------------

static void put_u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static uint64_t get_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (i * 8);
    return v;
}

static void put_i32_le(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(u >> (i * 8));
}

static int32_t get_i32_le(const uint8_t *p) {
    uint32_t u = 0;
    for (int i = 0; i < 4; i++) u |= ((uint32_t)p[i]) << (i * 8);
    return (int32_t)u;
}

// ---------------------------------------------------------------------------
// (De)serialisation of one slot
// ---------------------------------------------------------------------------

static void encode_slot(const peer_ledger_entry_t *e, uint8_t out[SLOT_BYTES]) {
    memset(out, 0, SLOT_BYTES);
    memcpy(out, e->pub, PEER_LEDGER_PUB_BYTES);
    put_u64_le(out + 32, e->last_meeting_count);
    put_u64_le(out + 40, e->tx_bytes_lifetime);
    put_u64_le(out + 48, e->rx_bytes_lifetime);
    put_u64_le(out + 56, e->file_count_lifetime);
    put_i32_le(out + 64, e->my_rep_score);
    // bytes 68..79 reserved
}

static void decode_slot(const uint8_t in[SLOT_BYTES], peer_ledger_entry_t *e) {
    memcpy(e->pub, in, PEER_LEDGER_PUB_BYTES);
    e->last_meeting_count  = get_u64_le(in + 32);
    e->tx_bytes_lifetime   = get_u64_le(in + 40);
    e->rx_bytes_lifetime   = get_u64_le(in + 48);
    e->file_count_lifetime = get_u64_le(in + 56);
    e->my_rep_score        = get_i32_le(in + 64);
}

// ---------------------------------------------------------------------------
// Whole-file persistence
// ---------------------------------------------------------------------------

static int persist(void) {
    uint8_t buf[BUFFER_BYTES];
    memset(buf, 0, sizeof(buf));
    buf[0] = MAGIC0;
    buf[1] = MAGIC1;
    buf[2] = MAGIC2;
    buf[3] = MAGIC3;
    buf[4] = VERSION;
    buf[5] = g_used;
    // bytes 6..15 reserved
    for (uint8_t i = 0; i < g_used; i++) {
        encode_slot(&g_entries[i], buf + HEADER_BYTES + (size_t)i * SLOT_BYTES);
    }
    if (fs_system_write(BLOB_NAME, buf, sizeof(buf)) != 0) {
        printf("[peer_ledger] WARN: persist failed\r\n");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void peer_ledger_init(void) {
    g_used = 0;
    memset(g_entries, 0, sizeof(g_entries));

    uint8_t buf[BUFFER_BYTES];
    int n = fs_system_read(BLOB_NAME, buf, sizeof(buf));
    if (n != (int)sizeof(buf)) return;          // missing or corrupt: empty
    if (buf[0] != MAGIC0 || buf[1] != MAGIC1 ||
        buf[2] != MAGIC2 || buf[3] != MAGIC3) return;
    if (buf[4] != VERSION) return;
    uint8_t used = buf[5];
    if (used > PEER_LEDGER_CAP) return;          // header lies about size

    for (uint8_t i = 0; i < used; i++) {
        decode_slot(buf + HEADER_BYTES + (size_t)i * SLOT_BYTES, &g_entries[i]);
    }
    g_used = used;
}

void peer_ledger_reset(void) {
    g_used = 0;
    memset(g_entries, 0, sizeof(g_entries));
}

int peer_ledger_count(void) {
    return g_used;
}

// Linear search by pub. Returns index or -1.
static int find_index(const uint8_t pub[PEER_LEDGER_PUB_BYTES]) {
    for (uint8_t i = 0; i < g_used; i++) {
        if (memcmp(g_entries[i].pub, pub, PEER_LEDGER_PUB_BYTES) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Index of the entry with the lowest last_meeting_count (ties → lowest
// array index). Caller has already verified the table is non-empty.
static uint8_t pick_eviction_victim(void) {
    uint8_t  victim = 0;
    uint64_t best   = g_entries[0].last_meeting_count;
    for (uint8_t i = 1; i < g_used; i++) {
        if (g_entries[i].last_meeting_count < best) {
            best   = g_entries[i].last_meeting_count;
            victim = i;
        }
    }
    return victim;
}

// Get-or-insert. Returns the slot index. May evict if at capacity.
static uint8_t upsert_slot(const uint8_t pub[PEER_LEDGER_PUB_BYTES]) {
    int existing = find_index(pub);
    if (existing >= 0) return (uint8_t)existing;

    uint8_t slot;
    if (g_used < PEER_LEDGER_CAP) {
        slot = g_used++;
    } else {
        slot = pick_eviction_victim();
    }
    memset(&g_entries[slot], 0, sizeof(g_entries[slot]));
    memcpy(g_entries[slot].pub, pub, PEER_LEDGER_PUB_BYTES);
    return slot;
}

bool peer_ledger_get(const uint8_t pub[PEER_LEDGER_PUB_BYTES],
                     peer_ledger_entry_t *out) {
    if (!out) return false;
    int i = find_index(pub);
    if (i < 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    *out = g_entries[i];
    return true;
}

// Saturating add: prevents wrap when a single delta would push past
// UINT64_MAX. Same defensive posture as meeting_count.
static uint64_t sat_add_u64(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a) return UINT64_MAX;
    return a + b;
}

int peer_ledger_apply(const uint8_t pub[PEER_LEDGER_PUB_BYTES],
                      uint64_t tx_delta,
                      uint64_t rx_delta,
                      uint64_t file_delta,
                      uint64_t peer_meeting_count_seen) {
    uint8_t i = upsert_slot(pub);
    peer_ledger_entry_t *e = &g_entries[i];

    e->tx_bytes_lifetime   = sat_add_u64(e->tx_bytes_lifetime,   tx_delta);
    e->rx_bytes_lifetime   = sat_add_u64(e->rx_bytes_lifetime,   rx_delta);
    e->file_count_lifetime = sat_add_u64(e->file_count_lifetime, file_delta);
    if (peer_meeting_count_seen > e->last_meeting_count) {
        e->last_meeting_count = peer_meeting_count_seen;
    }
    return persist();
}

int peer_ledger_set_rep(const uint8_t pub[PEER_LEDGER_PUB_BYTES],
                        int32_t rep_score) {
    uint8_t i = upsert_slot(pub);
    g_entries[i].my_rep_score = rep_score;
    return persist();
}
