/* components/transport/transport_replay.c */

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

/* Internal helper implementing WireGuard-style 64-bit replay window.
 * Returns CEEPEW_OK when packet accepted and window updated.
 * Returns CEEPEW_ERR_REPLAY when the packet is a replay or too-old.
 */
static CeePewErr_t replay_check_and_update(ReplayWindow_t *w, uint64_t seq){
    CEEPEW_ASSERT(w != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(seq > 0ULL, CEEPEW_ERR_PARAM);

    portENTER_CRITICAL(&w->spinlock);

    if (!w->initialised){
        w->initialised = true;
        w->last_seq = seq;
        w->bitmap = 1ULL;
        portEXIT_CRITICAL(&w->spinlock);
        return CEEPEW_OK;
    }

    if (seq > w->last_seq){
        uint64_t diff = seq - w->last_seq;
        if (diff >= CEEPEW_REPLAY_WINDOW_SIZE){
            w->bitmap = 1ULL;
        } else {
            w->bitmap = (w->bitmap << diff) | 1ULL;
        }
        w->last_seq = seq;
        portEXIT_CRITICAL(&w->spinlock);
        return CEEPEW_OK;
    }

    uint64_t diff = w->last_seq - seq;
    if (diff >= CEEPEW_REPLAY_WINDOW_SIZE){
        portEXIT_CRITICAL(&w->spinlock);
        return CEEPEW_ERR_REPLAY;
    }

    uint64_t mask = 1ULL << diff;
    if ((w->bitmap & mask) != 0ULL){
        portEXIT_CRITICAL(&w->spinlock);
        return CEEPEW_ERR_REPLAY;
    }

    w->bitmap |= mask;
    portEXIT_CRITICAL(&w->spinlock);
    return CEEPEW_OK;
}

CeePewErr_t transport_replay_check(uint64_t msg_id, uint32_t timestamp, bool *is_replay){
    CEEPEW_ASSERT(is_replay != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg_id > 0U, CEEPEW_ERR_PARAM);
    (void)timestamp;

    CeePewErr_t err = replay_check_and_update(&s_window, msg_id);
    if (err == CEEPEW_OK){
        *is_replay = false;
        return CEEPEW_OK;
    }
    else if (err == CEEPEW_ERR_REPLAY){
        *is_replay = true;
        return CEEPEW_OK; /* Silent fail as callers expect */
    }
    return err;
}

/* Test helper: reset internal replay window (unit tests only) */
void transport_replay_reset(void){
    portENTER_CRITICAL(&s_window.spinlock);
    s_window.last_seq = 0ULL;
    s_window.bitmap = 0ULL;
    s_window.initialised = false;
    portEXIT_CRITICAL(&s_window.spinlock);
}

