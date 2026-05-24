/* main/session_send.c
 *
 * End-to-end TX wrapper for a single plaintext message.
 * This module keeps the transmit flow in one place so the session FSM
 * can hand off message delivery without duplicating crypto or framing logic.
 */

#include "session_fsm.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "crypto_ctx.h"
#include "crypto_box_wrap.h"
#include "compress_huffman.h"
#include "ecc_hamming.h"
#include "ecc_crc32.h"
#include "transport_esl.h"

#include <stdint.h>
#include <string.h>

CeePewErr_t crypto_ascon_aead_encrypt(const uint8_t key[16],
                                      const uint8_t nonce[16],
                                      const uint8_t *ad,
                                      uint16_t ad_len,
                                      const uint8_t *pt,
                                      uint16_t pt_len,
                                      uint8_t *ct,
                                      uint16_t *ct_len);
CeePewErr_t transport_espnow_send(const uint8_t *peer_mac,
                                  const uint8_t *data,
                                  uint16_t len);

CeePewErr_t session_send_message(const uint8_t *plaintext, uint16_t len,
                                 const uint8_t peer_mac[6],
                                 const uint8_t peer_public_key[32])
{
    CEEPEW_ASSERT(plaintext != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(peer_mac != NULL && peer_public_key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(session_is_active(), CEEPEW_ERR_PARAM);

    static uint8_t s_compressed[CEEPEW_HUFF_BUF_MAX];
    static uint8_t s_ascon_ct[CEEPEW_MAX_MSG_BYTES + CEEPEW_ASCON_TAG_BYTES];
    static uint8_t s_box_ct[CEEPEW_MAX_MSG_BYTES + 64U];
    static uint8_t s_sig[64U];
    static uint8_t s_payload[CEEPEW_MAX_MSG_BYTES + 128U];
    static uint8_t s_fec_buf[CEEPEW_FEC_BUF_MAX];
    static uint8_t s_frame[CEEPEW_PACKET_MAX_BYTES];

    uint16_t comp_len = (uint16_t)sizeof(s_compressed);
    CeePewErr_t err = compress_huffman_compress(plaintext, len, s_compressed,
                                                &comp_len, (uint16_t)sizeof(s_compressed), NULL);
    if (err != CEEPEW_OK) { return err; }

    err = session_enforce_nonce_limit();
    if (err != CEEPEW_OK) { return err; }

    uint8_t nonce_24[24U];
    err = session_get_nonce(nonce_24);
    if (err != CEEPEW_OK) { return err; }

    uint8_t ascon_nonce[16U];
    memcpy(ascon_nonce, nonce_24, sizeof(ascon_nonce));

    uint8_t ascon_key[16U];
    err = session_get_session_key(ascon_key);
    if (err != CEEPEW_OK) { return err; }

    uint16_t ascon_ct_len = (uint16_t)sizeof(s_ascon_ct);
    err = crypto_ascon_aead_encrypt(ascon_key, ascon_nonce, NULL, 0U,
                                    s_compressed, comp_len,
                                    s_ascon_ct, &ascon_ct_len);
    if (err != CEEPEW_OK) { return err; }

    uint16_t box_ct_len = (uint16_t)sizeof(s_box_ct);
    err = crypto_box_encrypt(&g_crypto_ctx, peer_public_key,
                             s_ascon_ct, ascon_ct_len,
                             s_box_ct, &box_ct_len);
    if (err != CEEPEW_OK) { return err; }

    err = session_sign_message(s_box_ct, box_ct_len, s_sig);
    if (err != CEEPEW_OK) { return err; }

    CEEPEW_ASSERT((uint32_t)box_ct_len + 64U <= sizeof(s_payload), CEEPEW_ERR_BOUNDS);
    memcpy(s_payload, s_box_ct, box_ct_len);
    memcpy(s_payload + box_ct_len, s_sig, 64U);

    uint16_t payload_len = (uint16_t)(box_ct_len + 64U);
    uint16_t fec_len = (uint16_t)sizeof(s_fec_buf);
    err = ecc_hamming_encode(s_payload, payload_len, s_fec_buf, &fec_len);
    if (err != CEEPEW_OK) { return err; }

    CEEPEW_ASSERT((uint32_t)fec_len + sizeof(uint32_t) <= sizeof(s_frame), CEEPEW_ERR_BOUNDS);
    memcpy(s_frame, s_fec_buf, fec_len);
    uint32_t crc = 0U;
    err = ecc_crc32_compute(s_frame, fec_len, &crc);
    if (err != CEEPEW_OK) { return err; }
    memcpy(s_frame + fec_len, &crc, sizeof(crc));

    uint16_t frame_len = (uint16_t)(fec_len + sizeof(uint32_t));
    err = transport_esl_process_outgoing(s_frame, &frame_len, (uint16_t)sizeof(s_frame));
    if (err != CEEPEW_OK) { return err; }

    err = transport_espnow_send(peer_mac, s_frame, frame_len);
    if (err != CEEPEW_OK) { return err; }

    err = session_update_last_message_time();
    if (err != CEEPEW_OK) { return err; }

    return CEEPEW_OK;
}
