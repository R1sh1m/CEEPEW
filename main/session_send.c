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
#include "hal_radio.h"
#include "session_msgstore.h"
#include "transport_esl.h"
#include "esp_crc.h"

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

CeePewErr_t session_send_message(const uint8_t *plaintext, uint16_t len,
                                 const uint8_t peer_mac[6],
                                 const uint8_t peer_public_key[32])
{
    CEEPEW_ASSERT(plaintext != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(peer_mac != NULL && peer_public_key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(session_is_active(), CEEPEW_ERR_PARAM);

    /* The peer_public_key parameter is now the peer's X25519 public key
     * (exchanged via BLE sign_pk exchange). Used by crypto_box_encrypt
     * for ECDH shared secret derivation. */
    uint8_t local_compressed[CEEPEW_HUFF_BUF_MAX];
    uint8_t local_ascon_ct[CEEPEW_MAX_MSG_BYTES + CEEPEW_ASCON_TAG_BYTES];
    uint8_t local_box_ct[CEEPEW_MAX_MSG_BYTES + 64U];
    uint8_t local_sig[64U];
    uint8_t local_payload[CEEPEW_MAX_MSG_BYTES + 128U];
    uint8_t local_fec_buf[CEEPEW_FEC_BUF_MAX];
    uint8_t local_frame[CEEPEW_PACKET_MAX_BYTES];

    uint16_t comp_len = (uint16_t)sizeof(local_compressed);
    CeePewErr_t err = compress_huffman_compress(plaintext, len, local_compressed,
                                                &comp_len, (uint16_t)sizeof(local_compressed), NULL);
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

    uint16_t ascon_ct_len = (uint16_t)sizeof(local_ascon_ct);
    err = crypto_ascon_aead_encrypt(ascon_key, ascon_nonce, NULL, 0U,
                                    local_compressed, comp_len,
                                    local_ascon_ct, &ascon_ct_len);
    if (err != CEEPEW_OK) { return err; }

    uint16_t box_ct_len = (uint16_t)sizeof(local_box_ct);
    err = crypto_box_encrypt(&g_crypto_ctx, peer_public_key,
                             local_ascon_ct, ascon_ct_len,
                             local_box_ct, &box_ct_len);
    if (err != CEEPEW_OK) { return err; }

    err = session_sign_message(local_box_ct, box_ct_len, local_sig);
    if (err != CEEPEW_OK) { return err; }

    CEEPEW_ASSERT((uint32_t)box_ct_len + 64U <= sizeof(local_payload), CEEPEW_ERR_BOUNDS);
    memcpy(local_payload, local_box_ct, box_ct_len);
    memcpy(local_payload + box_ct_len, local_sig, 64U);

    uint16_t payload_len = (uint16_t)(box_ct_len + 64U);
    uint16_t fec_len = (uint16_t)sizeof(local_fec_buf);
    err = ecc_hamming_encode(local_payload, payload_len, local_fec_buf, &fec_len);
    if (err != CEEPEW_OK) { return err; }

    CEEPEW_ASSERT((uint32_t)fec_len + sizeof(uint32_t) <= sizeof(local_frame), CEEPEW_ERR_BOUNDS);
    memcpy(local_frame, local_fec_buf, fec_len);
    uint32_t crc = esp_crc32_le(0U, local_frame, fec_len);
    memcpy(local_frame + fec_len, &crc, sizeof(crc));

    uint16_t frame_len = (uint16_t)(fec_len + sizeof(uint32_t));
    err = transport_esl_process_outgoing(local_frame, &frame_len, (uint16_t)sizeof(local_frame));
    if (err != CEEPEW_OK) { return err; }

    err = hal_radio_set_peer(peer_mac);
    if (err != CEEPEW_OK) { return err; }
    err = hal_radio_init();
    if (err != CEEPEW_OK) { return err; }

    err = hal_radio_send(local_frame, frame_len);
    if (err != CEEPEW_OK) { return err; }

    err = msg_store_add(local_frame, frame_len, len, 1U);
    if (err != CEEPEW_OK) { return err; }

    err = session_update_last_message_time();
    if (err != CEEPEW_OK) { return err; }

    return CEEPEW_OK;
}
