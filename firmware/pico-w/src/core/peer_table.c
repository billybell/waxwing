#include "core/peer_table.h"
#include "core/constants.h"

#include <string.h>
#include <stdint.h>

// Each slot holds one peer we've recorded a sync for. `last_synced_ms` is
// also our LRU stamp — most-recently used == newest timestamp. Eviction
// when full picks the slot with the smallest timestamp.
typedef struct {
    uint8_t            tpk_prefix[PEER_TPK_PREFIX_LEN];
    uint8_t            last_version;
    peer_sync_result_t last_result;
    uint32_t           last_synced_ms;
    int                used;
} peer_entry_t;

static peer_entry_t g_table[PEER_TABLE_CAP];

void peer_table_init(void) {
    memset(g_table, 0, sizeof(g_table));
}

static int find_slot(const uint8_t *tpk_prefix) {
    for (int i = 0; i < PEER_TABLE_CAP; i++) {
        if (g_table[i].used &&
            memcmp(g_table[i].tpk_prefix, tpk_prefix,
                   PEER_TPK_PREFIX_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

// Pick a slot for a new entry. Prefer an empty slot; if none, pick the
// LRU slot (smallest last_synced_ms among used entries).
static int pick_eviction_slot(void) {
    int lru = 0;
    uint32_t lru_stamp = UINT32_MAX;
    for (int i = 0; i < PEER_TABLE_CAP; i++) {
        if (!g_table[i].used) return i;
        if (g_table[i].last_synced_ms < lru_stamp) {
            lru_stamp = g_table[i].last_synced_ms;
            lru = i;
        }
    }
    return lru;
}

static uint32_t backoff_for(peer_sync_result_t r) {
    return (r == PEER_SYNC_RESULT_SUCCESS)
           ? (uint32_t)PEER_SUCCESS_BACKOFF_MS
           : (uint32_t)PEER_FAILED_BACKOFF_MS;
}

peer_decision_t peer_table_decide(const uint8_t *tpk_prefix,
                                  uint8_t advertised_version,
                                  uint32_t now_ms) {
    int idx = find_slot(tpk_prefix);
    if (idx < 0) return PEER_DECISION_CONNECT;            // never met

    const peer_entry_t *e = &g_table[idx];

    // !=, never >. The 1-byte counter wraps; any inequality means
    // "something changed since we last looked" and is worth a sync.
    if (e->last_version != advertised_version) {
        return PEER_DECISION_CONNECT;
    }

    // Subtraction is well-defined under wrap for monotonic millis.
    uint32_t age = now_ms - e->last_synced_ms;
    if (age >= backoff_for(e->last_result)) {
        return PEER_DECISION_CONNECT;
    }
    return PEER_DECISION_SKIP;
}

void peer_table_record_sync(const uint8_t *tpk_prefix,
                            uint8_t synced_version,
                            peer_sync_result_t result,
                            uint32_t now_ms) {
    int idx = find_slot(tpk_prefix);
    if (idx < 0) {
        idx = pick_eviction_slot();
        memcpy(g_table[idx].tpk_prefix, tpk_prefix, PEER_TPK_PREFIX_LEN);
        g_table[idx].used = 1;
    }
    g_table[idx].last_version   = synced_version;
    g_table[idx].last_result    = result;
    g_table[idx].last_synced_ms = now_ms;
}

int peer_table_size(void) {
    int n = 0;
    for (int i = 0; i < PEER_TABLE_CAP; i++) if (g_table[i].used) n++;
    return n;
}
