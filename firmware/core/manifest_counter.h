#ifndef WAXWING_MANIFEST_COUNTER_H
#define WAXWING_MANIFEST_COUNTER_H

#include <stdint.h>

/*
 * Manifest counter: a 1-byte hint advertised in BLE service-data so peer
 * scanners can decide whether a previously-synced node has anything new
 * worth a fresh handshake.
 *
 * Semantics
 * ---------
 *   - Wraps at 256. Comparison MUST be `!=`, never `>` — wrap means there
 *     is no ordering, only "did this change since I last looked."
 *   - Bumped exactly once per successful mutation of /files/ (peer pull
 *     finalised, companion write finalised, file deleted). The filestore
 *     itself does NOT bump; the bump lives in the caller that just
 *     observed a successful mutation, so the rule stays "any successful
 *     user-files change increments once."
 *   - Persisted as a single byte at /system/manifest_version.bin via the
 *     fs_system_* API. Boot loads once; subsequent bumps update RAM and
 *     write through to flash in the same call.
 *   - This is a hint, not a fact. A collision (two distinct local states
 *     happening to land on the same byte) costs at most one wasted
 *     handshake, bounded by the backoff window.
 *
 * Why a separate module
 * ---------------------
 * Sits between the filestore (which doesn't know it exists) and the BLE
 * advertisement layer (which reads the cached value on every refresh).
 * Pure logic except for the two fs_system_* calls; host-testable against
 * the existing mock_filestore.
 */

/*
 * Load the persisted counter from /system/manifest_version.bin. Sets the
 * in-RAM cached value; any subsequent manifest_counter_get() returns it
 * without touching flash. If the blob is missing or corrupted (wrong
 * size), the counter resets to 0 and the caller is unaware.
 *
 * Must be called once after fs_init(). Idempotent — calling again just
 * reloads from flash.
 */
void manifest_counter_init(void);

/*
 * Increment the counter (with wrap-around) and write the new value
 * through to flash. Returns the new value.
 *
 * Failure to persist is logged but otherwise ignored — the in-RAM value
 * is still bumped, so the advertised counter remains live until the
 * next reboot. On reboot a missing/stale blob just looks like a normal
 * fresh-counter situation, which a peer treats as "go sync." Acceptable
 * cost.
 */
uint8_t manifest_counter_bump(void);

/*
 * Read the current cached counter value. Cheap; never touches flash.
 */
uint8_t manifest_counter_get(void);

#endif // WAXWING_MANIFEST_COUNTER_H
