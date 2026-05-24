/* components/transport/transport_stream.c */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdbool.h>
#include <stdint.h>

static uint8_t s_stream_buf[CEEPEW_MAX_MSG_BYTES];
static uint16_t s_stream_len = 0U;
static bool s_stream_ready = false;

CeePewErr_t transport_stream_send(const uint8_t *msg, uint16_t msg_len){
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);

    for (uint16_t i = 0U; i < msg_len; i++){ s_stream_buf[i] = msg[i];}
    s_stream_len = msg_len;
    s_stream_ready = true;
    return CEEPEW_OK;
}

CeePewErr_t transport_stream_receive(uint8_t *out, uint16_t *out_len, uint16_t max_out_len){
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_out_len > 0U, CEEPEW_ERR_PARAM);
    if (!s_stream_ready){ return CEEPEW_ERR_NOENT;}

    CEEPEW_ASSERT(s_stream_len <= max_out_len, CEEPEW_ERR_BOUNDS);
    for (uint16_t i = 0U; i < s_stream_len; i++) { out[i] = s_stream_buf[i];}
    *out_len = s_stream_len;
    s_stream_ready = false;
    s_stream_len = 0U;
    return CEEPEW_OK;
}