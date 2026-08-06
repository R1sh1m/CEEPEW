/* main/session_send.c
 *
 * End-to-end TX wrapper for a single plaintext message.
 * Stages are composed via Pipeline_t: compress → encrypt → frame_fec → esl_wrap.
 * Each stage allocates its output from the region allocator.
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
#include "ceepew_pipeline.h"
#include "ceepew_region.h"
#include "crypto_ctx.h"
#include "crypto_box_wrap.h"
#include "crypto_ascon.h"
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

/* ARQ send — declared in components/ecc/ecc_arq.c, no header file. */
extern CeePewErr_t ecc_arq_send(const uint8_t peer_mac[6],
                                const uint8_t *payload, uint16_t payload_len);

/* ────────────────────────────────────────────────────────────────────────── */
/* TX Pipeline Stage Functions                                              */
/* ────────────────────────────────────────────────────────────────────────── */

/* Stage 1: compress — wraps compress_huffman_compress with region output */
static CeePewErr_t stage_tx_compress(Region_t *region, const uint8_t *in,
                                     uint16_t in_len, uint8_t **out,
                                     uint16_t *out_len, void *stage_ctx)
{
    (void)stage_ctx;
    CEEPEW_ASSERT(region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t *buf = (uint8_t *)region_alloc(region, CEEPEW_HUFF_BUF_MAX);
    if (buf == NULL) { return CEEPEW_ERR_ALLOC; }

    uint16_t comp_len = CEEPEW_HUFF_BUF_MAX;
    CeePewErr_t err = compress_huffman_compress(in, in_len, buf, &comp_len,
                                                CEEPEW_HUFF_BUF_MAX, NULL);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Huffman compression failed: err=%d", (int)err);
        return err;
    }

    float ratio = (in_len > 0U) ? ((float)comp_len / (float)in_len * 100.0f) : 100.0f;
    ESP_LOGI(TAG, "[SECURE_CHAT_TX] Huffman compress: in=%u B -> out=%u B (ratio=%.1f%%)",
             (unsigned)in_len, (unsigned)comp_len, (double)ratio);

    *out = buf;
    *out_len = comp_len;
    return CEEPEW_OK;
}

/* Stage 2: encrypt — mutex-locked ascon → box → sign, produces framed payload.
 * Output layout: [2-byte LE box_ct_len][box_ct][64-byte Ed25519 sig].
 * All intermediate key material is stack-resident and securely zeroed. */
static CeePewErr_t stage_tx_encrypt(Region_t *region, const uint8_t *in,
                                    uint16_t in_len, uint8_t **out,
                                    uint16_t *out_len, void *stage_ctx)
{
    (void)stage_ctx;
    CEEPEW_ASSERT(region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* ── Lock crypto mutex for the entire encrypt+sign sequence ──────── */
    CeePewErr_t err = crypto_mutex_lock();
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Encrypt stage: failed to acquire crypto mutex");
        return CEEPEW_ERR_BUSY;
    }

    /* Enforce nonce limit while holding mutex */
    err = session_enforce_nonce_limit();
    if (err == CEEPEW_ERR_NONCE_EXHAUSTED) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Nonce limit exhausted — session keys locked!");
        (void)crypto_mutex_unlock();
        return err;
    }
    if (err != CEEPEW_OK && err != CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Nonce check error: %d", (int)err);
        (void)crypto_mutex_unlock();
        return err;
    }

    /* Snapshot peer X25519 key under mutex */
    uint8_t local_box_pubkey[32U];
    memcpy(local_box_pubkey, g_crypto_ctx.peer_box_pubkey, 32U);

    /* Get nonce and session key */
    uint8_t nonce_24[24U];
    err = session_get_nonce(nonce_24);
    if (err != CEEPEW_OK) { (void)crypto_mutex_unlock(); return err; }

    uint8_t ascon_key[16U];
    err = session_get_session_key(ascon_key);
    if (err != CEEPEW_OK) { (void)crypto_mutex_unlock(); return err; }

    /* Ascon-128 AEAD encrypt */
    uint8_t ascon_nonce[16U];
    memcpy(ascon_nonce, nonce_24, sizeof(ascon_nonce));

    uint8_t local_ascon_ct[CRYPTO_BOX_INNER_MAX_BYTES];
    uint16_t ascon_ct_len = (uint16_t)sizeof(local_ascon_ct);
    err = crypto_ascon_aead_encrypt(ascon_key, ascon_nonce, NULL, 0U,
                                    in, in_len,
                                    local_ascon_ct, &ascon_ct_len);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Ascon-128 AEAD encrypt failed: err=%d", (int)err);
        (void)crypto_mutex_unlock();
        goto zero_keys;
    }

    /* crypto_box encrypt */
    uint8_t local_box_ct[CEEPEW_MAX_MSG_BYTES + 64U];
    uint16_t box_ct_len = (uint16_t)sizeof(local_box_ct);
    err = crypto_box_encrypt(&g_crypto_ctx, local_box_pubkey,
                             local_ascon_ct, ascon_ct_len,
                             local_box_ct, &box_ct_len);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Crypto box encrypt failed: err=%d", (int)err);
        (void)crypto_mutex_unlock();
        goto zero_intermediate;
    }

    /* Ed25519 sign */
    uint8_t sig[64U];
    err = session_sign_message(local_box_ct, box_ct_len, sig);
    (void)crypto_mutex_unlock(); /* mutex released before radio/FEC stages */
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Ed25519 sign failed: err=%d", (int)err);
        goto zero_intermediate;
    }

    /* Build output frame: [2-byte box_ct_len][box_ct][sig] */
    uint16_t payload_len = (uint16_t)(2U + box_ct_len + 64U);
    uint8_t *payload = (uint8_t *)region_alloc(region, payload_len);
    if (payload == NULL) {
        ceepew_secure_zero(local_box_ct, sizeof(local_box_ct));
        ceepew_secure_zero(sig, sizeof(sig));
        return CEEPEW_ERR_ALLOC;
    }

    payload[0] = (uint8_t)(box_ct_len & 0xFFU);
    payload[1] = (uint8_t)((box_ct_len >> 8U) & 0xFFU);
    memcpy(payload + 2U, local_box_ct, box_ct_len);
    memcpy(payload + 2U + box_ct_len, sig, 64U);

    *out = payload;
    *out_len = payload_len;

    ESP_LOGI(TAG, "[SECURE_CHAT_TX] Encrypt & sign complete: ascon_ct=%u B, box_ct=%u B, frame=%u B, nonce=%llu",
             (unsigned)ascon_ct_len, (unsigned)box_ct_len, (unsigned)payload_len,
             (unsigned long long)session_get_nonce_counter());

    /* Secure zero all key material and intermediate ciphertext */
    ceepew_secure_zero(local_box_ct, sizeof(local_box_ct));
    ceepew_secure_zero(sig, sizeof(sig));
zero_intermediate:
    ceepew_secure_zero(local_ascon_ct, sizeof(local_ascon_ct));
zero_keys:
    ceepew_secure_zero(ascon_key, sizeof(ascon_key));
    ceepew_secure_zero(ascon_nonce, sizeof(ascon_nonce));
    ceepew_secure_zero(nonce_24, sizeof(nonce_24));
    ceepew_secure_zero(local_box_pubkey, sizeof(local_box_pubkey));
    return err;
}

/* Stage 3: frame_fec — Hamming(15,11) encode + CRC-32 append */
static CeePewErr_t stage_tx_frame_fec(Region_t *region, const uint8_t *in,
                                       uint16_t in_len, uint8_t **out,
                                       uint16_t *out_len, void *stage_ctx)
{
    (void)stage_ctx;
    CEEPEW_ASSERT(region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* FEC encode */
    uint8_t *fec_buf = (uint8_t *)region_alloc(region, CEEPEW_FEC_BUF_MAX);
    if (fec_buf == NULL) { return CEEPEW_ERR_ALLOC; }

    uint16_t fec_len = CEEPEW_FEC_BUF_MAX;
    CeePewErr_t err = ecc_hamming_encode(in, in_len, fec_buf, &fec_len);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Hamming(15,11) FEC encode failed: err=%d", (int)err);
        return err;
    }

    /* Append CRC-32 after the FEC data */
    uint32_t crc = esp_crc32_le(0U, fec_buf, fec_len);
    uint16_t frame_len = (uint16_t)(fec_len + sizeof(uint32_t));
    if ((uint32_t)frame_len > CEEPEW_FEC_BUF_MAX) { return CEEPEW_ERR_BOUNDS; }
    memcpy(fec_buf + fec_len, &crc, sizeof(crc));

    ESP_LOGI(TAG, "[SECURE_CHAT_TX] FEC & CRC-32 encode: in=%u B -> fec_crc=%u B (crc=0x%08X)",
             (unsigned)in_len, (unsigned)frame_len, (unsigned)crc);

    *out = fec_buf;
    *out_len = frame_len;
    return CEEPEW_OK;
}

/* Stage 4: esl_wrap — wraps frame with ESL header and ESL CRC.
 * The nonce_counter from the encrypt stage is embedded in the ESL header
 * so the peer's replay window can track it. */
static CeePewErr_t stage_tx_esl_wrap(Region_t *region, const uint8_t *in,
                                      uint16_t in_len, uint8_t **out,
                                      uint16_t *out_len, void *stage_ctx)
{
    (void)stage_ctx;
    CEEPEW_ASSERT(region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* Copy input payload to the start of a max-size buffer. The ESL function
     * will move it to make room for the header, then append the ESL CRC. */
    uint8_t *buf = (uint8_t *)region_alloc(region, CEEPEW_PACKET_MAX_BYTES);
    if (buf == NULL) { return CEEPEW_ERR_ALLOC; }
    memcpy(buf, in, in_len);

    uint64_t nonce_counter = session_get_nonce_counter();
    uint16_t wrapped_len = in_len;
    CeePewErr_t err = transport_esl_process_outgoing(buf, &wrapped_len,
                                                      CEEPEW_PACKET_MAX_BYTES,
                                                      nonce_counter);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] ESL wrap failed: err=%d", (int)err);
        return err;
    }

    ESP_LOGI(TAG, "[SECURE_CHAT_TX] ESL frame wrapped: total=%u B, nonce_counter=%llu",
             (unsigned)wrapped_len, (unsigned long long)nonce_counter);

    *out = buf;
    *out_len = wrapped_len;
    return CEEPEW_OK;
}

/* TX pipeline handle and build-once flag */
static Pipeline_t s_tx_pipeline;
static bool s_tx_pipeline_built = false;

/* Build the TX pipeline once at first use. Called from session_send_message */
static CeePewErr_t session_send_build_tx_pipeline(void)
{
    CeePewErr_t err;

    err = pipeline_reset(&s_tx_pipeline);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_tx_pipeline, stage_tx_compress, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_tx_pipeline, stage_tx_encrypt, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_tx_pipeline, stage_tx_frame_fec, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_tx_pipeline, stage_tx_esl_wrap, NULL);
    if (err != CEEPEW_OK) { return err; }

    s_tx_pipeline_built = true;
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Public API                                                                */
/* ────────────────────────────────────────────────────────────────────────── */

CeePewErr_t session_send_message(const uint8_t *plaintext, uint16_t len,
                                 const uint8_t peer_mac[6],
                                 const uint8_t peer_public_key[32])
{
    CEEPEW_ASSERT(plaintext != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(session_is_active(), CEEPEW_ERR_PARAM);

    ESP_LOGI(TAG, "[SECURE_CHAT_TX] Initiating TX: len=%u bytes, peer_mac=%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)len, peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);

    /* RC#0 guard: the peer's X25519 key must have arrived via BLE before we
     * can encrypt. If it hasn't, the caller passed either NULL or the wrong
     * (Ed25519) key — both produce unreadable ciphertext.  We gate here so
     * the UI gets a clean CEEPEW_ERR_PARAM rather than silently sending
     * garbage the peer can never decrypt. */
    if (!session_peer_box_pubkey_valid()) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] FAILED: peer X25519 box pubkey not yet received");
        return CEEPEW_ERR_PARAM;
    }

    /* Verify ESP-NOW peer is registered before attempting to send. */
    if (!hal_radio_is_peer_registered(peer_mac)) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] FAILED: ESP-NOW peer MAC not registered in radio stack");
        return CEEPEW_ERR_PARAM;
    }

    (void)peer_public_key;

    /* Build pipeline on first use */
    if (!s_tx_pipeline_built) {
        CeePewErr_t err = session_send_build_tx_pipeline();
        if (err != CEEPEW_OK) {
            ESP_LOGE(TAG, "[SECURE_CHAT_TX] Failed to build TX pipeline: %d", (int)err);
            return err;
        }
    }

    /* Run the pipeline: compress → encrypt → frame_fec → esl_wrap. */
    uint8_t *final_frame = NULL;
    uint16_t final_len = 0U;
    CeePewErr_t err = pipeline_run(&s_tx_pipeline, &g_region,
                                    plaintext, len,
                                    &final_frame, &final_len);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Pipeline execution failed: err=%d", (int)err);
        /* Roll back the nonce that session_enforce_nonce_limit() pre-incremented
         * inside stage_tx_encrypt. The frame was NEVER sent (we haven't reached
         * ecc_arq_send yet), so rolling back is safe — the peer hasn't seen this
         * nonce value and the counter remains synchronized. Without this rollback,
         * every TX pipeline failure permanently advances the nonce counter by 2,
         * eventually desynchronizing INIT's and RESP's nonce sequences and causing
         * all subsequent INIT→RESP messages to fail Ascon AEAD verification. */
        if (session_is_active()) {
            (void)session_rollback_nonce();
        }
        region_reset(&g_region);
        return err;
    }


    /* Update last message activity timestamp on send attempt */
    (void)session_update_last_message_time();

    /* Wrap frame with ARQ and send with retry/backoff */
    ESP_LOGI(TAG, "[SECURE_CHAT_TX] Submitting %u B ESL frame to Stop-and-Wait ARQ...", (unsigned)final_len);
    err = ecc_arq_send(peer_mac, final_frame, final_len);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] ARQ transmission failed: err=%d (peer offline or packet dropped)", (int)err);
        region_reset(&g_region);
        return err;
    }

    ESP_LOGI(TAG, "[SECURE_CHAT_TX] Message successfully delivered & ACKed via ARQ!");

    /* Do not store handshake/control sync messages (HELLO/ACK/PING/PONG) in the message store */
    bool is_handshake = (len == 1U && (plaintext[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ||
                                       plaintext[0] == CEEPEW_KEY_SYNC_ACK_BYTE ||
                                       plaintext[0] == CEEPEW_KEY_SYNC_PING_BYTE ||
                                       plaintext[0] == CEEPEW_KEY_SYNC_PONG_BYTE));
    if (!is_handshake) {
        err = msg_store_add(plaintext, len, 1U);
        if (err != CEEPEW_OK) {
            region_reset(&g_region);
            return err;
        }
    }

    err = session_update_last_message_time();

    /* Free transient pipeline allocations from the region pool */
    region_reset(&g_region);
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

    uint32_t start_count = msg_store_count();

    /* Send the message */
    err = session_send_message(payload, len, peer_mac, NULL);
    if (err != CEEPEW_OK) {
        return err;
    }

    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t elapsed_ms = 0U;

    while (elapsed_ms < timeout_ms) {
        uint32_t current_count = msg_store_count();
        if (current_count > start_count) {
            const StoredMsg_t *msg = msg_store_get(current_count - 1);
            if (msg != NULL) {
                if (msg->meta.dir == 0U && msg->meta.payload_len == len &&
                    memcmp(msg->plaintext, payload, len) == 0) {
                    return CEEPEW_OK;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed_ms = (uint32_t)((esp_timer_get_time() / 1000ULL) - start_ms);
    }

    return CEEPEW_ERR_TIMEOUT;
}

