#ifndef WAXWING_PEER_TABLE_H
#define WAXWING_PEER_TABLE_H

#include <stdint.h>

/*
 * Peer seen-table: a small RAM-only LRU of nodes we've recently
 * encountered, keyed by a prefix of the peer's transport public key.
 *
 * Used by the scanning side of the mesh state machine to decide whether
 * to bother connecting to a peer whose advertisement we just observed.
 * The decision rule is:
 *
 *   - never seen this peer            → CONNECT
 *   - their advertised counter has changed since we last synced
 *                                     → CONNECT
 *   - same counter, but the recorded sync was long enough ago that the
 *     backoff window has expired      → CONNECT
 *   - otherwise                       → SKIP
 *
 * The two backoff windows (clean-sync vs. failed-sync) come from
 * core/constants.h. They are floors: the manifest counter is the
 * optimisation that lets us skip a sync sooner when nothing has
 * changed; the backoff is what guarantees eventual reconnection even if
 * the counter happens to collide.
 *
 * No flash backing in v0. The cost of a cold table on boot is one
 * wasted handshake per nearby peer, bounded by the backoff window.
 */

typedef enum {
    PEER_DECISION_CONNECT,
    PEER_DECISION_SKIP,
} peer_decision_t;

typedef enum {
    PEER_SYNC_RESULT_SUCCESS,   // clean sync — use long backoff
    PEER_SYNC_RESULT_FAILED,    // mid-session error/disconnect — short backoff
} peer_sync_result_t;

/* Reset the table to empty. Idempotent. */
void peer_table_init(void);

/*
 * Decide whether to connect to a peer that just appeared in scan
 * results. `tpk_prefix` is the leading PEER_TPK_PREFIX_LEN bytes of the
 * peer's transport public key (extracted from advert / scan-response —
 * the caller is responsible for that). `advertised_version` is the
 * 1-byte manifest counter from the same advert. `now_ms` is a
 * monotonic millisecond clock supplied by the caller.
 */
peer_decision_t peer_table_decide(const uint8_t *tpk_prefix,
                                  uint8_t advertised_version,
                                  uint32_t now_ms);

/*
 * Called by peer_sync after a session ends. `synced_version` is the
 * counter we observed in the peer's advertisement when we decided to
 * connect — we record it as "what we last saw" so the next encounter
 * with a different counter triggers a re-sync.
 *
 * `result` chooses which backoff window applies:
 *   SUCCESS  → PEER_SUCCESS_BACKOFF_MS
 *   FAILED   → PEER_FAILED_BACKOFF_MS
 *
 * If the peer is already in the table the entry is updated in place
 * (refreshing its LRU position); otherwise a new entry is added,
 * evicting the LRU slot if the table is full.
 */
void peer_table_record_sync(const uint8_t *tpk_prefix,
                            uint8_t synced_version,
                            peer_sync_result_t result,
                            uint32_t now_ms);

/* Number of entries currently held. Useful in tests. */
int peer_table_size(void);

#endif // WAXWING_PEER_TABLE_H
