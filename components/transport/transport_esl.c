/* components/transport/transport_esl.c */

#include "hal_input.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>

/* ESL (ESP-NOW Security Layer) entry points — placeholders for integration
 * of CRC -> FEC -> Ascon auth -> replay checks pipeline.
 */

CeePewErr_t transport_esl_process_outgoing(uint8_t *frame, uint16_t *len, uint16_t max_len)
{
    CEEPEW_ASSERT(frame != NULL || *len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_len > 0U, CEEPEW_ERR_PARAM);

    /* No-op passthrough for now */
    (void)frame; (void)len; (void)max_len;
    return CEEPEW_ERR_UNSUPPORTED;
}

CeePewErr_t transport_esl_process_incoming(uint8_t *frame, uint16_t *len)
{
    CEEPEW_ASSERT(frame != NULL || *len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len != NULL, CEEPEW_ERR_NULL_PTR);

    (void)frame; (void)len;
    return CEEPEW_ERR_UNSUPPORTED;
}
