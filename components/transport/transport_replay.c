/* components/transport/transport_replay.c */

#include "hal_input.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>

/* Simple replay detector placeholder: caller provides message id and timestamp.
 * For now, always accept and return not replayed. Real implementation must
 * track seen nonces and enforce silence-on-auth-failure.
 */

CeePewErr_t transport_replay_check(uint64_t msg_id, uint32_t timestamp, bool *is_replay)
{
    CEEPEW_ASSERT(is_replay != NULL, CEEPEW_ERR_NULL_PTR);

    (void)msg_id; (void)timestamp;
    *is_replay = false;
    return CEEPEW_OK;
}
