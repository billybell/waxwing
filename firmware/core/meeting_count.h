#ifndef WAXWING_MEETING_COUNT_H
#define WAXWING_MEETING_COUNT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lifetime encounter counter (M4 stage 3).
 *
 * Monotonically increasing 64-bit count of completed encounters this
 * node has participated in. Used as the proof-of-time field in
 * encounter records (since `now_ms()` is only reliable on companion-
 * paired devices) and as a reputation input ("how widely connected
 * is this node?").
 *
 * Semantics
 * ---------
 *   - Bumped exactly once per encounter that reaches DONE in
 *     encounter_session.{h,c}. Not bumped for abandoned handshakes.
 *   - Persisted as 8 bytes little-endian at /system/meeting_count.bin
 *     via the fs_system_* API.
 *   - In-RAM cache mirrors the on-disk value; reads never touch flash.
 *   - Saturating: never wraps. uint64 is enough lifetime headroom.
 *
 * Integrity
 * ---------
 * A node can locally lie about its count by editing /system/, but the
 * mesh as a whole can audit consistency: every encounter record carries
 * BOTH parties' counts at meeting time, so once an encounter is signed
 * and propagated, that snapshot is permanent. A node claiming count=10
 * today after a peer has already published a record showing it
 * signed an encounter at count=50000 last week is a detectable fraud.
 */

/* Load the persisted counter from /system/meeting_count.bin. Sets the
 * in-RAM cached value; missing or corrupted blob resets to 0. Idempotent.
 * Must be called once after fs_init().
 */
void meeting_count_init(void);

/* Reset to zero (for tests). Does not touch flash. */
void meeting_count_reset(void);

/* Increment and persist. Returns the new value. Failure to persist
 * leaves the in-RAM value bumped and logs a warning — same trade-off
 * as manifest_counter (RAM-only until reboot, then a fresh-counter
 * cold start).
 */
uint64_t meeting_count_bump(void);

/* Read the cached counter. Cheap; never touches flash. */
uint64_t meeting_count_get(void);

#ifdef __cplusplus
}
#endif

#endif // WAXWING_MEETING_COUNT_H
