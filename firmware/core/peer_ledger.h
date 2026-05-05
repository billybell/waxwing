#ifndef WAXWING_PEER_LEDGER_H
#define WAXWING_PEER_LEDGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-peer running totals (M4 stage 4).
 *
 * Persistent ledger keyed by full 32-byte peer TPK. Tracks the
 * cumulative state of our relationship with each peer we've encountered,
 * so the lifetime fields in the next encounter record between us and a
 * given peer can carry signed history. Updated continuously during a
 * session as commands flow; consulted at the START of the next session
 * to populate PROPOSE / ACCEPT.
 *
 * Distinction from peer_table
 * ---------------------------
 * peer_table is a RAM-only LRU keyed by 8-byte tpk_prefix, used to
 * decide connect-or-skip on advertisement. peer_ledger is on disk,
 * keyed by full 32-byte pub, and tracks the *content* of the
 * relationship (bytes flowed, files received, peer's claimed lifetime
 * count at our last meeting). Either can be wiped without touching
 * the other.
 *
 * Storage layout
 * --------------
 * /system/peer_ledger.bin:
 *   header (16 bytes): magic 'WXPL', version 1, used_count u8,
 *                      reserved 10 bytes
 *   slots[PEER_LEDGER_CAP]: each 80 bytes —
 *     pub[32]
 *     last_meeting_count_le[8]
 *     tx_bytes_lifetime_le[8]
 *     rx_bytes_lifetime_le[8]
 *     file_count_lifetime_le[8]
 *     my_rep_score_le[4]
 *     reserved[12]
 *
 * Eviction at capacity
 * --------------------
 * Lowest last_meeting_count peer is evicted first. Rationale: a peer
 * with a high meeting_count has been encountered widely and is more
 * useful as a reputation anchor than one we barely know. Ties broken
 * by lowest array index (deterministic).
 */

#define PEER_LEDGER_CAP        64
#define PEER_LEDGER_PUB_BYTES  32

typedef struct {
    uint8_t  pub[PEER_LEDGER_PUB_BYTES];
    uint64_t last_meeting_count;
    uint64_t tx_bytes_lifetime;
    uint64_t rx_bytes_lifetime;
    uint64_t file_count_lifetime;
    int32_t  my_rep_score;
} peer_ledger_entry_t;

/* Load the persisted ledger from /system/peer_ledger.bin. Wrong magic,
 * wrong version, or short read: treats as empty. Idempotent.
 * Must be called once after fs_init().
 */
void peer_ledger_init(void);

/* Reset the in-RAM ledger to empty. Does not touch flash. (For tests.) */
void peer_ledger_reset(void);

/* Number of distinct peers currently in the ledger. */
int peer_ledger_count(void);

/* Read the current ledger entry for a peer. Returns true on hit and
 * fills *out. Returns false if peer not in ledger; *out is then zeroed.
 */
bool peer_ledger_get(const uint8_t pub[PEER_LEDGER_PUB_BYTES],
                     peer_ledger_entry_t *out);

/* Apply deltas to a peer's running totals. Creates a new entry on
 * first contact, evicting the lowest-meeting-count peer if at capacity.
 * `peer_meeting_count_seen` updates last_meeting_count if it's higher
 * than the stored value (peers should be monotonic; lower values are
 * either replay or fraud and we ignore them locally).
 *
 * Persists on every call. Returns 0 on success; on persist failure the
 * in-RAM update is still applied and -1 is returned (same trade-off as
 * manifest_counter / meeting_count).
 */
int peer_ledger_apply(const uint8_t pub[PEER_LEDGER_PUB_BYTES],
                      uint64_t tx_delta,
                      uint64_t rx_delta,
                      uint64_t file_delta,
                      uint64_t peer_meeting_count_seen);

/* Set the reputation score for a peer. Used by future reputation logic;
 * for stage 4 callers should leave entries at the default 0. Creates
 * the entry on first contact (without bumping any deltas). Returns 0 on
 * success, -1 on persist failure.
 */
int peer_ledger_set_rep(const uint8_t pub[PEER_LEDGER_PUB_BYTES],
                        int32_t rep_score);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_PEER_LEDGER_H
