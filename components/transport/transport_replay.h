/* components/transport/transport_replay.h
 *
 * WireGuard-style 64-bit replay window — check/commit split API.
 *
 * SECURITY MODEL (WireGuard):
 *   transport_replay_check() is a PURE READ. It never mutates the window.
 *   It runs pre-decrypt as a cheap early reject of obvious replays/dups.
 *   transport_replay_commit() advances the window and MUST be called only
 *   AFTER the Ascon-128 AEAD tag verifies (task_session decrypt path).
 *   This prevents forged frames from shifting last_seq/bitmap and
 *   desyncing legitimate traffic.
 *
 * CONCURRENCY: both calls take/release the window spinlock internally and
 * never hold it across crypto. commit() re-checks under the lock so a
 * duplicate that passed check concurrently (dup-then-confirm interleaving)
 * fails commit instead of advancing the window twice.
 */

#ifndef TRANSPORT_REPLAY_H
#define TRANSPORT_REPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure-read replay predicate. No window state is modified.
 * Returns CEEPEW_OK always (on valid args); *is_replay reports the verdict.
 * A forward seq (seq > last_seq) is admissible here — it is NOT committed. */
CeePewErr_t transport_replay_check(uint64_t msg_id, uint32_t timestamp, bool *is_replay);

/* Advance the window for msg_id. Call only after AEAD tag verification.
 * Returns CEEPEW_OK on commit, CEEPEW_ERR_REPLAY if msg_id is now a
 * duplicate or too old (re-checked under lock), CEEPEW_ERR_PARAM if
 * msg_id == 0. */
CeePewErr_t transport_replay_commit(uint64_t msg_id);

/* Reset the window. Called from session_reset_to_discovery() at the start
 * of each new pairing session (unit tests only otherwise). */
void transport_replay_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_REPLAY_H */
