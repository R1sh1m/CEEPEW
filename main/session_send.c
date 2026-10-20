/* main/session_send.c
 *
 * End-to-end TX wrapper for a single plaintext message.
 * With fragmentation: compress once → split compressed stream into fragments
 * → each fragment: [frag_hdr][chunk] → encrypt → FEC → ESL → ARQ send.
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
#include "hal_rgb.h"
#include "session_msgstore.h"
#include "task_session.h"
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

#if CONFIG_CEEPEW_BENCH_58B
/* Bench-only forged-frame arming (see session_fsm.h). Consumed once. */
static bool s_bench_forge_armed = false;

void session_send_bench_forge_next(void)
{
    s_bench_forge_armed = true;
}

/* Bench-only replay-injection arming (see session_fsm.h). Consumed once.
 * Armed frames emit the previously saved ESL-wrapped blob verbatim at
 * stage_tx_esl_wrap, carrying a stale ESL msg_id but a fresh ARQ seq
 * (assigned downstream in ecc_arq_send), so the peer's replay window
 * rejects it as CEEPEW_ERR_REPLAY. */
static bool     s_bench_replay_armed = false;
static uint16_t s_bench_replay_len   = 0U;
static uint8_t  s_bench_replay_buf[256];   /* static: zero-heap rule */

void session_send_bench_replay_next(void)
{
    s_bench_replay_armed = true;
}
#endif

/* ────────────────────────────────────────────────────────────────────────── */
/* TX Pipeline Stage Functions                                              */
/* ────────────────────────────────────────────────────────────────────────── */

/* Pending ESL header handoff between the encrypt and esl_wrap stages.
 * stage_tx_encrypt prepares the 24-byte header (allocating the tx seq) and
 * binds it as Ascon-128 AEAD associated data; stage_tx_esl_wrap then wraps the
 * frame with the SAME bytes (no second seq allocation). The two stages run
 * back-to-back within a single pipeline_run() on the single TX task, so this
 * static is not shared concurrently. */
static uint8_t s_tx_pending_hdr[CEEPEW_ESL_HEADER_BYTES];
static bool s_tx_pending_hdr_valid = false;

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

    /* Invalidate any header from a previous (failed) attempt; only a header
     * successfully prepared in this stage may be used by stage_tx_esl_wrap. */
    s_tx_pending_hdr_valid = false;

#if CEEPEW_CAPTURE_ESL_BYPASS
    /* Capture bench Modes A/B: application crypto (Ascon/box/sign) is
     * bypassed — the fragment payload continues in cleartext. No nonce is
     * consumed and no key material is touched. */
    uint8_t *plain_out = (uint8_t *)region_alloc(region, in_len);
    if (plain_out == NULL) { return CEEPEW_ERR_ALLOC; }
    memcpy(plain_out, in, in_len);
    *out = plain_out;
    *out_len = in_len;
    return CEEPEW_OK;
#else
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

    /* Prepare the ESL header now — before encryption — so its 24 bytes can be
     * bound as Ascon-128 AEAD associated data. session_get_nonce() has already
     * run enforce_nonce_limit(), so session_get_nonce_counter() is the
     * post-increment value embedded in the nonce; the header's nonce_counter
     * therefore matches the crypto nonce. stage_tx_esl_wrap reuses these exact
     * bytes (no second seq allocation). */
    err = transport_esl_prepare_header(s_tx_pending_hdr, session_get_nonce_counter());
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] ESL header prepare failed: err=%d", (int)err);
        (void)crypto_mutex_unlock();
        goto zero_keys;
    }
    s_tx_pending_hdr_valid = true;

    /* Ascon-128 AEAD encrypt — the ESL header is authenticated associated data */
    uint8_t ascon_nonce[16U];
    memcpy(ascon_nonce, nonce_24, sizeof(ascon_nonce));

    uint8_t local_ascon_ct[CRYPTO_BOX_INNER_MAX_BYTES];
    uint16_t ascon_ct_len = (uint16_t)sizeof(local_ascon_ct);
    err = crypto_ascon_aead_encrypt(ascon_key, ascon_nonce,
                                    s_tx_pending_hdr, CEEPEW_ESL_HEADER_BYTES,
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

#if CONFIG_CEEPEW_BENCH_58B
    /* Bench-only forged frame: corrupt one outer-ciphertext byte after
     * encryption (before signing), so the peer's fresh-seq frame fails at
     * box HMAC and exercises the forged-frame reject path over the air. */
    if (s_bench_forge_armed) {
        s_bench_forge_armed = false;
        if (box_ct_len > 0U) {
            local_box_ct[box_ct_len - 1U] ^= 0x01U;
            ESP_LOGI(TAG, "[BENCH58_FORGE] armed frame corrupted (1 box byte)");
        }
    }
#endif

    /* Ed25519 sign */
    uint8_t sig[64U];
#if CONFIG_CEEPEW_BENCH_58B
    int64_t t_bench_sign_us = esp_timer_get_time();
#endif
    err = session_sign_message(local_box_ct, box_ct_len, sig);
#if CONFIG_CEEPEW_BENCH_58B
    ESP_LOGI(TAG, "[BENCHTX] sign_us=%lu err=%d",
             (unsigned long)(esp_timer_get_time() - t_bench_sign_us), (int)err);
#endif
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

    ESP_LOGD(TAG, "[SECURE_CHAT_TX] Encrypt & sign complete: ascon_ct=%u B, box_ct=%u B, frame=%u B, nonce=%llu",
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
#endif /* CEEPEW_CAPTURE_ESL_BYPASS */
}

/* Stage 3: frame_fec — Hamming(15,11) encode + CRC-32 append */
static CeePewErr_t stage_tx_frame_fec(Region_t *region, const uint8_t *in,
                                       uint16_t in_len, uint8_t **out,
                                       uint16_t *out_len, void *stage_ctx)
{
    (void)stage_ctx;
    CEEPEW_ASSERT(region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* FEC encode (Hamming) unless the reliability ablation disables it */
    uint8_t *fec_buf = (uint8_t *)region_alloc(region, CEEPEW_FEC_BUF_MAX);
    if (fec_buf == NULL) { return CEEPEW_ERR_ALLOC; }

    uint16_t fec_len = (uint16_t)CEEPEW_FEC_BUF_MAX;
#if CEEPEW_FEC_ENABLED
    CeePewErr_t err = ecc_hamming_encode(in, in_len, fec_buf, &fec_len);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Hamming(15,11) FEC encode failed: err=%d", (int)err);
        return err;
    }
#else
    if ((uint32_t)in_len + sizeof(uint32_t) > CEEPEW_FEC_BUF_MAX) { return CEEPEW_ERR_BOUNDS; }
    memcpy(fec_buf, in, in_len);
    fec_len = in_len;
#endif

    /* Append trailer after the (optionally FEC-encoded) data.
     *
     * Capture bench Modes A/B (CEEPEW_CAPTURE_ESL_BYPASS): trailer is
     * [pre-FEC payload length : 2 B LE][CRC-32 : 4 B], with the CRC covering
     * the FEC data AND the length field so the length is authenticated.
     *
     * The Hamming(15,11) codec is NOT length-inverse: the encoder picks
     * ceil(in_bits/11) codewords and then rounds the output up to whole
     * bytes, while the decoder derives its codeword count from
     * floor(in_bytes*8/15). For a 4-byte payload that means encode -> 6 B but
     * decode -> 5 B, and the original length cannot be recovered from the
     * encoded size (f(3) == f(4) == 6). In the full stack the stray trailing
     * byte lands AFTER the Ed25519 signature (sig_off = 2 + box_ct_len, read
     * from the front) and is therefore harmless. In the ESL-bypass path it
     * lands INSIDE the fragment chunk, inflating chunk_len by one byte and
     * corrupting the Huffman-decompressed length, so 1-byte control frames
     * (HELLO/ACK/PING/PONG) no longer decode to exactly 1 B and the
     * post-derive sync barrier never clears. Carry the exact length instead. */
#if CEEPEW_CAPTURE_ESL_BYPASS
    uint16_t frame_len = (uint16_t)(fec_len + sizeof(uint16_t) + sizeof(uint32_t));
    if ((uint32_t)frame_len > CEEPEW_FEC_BUF_MAX) { return CEEPEW_ERR_BOUNDS; }
    fec_buf[fec_len] = (uint8_t)(in_len & 0xFFU);
    fec_buf[fec_len + 1U] = (uint8_t)((in_len >> 8U) & 0xFFU);
    uint32_t crc = esp_crc32_le(0U, fec_buf, (uint32_t)fec_len + sizeof(uint16_t));
    memcpy(fec_buf + fec_len + sizeof(uint16_t), &crc, sizeof(crc));
#else
    /* Append CRC-32 after the (optionally FEC-encoded) data */
    uint32_t crc = esp_crc32_le(0U, fec_buf, fec_len);
    uint16_t frame_len = (uint16_t)(fec_len + sizeof(uint32_t));
    if ((uint32_t)frame_len > CEEPEW_FEC_BUF_MAX) { return CEEPEW_ERR_BOUNDS; }
    memcpy(fec_buf + fec_len, &crc, sizeof(crc));
#endif /* CEEPEW_CAPTURE_ESL_BYPASS */

    ESP_LOGD(TAG, "[SECURE_CHAT_TX] FEC(%s) & CRC-32 encode: in=%u B -> frame=%u B (crc=0x%08X)",
             (CEEPEW_FEC_ENABLED ? "on" : "off"), (unsigned)in_len, (unsigned)frame_len, (unsigned)crc);

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

#if CEEPEW_CAPTURE_ESL_BYPASS
    /* Capture bench Modes A/B: ESL header/CRC wrap is bypassed — the frame
     * leaves as the raw FEC-encoded payload (cleartext ESP-NOW payload). */
    uint8_t *raw_buf = (uint8_t *)region_alloc(region, in_len);
    if (raw_buf == NULL) { return CEEPEW_ERR_ALLOC; }
    memcpy(raw_buf, in, in_len);
    *out = raw_buf;
    *out_len = in_len;
    return CEEPEW_OK;
#else
    /* Copy input payload to the start of a max-size buffer. The ESL function
     * will move it to make room for the header, then append the ESL CRC. */
    if (!s_tx_pending_hdr_valid) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] ESL wrap: no prepared header (encrypt stage did not run)");
        return CEEPEW_ERR_INTERNAL;
    }
    uint8_t *buf = (uint8_t *)region_alloc(region, CEEPEW_PACKET_MAX_BYTES);
    if (buf == NULL) { return CEEPEW_ERR_ALLOC; }
    memcpy(buf, in, in_len);

    uint16_t wrapped_len = in_len;
    CeePewErr_t err = transport_esl_process_outgoing_with_header(buf, &wrapped_len,
                                                                  CEEPEW_PACKET_MAX_BYTES,
                                                                  s_tx_pending_hdr);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] ESL wrap failed: err=%d", (int)err);
        return err;
    }
    s_tx_pending_hdr_valid = false;

#if CONFIG_CEEPEW_BENCH_58B
    /* Bench-only replay hook: on an armed frame, emit the previously saved
     * ESL-wrapped blob verbatim (stale ESL header, fresh ARQ seq downstream)
     * so the peer's replay window rejects it as CEEPEW_ERR_REPLAY. Otherwise
     * save the freshly wrapped blob as the source for a later replay. */
    if (s_bench_replay_armed && s_bench_replay_len > 0U) {
        memcpy(buf, s_bench_replay_buf, s_bench_replay_len);
        wrapped_len = s_bench_replay_len;
        s_bench_replay_armed = false;
        ESP_LOGI(TAG, "[BENCH58_REPLAY] replayed %u B with stale esl header (fresh arq seq downstream)",
                 (unsigned)wrapped_len);
    } else {
        s_bench_replay_armed = false;
        if (wrapped_len <= (uint16_t)sizeof(s_bench_replay_buf)) {
            memcpy(s_bench_replay_buf, buf, wrapped_len);
            s_bench_replay_len = wrapped_len;
        } else {
            ESP_LOGE(TAG, "[BENCH58_REPLAY] wrapped frame %u B exceeds replay buffer %zu, not saved",
                     (unsigned)wrapped_len, sizeof(s_bench_replay_buf));
        }
    }
#endif

    ESP_LOGD(TAG, "[SECURE_CHAT_TX] ESL frame wrapped (header bound as AEAD AD): total=%u B",
             (unsigned)wrapped_len);

    *out = buf;
    *out_len = wrapped_len;
    return CEEPEW_OK;
#endif /* CEEPEW_CAPTURE_ESL_BYPASS */
}

/* Per-fragment pipeline: encrypt → frame_fec → esl_wrap (no compress) */
static Pipeline_t s_tx_frag_pipeline;
static bool s_tx_frag_pipeline_built = false;

static CeePewErr_t session_send_build_frag_pipeline(void)
{
    CeePewErr_t err;

    err = pipeline_reset(&s_tx_frag_pipeline);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_tx_frag_pipeline, stage_tx_encrypt, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_tx_frag_pipeline, stage_tx_frame_fec, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_tx_frag_pipeline, stage_tx_esl_wrap, NULL);
    if (err != CEEPEW_OK) { return err; }

    s_tx_frag_pipeline_built = true;
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

    /* Detect control/handshake messages (single-byte, no fragmentation) */
    bool is_handshake = (len == 1U && (plaintext[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ||
                                        plaintext[0] == CEEPEW_KEY_SYNC_ACK_BYTE ||
                                        plaintext[0] == CEEPEW_KEY_SYNC_PING_BYTE ||
                                        plaintext[0] == CEEPEW_KEY_SYNC_PONG_BYTE));

    /* ── Step 1: Compress the full message once ─────────────────────────── */
    uint8_t *compressed = (uint8_t *)region_alloc(&g_region, CEEPEW_HUFF_BUF_MAX);
    if (compressed == NULL) { return CEEPEW_ERR_ALLOC; }

    uint16_t comp_len = CEEPEW_HUFF_BUF_MAX;
    CeePewErr_t err = compress_huffman_compress(plaintext, len, compressed, &comp_len,
                                                CEEPEW_HUFF_BUF_MAX, NULL);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Huffman compression failed: err=%d", (int)err);
        region_reset(&g_region);
        return err;
    }

    float ratio = (len > 0U) ? ((float)comp_len / (float)len * 100.0f) : 100.0f;
    ESP_LOGD(TAG, "[SECURE_CHAT_TX] Huffman compress: in=%u B -> out=%u B (ratio=%.1f%%)",
             (unsigned)len, (unsigned)comp_len, (double)ratio);

    /* ── Step 2: Fragment compressed stream ─────────────────────────────── */
    uint8_t total_frags = (uint8_t)((comp_len + CEEPEW_FRAG_DATA_MAX - 1U) / CEEPEW_FRAG_DATA_MAX);
    if (total_frags == 0U) { total_frags = 1U; }
    if (total_frags > CEEPEW_MAX_FRAGMENTS) {
        ESP_LOGE(TAG, "[SECURE_CHAT_TX] Too many fragments: %u > %u", total_frags, CEEPEW_MAX_FRAGMENTS);
        region_reset(&g_region);
        return CEEPEW_ERR_BOUNDS;
    }

    ESP_LOGI(TAG, "[SECURE_CHAT_TX] Fragmenting: %u compressed bytes -> %u fragments",
             (unsigned)comp_len, (unsigned)total_frags);

    /* Build per-fragment pipeline on first use */
    if (!s_tx_frag_pipeline_built) {
        err = session_send_build_frag_pipeline();
        if (err != CEEPEW_OK) {
            ESP_LOGE(TAG, "[SECURE_CHAT_TX] Failed to build fragment pipeline: %d", (int)err);
            region_reset(&g_region);
            return err;
        }
    }

    /* ── Step 3: Send each fragment sequentially ────────────────────────── */
    uint16_t offset = 0U;
    for (uint8_t frag_idx = 0U; frag_idx < total_frags; frag_idx++) {
        uint16_t chunk_len = (uint16_t)((comp_len - offset > CEEPEW_FRAG_DATA_MAX)
                                         ? CEEPEW_FRAG_DATA_MAX
                                         : (comp_len - offset));
        
        /* Build fragment payload: [flags|total-1][idx][chunk] */
        uint16_t frag_payload_len = (uint16_t)(CEEPEW_FRAG_HEADER_BYTES + chunk_len);
        uint8_t *frag_in = (uint8_t *)region_alloc(&g_region, frag_payload_len);
        if (frag_in == NULL) {
            region_reset(&g_region);
            return CEEPEW_ERR_ALLOC;
        }

        /* Fragment header: START bit (0x80) only on the FIRST fragment;
         * continuation fragments carry flags = total-1 without 0x80 so the
         * receiver's reassembly treats them as continuations, not new starts. */
        frag_in[0] = (uint8_t)(((frag_idx == 0U) ? 0x80U : 0x00U) |
                               ((total_frags - 1U) & 0x3FU)); /* START bit + total-1 (6 bits) */
        frag_in[1] = frag_idx;                                         /* fragment index */
        if (chunk_len > 0U) {
            memcpy(frag_in + CEEPEW_FRAG_HEADER_BYTES, compressed + offset, chunk_len);
        }

        ESP_LOGD(TAG, "[SECURE_CHAT_TX] Fragment %u/%u: chunk=%u B, frag_in=%u B",
                 (unsigned)(frag_idx + 1U), (unsigned)total_frags,
                 (unsigned)chunk_len, (unsigned)frag_payload_len);

        /* Run per-fragment pipeline: encrypt → fec → esl */
        uint8_t *final_frame = NULL;
        uint16_t final_len = 0U;
        err = pipeline_run(&s_tx_frag_pipeline, &g_region,
                            frag_in, frag_payload_len,
                            &final_frame, &final_len);
        if (err != CEEPEW_OK) {
            ESP_LOGE(TAG, "[SECURE_CHAT_TX] Fragment %u pipeline failed: err=%d",
                     (unsigned)(frag_idx + 1U), (int)err);
            /* Roll back the nonce that session_enforce_nonce_limit() pre-incremented
             * inside stage_tx_encrypt. The frame was NEVER sent.
             * Capture bench Modes A/B: the encrypt stage is bypassed and no
             * nonce was consumed, so there is nothing to roll back. */
#if !CEEPEW_CAPTURE_ESL_BYPASS
            if (session_is_active()) {
                (void)session_rollback_nonce();
            }
#endif
            region_reset(&g_region);
            return err;
        }

        /* Update last message activity timestamp on first fragment send attempt */
        if (frag_idx == 0U) {
            (void)session_update_last_message_time();
        }

        /* Wrap frame with ARQ and send with retry/backoff */
        ESP_LOGD(TAG, "[SECURE_CHAT_TX] Submitting fragment %u/%u (%u B) to ARQ...",
                 (unsigned)(frag_idx + 1U), (unsigned)total_frags, (unsigned)final_len);
        err = ecc_arq_send(peer_mac, final_frame, final_len);
        if (err != CEEPEW_OK) {
            ESP_LOGE(TAG, "[SECURE_CHAT_TX] Fragment %u ARQ failed: err=%d (peer offline or packet dropped)",
                     (unsigned)(frag_idx + 1U), (int)err);
            region_reset(&g_region);
            return err;
        }

        ESP_LOGD(TAG, "[SECURE_CHAT_TX] Fragment %u/%u TX-complete (link ACK only, no peer auth)!",
                 (unsigned)(frag_idx + 1U), (unsigned)total_frags);

        offset += chunk_len;
    }

    ESP_LOGI(TAG, "[SECURE_CHAT_TX] All %u fragments successfully delivered!", (unsigned)total_frags);

    /* Do not store handshake/control sync messages in the message store */
    if (!is_handshake) {
        err = msg_store_add(plaintext, len, 1U);
        if (err != CEEPEW_OK) {
            region_reset(&g_region);
            return err;
        }
        /* Update delivery status to DELIVERED since ecc_arq_send confirmed ARQ ACK */
        uint8_t count = msg_store_count();
        if (count > 0U) {
            (void)msg_store_update_delivery_status(count - 1U, MSG_STATUS_DELIVERED);
        }
        /* Trigger TX LED blink (green blink for 250 ms) */
        task_session_trigger_rgb_blink(RGB_GREEN_BLINK, 250U);
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

    /* Raw microsecond timebase: esp_timer_get_time() is monotonic us.
     * No /1000 quantisation and no 64-bit division on this polling path. */
    uint64_t start_us = (uint64_t)esp_timer_get_time();
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;

    while (((uint64_t)esp_timer_get_time() - start_us) < timeout_us) {
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
    }

    return CEEPEW_ERR_TIMEOUT;
}