/* components/ecc/ecc_arq.c */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdbool.h>
#include <stdint.h>

#define CEEPEW_ARQ_SEQ_BYTES 1U

static uint8_t s_tx_seq = 0U;
static uint8_t s_expected_rx_seq = 0U;
static bool s_rx_init = false;

/* Forward-declare transport functions provided elsewhere */
CeePewErr_t transport_espnow_send(const uint8_t *peer_mac, const uint8_t *data, uint16_t len);
CeePewErr_t transport_wait_ack(const uint8_t *peer_mac, uint8_t seq, uint32_t timeout_ms);

CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, uint16_t max_out_len){
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT((uint32_t)in_len + CEEPEW_ARQ_SEQ_BYTES <= max_out_len, CEEPEW_ERR_BOUNDS);

    out[0] = s_tx_seq++;
    for (uint16_t i = 0U; i < in_len; i++){ out[1U + i] = in[i];}
    *out_len = (uint16_t)(in_len + CEEPEW_ARQ_SEQ_BYTES);
    return CEEPEW_OK;
}

CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, uint16_t max_out_len, bool *corrected) {
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL && corrected != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len >= CEEPEW_ARQ_SEQ_BYTES, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT((uint32_t)(in_len - CEEPEW_ARQ_SEQ_BYTES) <= max_out_len, CEEPEW_ERR_BOUNDS);
    uint8_t seq = in[0];
    if (!s_rx_init){
        s_rx_init = true;
        s_expected_rx_seq = seq;
    }

    if (seq != s_expected_rx_seq){
        *corrected = false;
        return CEEPEW_ERR_REPLAY;
    }

    uint16_t payload_len = (uint16_t)(in_len - CEEPEW_ARQ_SEQ_BYTES);
    for (uint16_t i = 0U; i < payload_len; i++){ out[i] = in[1U + i]; }
    *out_len = payload_len;
    *corrected = false;
    s_expected_rx_seq++;
    return CEEPEW_OK;
}

/* Stop-and-Wait ARQ sender using transport_espnow_send() and transport_wait_ack() */
CeePewErr_t ecc_arq_send(const uint8_t peer_mac[6], const uint8_t *payload, uint16_t payload_len){
    CEEPEW_ASSERT(peer_mac != NULL && payload != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(payload_len > 0U && payload_len <= CEEPEW_PACKET_MAX_BYTES, CEEPEW_ERR_PARAM);

    uint8_t frame[CEEPEW_PACKET_MAX_BYTES];
    uint16_t frame_len = 0U;

    /* Prepare frame with sequence byte */
    CeePewErr_t err = ecc_arq_encode(payload, payload_len, frame, &frame_len, sizeof(frame));
    if (err != CEEPEW_OK) { return err; }

    uint8_t seq = frame[0];

    for (uint8_t attempt = 0U; attempt < CEEPEW_ARQ_MAX_RETRIES; attempt++){
        err = transport_espnow_send(peer_mac, frame, frame_len);
        if (err != CEEPEW_OK){ return err; }

        /* Wait for ACK (stubbed in platforms without ACK path) */
        CeePewErr_t ack_err = transport_wait_ack(peer_mac, seq, CEEPEW_ARQ_TIMEOUT_MS);
        if (ack_err == CEEPEW_OK){
            return CEEPEW_OK; /* Success */
        }
        else if (ack_err == CEEPEW_ERR_TIMEOUT){
            /* retry */
            continue;
        }
        else {
            return ack_err;
        }
    }
    return CEEPEW_ERR_MAX_RETRIES;
}
