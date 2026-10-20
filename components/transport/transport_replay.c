/* components/transport/transport_replay.c
 *
 * WireGuard-style 64-bit replay window with check/commit split.
 *
 * SECURITY: transport_replay_check() is a PURE READ — it snapshots the
 * window under the spinlock and never writes bitmap/last_seq. It runs
 * pre-decrypt (transport_esl replay stage) as a cheap early reject.
 * The window advances only via transport_replay_commit(), called from the
 * task_session decrypt path AFTER the Ascon-128 AEAD tag verifies.
 * Forged frames therefore cannot shift the window (WireGuard model).
 */

#include "transport_replay.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct{
    uint64_t last_seq;
    uint64_t bitmap;
    bool initialised;
    portMUX_TYPE spinlock;
} ReplayWindow_t;

static ReplayWindow_t s_window = {0ULL, 0ULL, false, portMUX_INITIALIZER_UNLOCKED};

/* Pure predicate on a window snapshot: true when seq must be rejected
 * (duplicate of a seen seq, or older than the 64-bit window depth).
 * Forward seq values (seq > last_seq) are admissible — admission is NOT
 * commitment; the caller must still commit after AEAD verification. */
static bool replay_is_stale_or_dup(uint64_t last_seq, uint64_t bitmap,
                                    bool initialised, uint64_t seq){
    if (!initialised){
        return false;
    }
    if (seq > last_seq){
        return false;
    }
    uint64_t diff = last_seq - seq;
    if (diff >= CEEPEW_REPLAY_WINDOW_SIZE){
        return true;
    }
    return ((bitmap & (1ULL << diff)) != 0ULL);
}

CeePewErr_t transport_replay_check(uint64_t msg_id, uint32_t timestamp, bool *is_replay){
    CEEPEW_ASSERT(is_replay != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg_id > 0U, CEEPEW_ERR_PARAM);
    (void)timestamp;

    /* Snapshot under lock; release before deciding — never hold across work,
     * and never write. This is a pure read. */
    uint64_t last_seq = 0ULL;
    uint64_t bitmap = 0ULL;
    bool initialised = false;
    portENTER_CRITICAL(&s_window.spinlock);
    last_seq = s_window.last_seq;
    bitmap = s_window.bitmap;
    initialised = s_window.initialised;
    portEXIT_CRITICAL(&s_window.spinlock);

    *is_replay = replay_is_stale_or_dup(last_seq, bitmap, initialised, msg_id);
    return CEEPEW_OK;
}

CeePewErr_t transport_replay_commit(uint64_t msg_id){
    CEEPEW_ASSERT(msg_id > 0U, CEEPEW_ERR_PARAM);

    portENTER_CRITICAL(&s_window.spinlock);

    /* Re-check under the lock: a duplicate that passed check concurrently
     * (dup-then-confirm interleaving — two identical frames both admitted
     * pre-decrypt, both AEAD-verified) must fail here, not advance twice. */
    if (replay_is_stale_or_dup(s_window.last_seq, s_window.bitmap,
                                s_window.initialised, msg_id)){
        portEXIT_CRITICAL(&s_window.spinlock);
        return CEEPEW_ERR_REPLAY;
    }

    if (!s_window.initialised){
        s_window.initialised = true;
        s_window.last_seq = msg_id;
        s_window.bitmap = 1ULL;
        portEXIT_CRITICAL(&s_window.spinlock);
        return CEEPEW_OK;
    }

    if (msg_id > s_window.last_seq){
        uint64_t diff = msg_id - s_window.last_seq;
        if (diff >= CEEPEW_REPLAY_WINDOW_SIZE){
            s_window.bitmap = 1ULL;
        } else {
            s_window.bitmap = (s_window.bitmap << diff) | 1ULL;
        }
        s_window.last_seq = msg_id;
        portEXIT_CRITICAL(&s_window.spinlock);
        return CEEPEW_OK;
    }

    s_window.bitmap |= (1ULL << (s_window.last_seq - msg_id));
    portEXIT_CRITICAL(&s_window.spinlock);
    return CEEPEW_OK;
}

/* Test helper: reset internal replay window (unit tests only) */
void transport_replay_reset(void){
    portENTER_CRITICAL(&s_window.spinlock);
    s_window.last_seq = 0ULL;
    s_window.bitmap = 0ULL;
    s_window.initialised = false;
    portEXIT_CRITICAL(&s_window.spinlock);
}
