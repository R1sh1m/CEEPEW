/* components/transport/transport_stream.c */

#include "hal_input.h"
#include <stdint.h>
#include <string.h>
#include "../../main/ceepew_assert.h"
#include "../mem/ceepew_pipeline.h"

CeePewErr_t transport_stream_send(const uint8_t *msg, uint16_t msg_len)
{
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    (void)msg; (void)msg_len;
    return CEEPEW_ERR_UNSUPPORTED;
}

CeePewErr_t transport_stream_receive(uint8_t *out, uint16_t *out_len, uint16_t max_out_len)
{
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    (void)out; (void)out_len; (void)max_out_len;
    return CEEPEW_ERR_UNSUPPORTED;
}
