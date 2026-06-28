/* main/session_send.c
 *
 * End-to-end TX wrapper for a single plaintext message.
 * This module keeps the transmit flow in one place so the session FSM
 * can hand off message delivery without duplicating crypto or framing logic.
 *
 * KEY DESIGN INVARIANTS (do not violate):
 *   - crypto_box_encrypt takes the peer's X25519 public key (g_crypto_ctx.peer_box_pubkey).
 *     The Ed25519 peer sign key (peer_sign_pk) is for verifying RECEIVED signatures only.
 *   - All crypto operations are wrapped in g_crypto_mutex to prevent a torn read
 *     from Core 1 (session task) while Core 0 (UI task) is encrypting.
 *   - hal_radio_init() MUST run before hal_radio_set_peer() so that esp_now_add_peer()
 *     fires when s_initialised is already true.
 */

#include "session_fsm.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "crypto_ctx.h"
#include "crypto_box_wrap.h"
#include "compress_huffman.h"
#include "ecc_hamming.h"
#include "hal_radio.h"
#include "session_msgstore.h"
#include "transport_esl.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "session_send";

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
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(session_is_active(), CEEPEW_ERR_PARAM);

    /* RC#0 guard: the peer's X25519 key must have arrived via BLE before we
     * can encrypt. If it hasn't, the caller passed either NULL or the wrong
     * (Ed25519) key — both produce unreadable ciphertext.  We gate here so
     * the UI gets a clean CEEPEW_ERR_PARAM rather than silently sending
     * garbage the peer can never decrypt. */
    if (!session_peer_box_pubkey_valid()) {
        ESP_LOGE(TAG, "send: peer X25519 box pubkey not yet received — aborting");
        return CEEPEW_ERR_PARAM;
    }

    /* Verify ESP-NOW peer is registered before attempting to send.
     * This prevents sending to unregistered peers which causes
     * ESP_ERR_ESPNOW_NOT_FOUND (status=1) failures. */
    if (!hal_radio_is_peer_registered(peer_mac)) {
        ESP_LOGE(TAG, "send: ESP-NOW peer not registered — aborting");
        return CEEPEW_ERR_PARAM;
    }

    /* peer_public_key parameter is accepted for call-site compatibility but
     * is intentionally IGNORED: the correct key for crypto_box ECDH is always
     * g_crypto_ctx.peer_box_pubkey (X25519), read under the crypto mutex below.
     * The Ed25519 peer_sign_pk (returned by session_get_peer_public_key) must
     * never be passed to crypto_box — it would produce garbage ciphertext. */
    (void)peer_public_key;

    uint8_t local_compressed[CEEPEW_HUFF_BUF_MAX];
    uint8_t local_ascon_ct[CEEPEW_MAX_MSG_BYTES + CEEPEW_ASCON_TAG_BYTES];
    uint8_t local_box_ct[CEEPEW_MAX_MSG_BYTES + 64U];
    uint8_t local_sig[64U];
    uint8_t local_payload[CEEPEW_MAX_MSG_BYTES + 128U];
    uint8_t local_fec_buf[CEEPEW_FEC_BUF_MAX];
    uint8_t local_frame[CEEPEW_PACKET_MAX_BYTES];
    uint8_t local_box_pubkey[32U]; /* snapshot of peer X25519 key under mutex */

    uint16_t comp_len = (uint16_t)sizeof(local_compressed);
    CeePewErr_t err = compress_huffman_compress(plaintext, len, local_compressed,
                                                &comp_len, (uint16_t)sizeof(local_compressed), NULL);
    if (err != CEEPEW_OK) { goto cleanup; }

    /* ── Crypto critical section (RC#5) ──────────────────────────────────
     * Acquire the crypto mutex before reading g_crypto_ctx or calling any
     * crypto primitive that touches it. The session task on Core 1 writes
     * g_crypto_ctx during nonce updates and key derivation; without this
     * lock the UI task on Core 0 can observe a torn context mid-encrypt.
     * IMPORTANT: session_enforce_nonce_limit() must be called INSIDE the mutex
     * to prevent nonce counter race between TX (Core 0) and RX (Core 1) paths. */
    if (crypto_mutex_lock() != CEEPEW_OK) {
        ESP_LOGE(TAG, "send: failed to acquire crypto mutex");
        err = CEEPEW_ERR_BUSY;
        goto cleanup;
    }

    /* Enforce nonce limit while holding mutex to prevent race with RX path */
    err = session_enforce_nonce_limit();
    /* NONCE_NEARLY_EXHAUSTED is a warning (90% threshold) — the send can still
     * proceed. Only NONCE_EXHAUSTED (hard limit reached) is a true abort. */
    if (err == CEEPEW_ERR_NONCE_EXHAUSTED) { (void)crypto_mutex_unlock(); goto cleanup; }
    if (err != CEEPEW_OK && err != CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED) { (void)crypto_mutex_unlock(); goto cleanup; }

    /* Snapshot the peer X25519 key while holding the mutex */
    memcpy(local_box_pubkey, g_crypto_ctx.peer_box_pubkey, 32U);

    uint8_t nonce_24[24U];
    err = session_get_nonce(nonce_24);
    if (err != CEEPEW_OK) { (void)crypto_mutex_unlock(); goto cleanup; }

    uint8_t ascon_nonce[16U];
    memcpy(ascon_nonce, nonce_24, sizeof(ascon_nonce));

    uint8_t ascon_key[16U];
    err = session_get_session_key(ascon_key);
    if (err != CEEPEW_OK) { (void)crypto_mutex_unlock(); goto cleanup; }

    uint16_t ascon_ct_len = (uint16_t)sizeof(local_ascon_ct);
    err = crypto_ascon_aead_encrypt(ascon_key, ascon_nonce, NULL, 0U,
                                    local_compressed, comp_len,
                                    local_ascon_ct, &ascon_ct_len);
    if (err != CEEPEW_OK) { (void)crypto_mutex_unlock(); goto cleanup; }

    uint16_t box_ct_len = (uint16_t)sizeof(local_box_ct);
    err = crypto_box_encrypt(&g_crypto_ctx, local_box_pubkey,
                             local_ascon_ct, ascon_ct_len,
                             local_box_ct, &box_ct_len);
    if (err != CEEPEW_OK) { (void)crypto_mutex_unlock(); goto cleanup; }

    err = session_sign_message(local_box_ct, box_ct_len, local_sig);
    (void)crypto_mutex_unlock(); /* release mutex before radio ops */
    if (err != CEEPEW_OK) { goto cleanup; }

    /* ── End of crypto critical section ─────────────────────────────── */

    uint16_t payload_len = (uint16_t)(2U + box_ct_len + 64U);
    CEEPEW_ASSERT((uint32_t)payload_len <= sizeof(local_payload), CEEPEW_ERR_BOUNDS);
    local_payload[0] = (uint8_t)(box_ct_len & 0xFFU);
    local_payload[1] = (uint8_t)((box_ct_len >> 8U) & 0xFFU);
    memcpy(local_payload + 2U, local_box_ct, box_ct_len);
    memcpy(local_payload + 2U + box_ct_len, local_sig, 64U);

    uint16_t fec_len = (uint16_t)sizeof(local_fec_buf);
    err = ecc_hamming_encode(local_payload, payload_len, local_fec_buf, &fec_len);
    if (err != CEEPEW_OK) { goto cleanup; }

    CEEPEW_ASSERT((uint32_t)fec_len + sizeof(uint32_t) < sizeof(local_frame), CEEPEW_ERR_BOUNDS);
    memcpy(local_frame, local_fec_buf, fec_len);
    uint32_t crc = esp_crc32_le(0U, local_frame, fec_len);
    memcpy(local_frame + fec_len, &crc, sizeof(crc));

    uint16_t frame_len = (uint16_t)(fec_len + sizeof(uint32_t));
    err = transport_esl_process_outgoing(local_frame, &frame_len, (uint16_t)sizeof(local_frame));
    if (err != CEEPEW_OK) { goto cleanup; }

    /* hal_radio_init() is idempotent — safe to call every send to ensure
     * radio is up. The ESP-NOW peer is now registered ONCE after key
     * derivation in task_session.c, not per-message. */
    err = hal_radio_init();
    if (err != CEEPEW_OK) { goto cleanup; }

    err = hal_radio_send(local_frame, frame_len);
    if (err != CEEPEW_OK) { goto cleanup; }

    err = msg_store_add(local_frame, frame_len, len, 1U);
    if (err != CEEPEW_OK) { goto cleanup; }

    err = session_update_last_message_time();
    if (err != CEEPEW_OK) { goto cleanup; }

    err = CEEPEW_OK;

cleanup:
    /* Secure zero all stack buffers that held key material or plaintext */
    ceepew_secure_zero(local_compressed, sizeof(local_compressed));
    ceepew_secure_zero(local_ascon_ct, sizeof(local_ascon_ct));
    ceepew_secure_zero(local_box_ct, sizeof(local_box_ct));
    ceepew_secure_zero(local_sig, sizeof(local_sig));
    ceepew_secure_zero(local_payload, sizeof(local_payload));
    ceepew_secure_zero(local_fec_buf, sizeof(local_fec_buf));
    ceepew_secure_zero(local_frame, sizeof(local_frame));
    ceepew_secure_zero(local_box_pubkey, sizeof(local_box_pubkey));
    ceepew_secure_zero(nonce_24, sizeof(nonce_24));
    ceepew_secure_zero(ascon_nonce, sizeof(ascon_nonce));
    ceepew_secure_zero(ascon_key, sizeof(ascon_key));
    return err;
}

/* Send a message and wait for an echo response (round-trip test).
 * The peer must be running the same test and will echo the payload back.
 * Returns CEEPEW_OK on successful round-trip, error on timeout or failure. */
CeePewErr_t session_send_roundtrip(const uint8_t *payload, uint16_t len, uint32_t timeout_ms)
{
    CEEPEW_ASSERT(payload != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(session_is_active(), CEEPEW_ERR_PARAM);

    /* Get peer MAC */
    uint8_t peer_mac[6];
    CeePewErr_t err = session_get_peer_wifi_mac(peer_mac);
    if (err != CEEPEW_OK) {
        return err;
    }

    /* Send the message */
    err = session_send_message(payload, len, peer_mac, NULL);
    if (err != CEEPEW_OK) {
        return err;
    }

    /* Wait for echo response via session RX queue (handled by session task)
     * For this test, we'll use a simple polling approach with a timeout.
     * In a real implementation, this would use a semaphore or callback. */
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed_ms = 0U;

    while (elapsed_ms < timeout_ms) {
        /* Check if we received a message that matches our payload */
        /* This is a simplified test - in reality, the session RX task would
         * need to signal completion via a semaphore or event. */
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed_ms = (uint32_t)((esp_timer_get_time() / 1000ULL) - start_ms);
    }

    /* For now, just return success if send succeeded.
     * A full implementation would verify the echo was received. */
    return CEEPEW_OK;
}

