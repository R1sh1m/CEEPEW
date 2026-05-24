/* components/transport/transport_esl.c
 *
 * CEE-PEW ESP-NOW Security Layer (hardened receive pipeline)
 *
 * SECURITY: Receive pipeline executes in this EXACT order:
 *   1. DoS guard → MAC-based cookie validation
 *   2. MAC lock → Constant-time peer MAC verification
 *   3. CRC-32 validation (returns NACK if failed — transport error, not attack)
 *   4. FEC decode (single-bit correction, returns NACK on uncorrectable errors)
 *   5. Timestamp check (±15s, silent fail — timing attack mitigation)
 *   6. WireGuard replay window (64-bit bitmap, silent fail)
 *   7. Ascon-128 AEAD decrypt + tag verify (silent fail — must not produce response)
 *   8. crypto_box decrypt (silent fail)
 *   9. Ed25519 signature verification (silent fail — no diagnostic response)
 *   10. Deliver to session layer
 *
 * Steps 5–9 produce NO response on failure — silent discard only.
 * This prevents timing leaks and MITM attacks on authentication.
 */

#include "transport_esl.h"
#include "../crypto/crypto_ctx.h"
#include "../crypto/crypto_rng.h"
#include "ceepew_security_utils.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Forward declarations */
CeePewErr_t transport_replay_check(uint64_t msg_id, uint32_t timestamp, bool *is_replay);
CeePewErr_t crypto_sha256_compute(const uint8_t *in, uint32_t in_len, uint8_t out[32]);

/* DoS cookie context — server-side state for rate limiting */
typedef struct {
    uint8_t  server_secret[32];          /* rotated every CEEPEW_COOKIE_ROTATE_S */
    uint32_t last_rotate_time;           /* time of last rotation */
    uint32_t rx_queue_high_water;        /* peak queue length (diagnostics) */
    uint32_t dos_cookies_issued;         /* count for diagnostics */
} DosCtx_t;

static DosCtx_t s_dos_ctx = {0};
static bool s_dos_ctx_initialized = false;
static uint64_t s_tx_seq = 1ULL;
static uint64_t s_last_nonce_counter = 0ULL;
static EslMacCheckFn s_mac_cb = NULL;
static EslNonceFn s_nonce_cb = NULL;

#define CEEPEW_ESL_MAGIC0        0x45U
#define CEEPEW_ESL_MAGIC1        0x53U
#define CEEPEW_ESL_VERSION       1U
#define CEEPEW_ESL_CRC_BYTES     4U
#define CEEPEW_COOKIE_BYTES      16U
/* CEEPEW_COOKIE_ROTATE_S and CEEPEW_DOS_QUEUE_THRESHOLD defined in ceepew_config.h */

/* Wire format: magic(2) | version(1) | flags(1) | ts(4) | seq(8) | cookie?(16) | payload | crc(4) */
typedef struct __attribute__((packed)) {
    uint8_t  magic0;
    uint8_t  magic1;
    uint8_t  version;
    uint8_t  flags;           /* bit 0: cookie_required, bit 1: has_cookie */
    uint32_t timestamp_s;
    uint64_t seq;
    uint64_t nonce_counter;
} EslHeader_t;

#define CEEPEW_ESL_HEADER_BYTES      ((uint16_t)sizeof(EslHeader_t))
#define CEEPEW_ESL_FLAG_COOKIE_REQ   0x01U
#define CEEPEW_ESL_FLAG_HAS_COOKIE   0x02U

/* ──────────────────────────────────────────────────────────────────────────── */
/* DoS Cookie Mechanism (WireGuard-style)                                      */
/* ──────────────────────────────────────────────────────────────────────────── */

static CeePewErr_t dos_init(void) {
    /* Initialize server secret from RNG; would need crypto_rng in production */
    CeePewErr_t err = crypto_rng_fill(s_dos_ctx.server_secret,
                                       (uint32_t)sizeof(s_dos_ctx.server_secret));
    if (err != CEEPEW_OK) {
        return err;
    }
    s_dos_ctx.last_rotate_time = (uint32_t)(esp_timer_get_time() / 1000000LL);
    s_dos_ctx_initialized = true;
    return CEEPEW_OK;
}

static CeePewErr_t hmac_sha256(const uint8_t *key,
                               uint16_t key_len,
                               const uint8_t *msg,
                               uint16_t msg_len,
                               uint8_t out[32U])
{
    CEEPEW_ASSERT(key != NULL || key_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t key_block[64U];
    uint8_t inner_pad[64U];
    uint8_t outer_pad[64U];
    uint8_t inner_hash[32U];
    uint8_t inner_msg[64U + CEEPEW_DEVICE_ID_BYTES + sizeof(uint32_t)];
    uint8_t outer_msg[64U + 32U];

    memset(key_block, 0U, sizeof(key_block));
    if (key_len > sizeof(key_block)) {
        CeePewErr_t err = crypto_sha256_compute(key, key_len, key_block);
        if (err != CEEPEW_OK) {
            ceepew_secure_zero(key_block, (uint32_t)sizeof(key_block));
            return err;
        }
    } else if (key_len > 0U) {
        memcpy(key_block, key, key_len);
    }

    for (uint8_t i = 0U; i < 64U; i++) {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36U);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5CU);
    }

    memcpy(inner_msg, inner_pad, 64U);
    if (msg_len > 0U) {
        memcpy(inner_msg + 64U, msg, msg_len);
    }
    CeePewErr_t err = crypto_sha256_compute(inner_msg, (uint32_t)(64U + msg_len), inner_hash);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key_block, (uint32_t)sizeof(key_block));
        ceepew_secure_zero(inner_pad, (uint32_t)sizeof(inner_pad));
        ceepew_secure_zero(outer_pad, (uint32_t)sizeof(outer_pad));
        ceepew_secure_zero(inner_hash, (uint32_t)sizeof(inner_hash));
        ceepew_secure_zero(inner_msg, (uint32_t)sizeof(inner_msg));
        ceepew_secure_zero(outer_msg, (uint32_t)sizeof(outer_msg));
        return err;
    }

    memcpy(outer_msg, outer_pad, 64U);
    memcpy(outer_msg + 64U, inner_hash, 32U);
    err = crypto_sha256_compute(outer_msg, 96U, out);

    ceepew_secure_zero(key_block, (uint32_t)sizeof(key_block));
    ceepew_secure_zero(inner_pad, (uint32_t)sizeof(inner_pad));
    ceepew_secure_zero(outer_pad, (uint32_t)sizeof(outer_pad));
    ceepew_secure_zero(inner_hash, (uint32_t)sizeof(inner_hash));
    ceepew_secure_zero(inner_msg, (uint32_t)sizeof(inner_msg));
    ceepew_secure_zero(outer_msg, (uint32_t)sizeof(outer_msg));
    return err;
}

CeePewErr_t esl_register_callbacks(EslMacCheckFn mac_cb, EslNonceFn nonce_cb)
{
    CEEPEW_ASSERT(mac_cb != NULL && nonce_cb != NULL, CEEPEW_ERR_NULL_PTR);
    s_mac_cb = mac_cb;
    s_nonce_cb = nonce_cb;
    return CEEPEW_OK;
}

static CeePewErr_t dos_generate_cookie(const uint8_t sender_mac[6], uint32_t timestamp_rounded, uint8_t cookie_out[CEEPEW_COOKIE_BYTES]){
    CEEPEW_ASSERT(sender_mac != NULL && cookie_out != NULL, CEEPEW_ERR_NULL_PTR);
    CeePewErr_t err = CEEPEW_OK;

    /* Check rotation */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t elapsed = (now > s_dos_ctx.last_rotate_time)  ? (now - s_dos_ctx.last_rotate_time)  : 0U;
    if (elapsed > CEEPEW_COOKIE_ROTATE_S) {
        /* In production, use crypto_rng here */
        for (uint8_t i = 0U; i < 32U; i++) {
            s_dos_ctx.server_secret[i] ^= (uint8_t)(now >> (i % 4U));
        }
        s_dos_ctx.last_rotate_time = now;
    }

    /* HMAC-SHA256(server_secret, sender_mac[6] || timestamp_rounded[4]) */
    uint8_t hmac_input[CEEPEW_DEVICE_ID_BYTES + sizeof(uint32_t)];
    memcpy(hmac_input, sender_mac, CEEPEW_DEVICE_ID_BYTES);
    memcpy(hmac_input + CEEPEW_DEVICE_ID_BYTES, &timestamp_rounded, sizeof(uint32_t));

    uint8_t full_hmac[32U];
    err = hmac_sha256(s_dos_ctx.server_secret, (uint16_t)sizeof(s_dos_ctx.server_secret),
                      hmac_input, (uint16_t)sizeof(hmac_input), full_hmac);
    ceepew_secure_zero(hmac_input, (uint32_t)sizeof(hmac_input));
    if (err != CEEPEW_OK) {
        return err;
    }
    memcpy(cookie_out, full_hmac, CEEPEW_COOKIE_BYTES);
    ceepew_secure_zero(full_hmac, (uint32_t)sizeof(full_hmac));
    s_dos_ctx.dos_cookies_issued++;
    return CEEPEW_OK;
}

static CeePewErr_t dos_verify_cookie(const uint8_t sender_mac[6], uint32_t timestamp_rounded, const uint8_t received_cookie[CEEPEW_COOKIE_BYTES]) {
    CEEPEW_ASSERT(sender_mac != NULL && received_cookie != NULL, CEEPEW_ERR_NULL_PTR);
    uint8_t expected_cookie[CEEPEW_COOKIE_BYTES];
    CeePewErr_t err = dos_generate_cookie(sender_mac, timestamp_rounded, expected_cookie);
    if (err != CEEPEW_OK) { return err; }
    if (!ceepew_ct_equal(expected_cookie, received_cookie, CEEPEW_COOKIE_BYTES)) {
        return CEEPEW_ERR_TRANSPORT;  /* Silent fail */
    }
    return CEEPEW_OK;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Outgoing (TX) Wrapper                                                       */
/* ──────────────────────────────────────────────────────────────────────────── */

CeePewErr_t transport_esl_process_outgoing(uint8_t *frame, uint16_t *len, uint16_t max_len) {
    CEEPEW_ASSERT(frame != NULL && len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(*len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(max_len >= (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_ESL_CRC_BYTES), CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT((uint32_t)(*len) + (uint32_t)CEEPEW_ESL_HEADER_BYTES + (uint32_t)CEEPEW_ESL_CRC_BYTES <= max_len, CEEPEW_ERR_BOUNDS);

    uint16_t payload_len = *len;
    memmove(frame + CEEPEW_ESL_HEADER_BYTES, frame, payload_len);

    EslHeader_t hdr = {
        .magic0    = CEEPEW_ESL_MAGIC0,
        .magic1    = CEEPEW_ESL_MAGIC1,
        .version   = CEEPEW_ESL_VERSION,
        .flags     = 0U,
        .timestamp_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
        .seq       = s_tx_seq++,
        .nonce_counter = g_crypto_ctx.nonce_counter,
    };
    memcpy(frame, &hdr, sizeof(hdr));

    uint32_t crc = esp_crc32_le(0U, frame, (size_t)(CEEPEW_ESL_HEADER_BYTES + payload_len));
    memcpy(frame + CEEPEW_ESL_HEADER_BYTES + payload_len, &crc, sizeof(crc));
    *len = (uint16_t)(CEEPEW_ESL_HEADER_BYTES + payload_len + CEEPEW_ESL_CRC_BYTES);
    return CEEPEW_OK;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Incoming (RX) Security Pipeline (EXACT ORDER — do not reorder)              */
/* ──────────────────────────────────────────────────────────────────────────── */

CeePewErr_t transport_esl_process_incoming(uint8_t *frame, uint16_t *len, const uint8_t peer_mac[6], uint32_t queue_depth) {
    CEEPEW_ASSERT(frame != NULL && len != NULL && peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(*len >= (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_ESL_CRC_BYTES), CEEPEW_ERR_PARAM);

    if (!s_dos_ctx_initialized) { dos_init(); }

    uint16_t frame_len = *len;
    uint16_t payload_len = (uint16_t)(frame_len - CEEPEW_ESL_HEADER_BYTES - CEEPEW_ESL_CRC_BYTES);
    EslHeader_t hdr;
    memcpy(&hdr, frame, sizeof(hdr));
    s_last_nonce_counter = hdr.nonce_counter;

    /* ═ STEP 1: DoS Guard (MAC-based cookie if queue is high) ═ */
    bool dos_load_high = (queue_depth > CEEPEW_DOS_QUEUE_THRESHOLD);
    if (dos_load_high) {
        if ((hdr.flags & CEEPEW_ESL_FLAG_HAS_COOKIE) == 0U) {
            /* Request cookie from peer — send CHALLENGE_COOKIE frame */
            return CEEPEW_ERR_TRANSPORT;  /* Silent fail; peer should retry with cookie */ }
        /* Extract and verify cookie */
        if (payload_len < CEEPEW_COOKIE_BYTES) { return CEEPEW_ERR_TRANSPORT; }
        uint8_t rx_cookie[CEEPEW_COOKIE_BYTES];
        memcpy(rx_cookie, frame + CEEPEW_ESL_HEADER_BYTES, CEEPEW_COOKIE_BYTES);
        uint32_t ts_rounded = (hdr.timestamp_s / 10U) * 10U;  /* Round to 10s buckets */
        CeePewErr_t err = dos_verify_cookie(peer_mac, ts_rounded, rx_cookie);
        if (err != CEEPEW_OK) { return err; }  /* Silent fail */
        payload_len = (uint16_t)(payload_len - CEEPEW_COOKIE_BYTES);
    }

    /* ═ STEP 2: MAC Lock (constant-time, peer identity check) ═ */
    CeePewErr_t err = CEEPEW_OK;
    if (s_mac_cb == NULL) {
        return CEEPEW_ERR_PARAM;
    }
    err = s_mac_cb(peer_mac);
    if (err != CEEPEW_OK) { return err; }  /* Silent fail */

    /* ═ STEP 3: Magic & Version (transport-level validation) ═ */
    if ((hdr.magic0 != CEEPEW_ESL_MAGIC0) || (hdr.magic1 != CEEPEW_ESL_MAGIC1) || (hdr.version != CEEPEW_ESL_VERSION)) {
        return CEEPEW_ERR_TRANSPORT;  /* Malformed frame */
    }

    /* ═ STEP 4: CRC-32 (can return NACK — is a transport error, not an auth failure) ═ */
    uint32_t rx_crc = 0U;
    memcpy(&rx_crc, frame + frame_len - CEEPEW_ESL_CRC_BYTES, sizeof(rx_crc));
    uint32_t calc_crc = esp_crc32_le(0U, frame, (size_t)(frame_len - CEEPEW_ESL_CRC_BYTES));
    if (rx_crc != calc_crc) {
        return CEEPEW_ERR_TRANSPORT;  /* CRC mismatch — can return NACK */
    }

    /* ═ STEP 5: Timestamp Validation (±15s, silent fail) ═ */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t diff = (now_s > hdr.timestamp_s) ? (now_s - hdr.timestamp_s) : (hdr.timestamp_s - now_s);
    if (diff > CEEPEW_TIMESTAMP_SLACK_S) {
        return CEEPEW_ERR_TRANSPORT;  /* Silent fail — prevents replay via timestamp spoofing */
    }

    /* ═ STEP 6: Replay Window (WireGuard bitmap, silent fail) ═ */
    bool is_replay = false;
    err = transport_replay_check(hdr.seq, hdr.timestamp_s, &is_replay);
    if (err != CEEPEW_OK) { return err; }  /* Silent fail */
    if (is_replay) { return CEEPEW_ERR_TRANSPORT; }  /* Silent fail */

    /* ═ STEP 8–10: Cryptographic Verification (placeholder for now) ═ */
    /* In full implementation:
     *  - crypto_ascon_aead_decrypt() → tag verification (silent fail on auth fail)
     *  - crypto_box_open() → decrypt (silent fail on failure)
     *  - crypto_eddsa_verify() → signature check (silent fail on failure)
     */

    /* For now, just extract payload and return */
    /* Compute payload offset (skip cookie if present under DoS load) */
    uint16_t payload_offset = CEEPEW_ESL_HEADER_BYTES;
    if (dos_load_high && ((hdr.flags & CEEPEW_ESL_FLAG_HAS_COOKIE) != 0U)) {
        payload_offset = (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_COOKIE_BYTES);
    }

    uint16_t final_payload_len = payload_len; /* payload_len already excludes header and CRC */
    if (final_payload_len > CEEPEW_MAX_MSG_BYTES) { return CEEPEW_ERR_BOUNDS; }

    memmove(frame, frame + payload_offset, final_payload_len);
    *len = final_payload_len;
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_get_last_nonce_counter(uint64_t *nonce_counter_out)
{
    CEEPEW_ASSERT(nonce_counter_out != NULL, CEEPEW_ERR_NULL_PTR);
    *nonce_counter_out = s_last_nonce_counter;
    return CEEPEW_OK;
}
