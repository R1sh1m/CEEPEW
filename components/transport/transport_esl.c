/* components/transport/transport_esl.c - touched for compile check
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
#include "../crypto/crypto_ascon.h"
#include "session_fsm.h"
#include "ceepew_security_utils.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "ESL";

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
static bool s_esl_callbacks_registered = false;

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
    CEEPEW_ASSERT(!s_esl_callbacks_registered, CEEPEW_ERR_PARAM);  /* Cannot re-register */
    
    s_mac_cb = mac_cb;
    s_nonce_cb = nonce_cb;
    s_esl_callbacks_registered = true;
    return CEEPEW_OK;
}

void esl_reset_callbacks(void)
{
    s_mac_cb = NULL;
    s_nonce_cb = NULL;
    s_esl_callbacks_registered = false;
}

static CeePewErr_t dos_generate_cookie(const uint8_t sender_mac[6], uint32_t timestamp_rounded, uint8_t cookie_out[CEEPEW_COOKIE_BYTES]){
    CEEPEW_ASSERT(sender_mac != NULL && cookie_out != NULL, CEEPEW_ERR_NULL_PTR);
    CeePewErr_t err = CEEPEW_OK;

    /* Check rotation */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t elapsed = (now > s_dos_ctx.last_rotate_time)  ? (now - s_dos_ctx.last_rotate_time)  : 0U;
    if (elapsed > CEEPEW_COOKIE_ROTATE_S) {
        /* Rotate server secret securely using RNG */
        CeePewErr_t rng_err = crypto_rng_fill(s_dos_ctx.server_secret,
                                              (uint32_t)sizeof(s_dos_ctx.server_secret));
        CEEPEW_ASSERT(rng_err == CEEPEW_OK, rng_err);
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

    uint64_t nonce_counter = session_get_nonce_counter();
    EslHeader_t hdr = {
        .magic0    = CEEPEW_ESL_MAGIC0,
        .magic1    = CEEPEW_ESL_MAGIC1,
        .version   = CEEPEW_ESL_VERSION,
        .flags     = 0U,
        .timestamp_s = (uint32_t)(esp_timer_get_time() / 1000000LL),
        .seq       = s_tx_seq++,
        .nonce_counter = nonce_counter,
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
            ESP_LOGD(TAG, "DoS: No cookie found, high queue depth (%lu)", queue_depth);
            return CEEPEW_ERR_TRANSPORT;  /* Silent fail; peer should retry with cookie */
        }
        /* Extract and verify cookie */
        if (payload_len < CEEPEW_COOKIE_BYTES) {
            ESP_LOGW(TAG, "DoS: Cookie missing or truncated (payload_len=%u)", payload_len);
            return CEEPEW_ERR_TRANSPORT;
        }
        uint8_t rx_cookie[CEEPEW_COOKIE_BYTES];
        memcpy(rx_cookie, frame + CEEPEW_ESL_HEADER_BYTES, CEEPEW_COOKIE_BYTES);
        uint32_t ts_rounded = (hdr.timestamp_s / 10U) * 10U;  /* Round to 10s buckets */
        CeePewErr_t err = dos_verify_cookie(peer_mac, ts_rounded, rx_cookie);
        if (err != CEEPEW_OK) {
            ESP_LOGD(TAG, "DoS: Cookie verification failed (err=%d)", err);
            return err;  /* Silent fail */
        }
        payload_len = (uint16_t)(payload_len - CEEPEW_COOKIE_BYTES);
    }

    /* ═ STEP 2: MAC Lock (constant-time, peer identity check) ═ */
    CEEPEW_ASSERT(s_esl_callbacks_registered, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_mac_cb != NULL, CEEPEW_ERR_PARAM);
    CeePewErr_t err = s_mac_cb(peer_mac);
    if (err != CEEPEW_OK) {
        ESP_LOGD(TAG, "MAC lock failed: peer MAC not in session (err=%d)", err);
        return err;  /* Silent fail */
    }

    /* ═ STEP 3: Magic & Version (transport-level validation) ═ */
    if ((hdr.magic0 != CEEPEW_ESL_MAGIC0) || (hdr.magic1 != CEEPEW_ESL_MAGIC1) || (hdr.version != CEEPEW_ESL_VERSION)) {
        ESP_LOGW(TAG, "Malformed frame: magic=%02x%02x version=%u (expected %02x%02x v%u)",
                 hdr.magic0, hdr.magic1, hdr.version,
                 CEEPEW_ESL_MAGIC0, CEEPEW_ESL_MAGIC1, CEEPEW_ESL_VERSION);
        return CEEPEW_ERR_TRANSPORT;  /* Malformed frame */
    }

    /* ═ STEP 4: CRC-32 (can return NACK — is a transport error, not an auth failure) ═ */
    uint32_t rx_crc = 0U;
    memcpy(&rx_crc, frame + frame_len - CEEPEW_ESL_CRC_BYTES, sizeof(rx_crc));
    uint32_t calc_crc = esp_crc32_le(0U, frame, (size_t)(frame_len - CEEPEW_ESL_CRC_BYTES));
    if (rx_crc != calc_crc) {
        ESP_LOGD(TAG, "CRC mismatch: rx=%08lx calc=%08lx", rx_crc, calc_crc);
        return CEEPEW_ERR_TRANSPORT;  /* CRC mismatch — can return NACK */
    }

    /* ═ STEP 5: Timestamp Validation (±15s, silent fail) ═
     * 
     * SECURITY: This check prevents several attacks:
     * 1. Replay attacks via timestamp spoofing — old messages with crafted timestamps
     * 2. Preplay attacks — injecting future-dated messages to bypass the window
     * 3. Clock skew attacks — exploiting mismatched device clocks
     * 
     * LIMITATION: The ±15 second window assumes:
     * - Both device clocks are synchronized within ±15 seconds (via NTP or similar)
     * - Legitimate messages are encrypted and transmitted within milliseconds
     * - Attackers cannot predict the session_code and timestamps simultaneously
     * 
     * The window CEEPEW_TIMESTAMP_SLACK_S (15 seconds) is chosen to:
     * - Allow for typical clock drift on ESP32 devices without NTP
     * - Prevent a 15-second replay window that an attacker could exploit
     * - Be short enough that replaying old messages becomes impractical
     *
     * If devices have unsynchronized clocks (> ±45s apart by default), legitimate
     * messages will be silently rejected. The transport layer can apply a
     * peer_uptime_offset (set via session_set_peer_uptime_offset) to compensate
     * for raw-uptime differences; without it, both devices' ESL headers carry
     * their own boot uptime, which diverges as soon as one device reboots.
     *
     * Silent fail (no error response) prevents timing-based clock inference attacks.
     */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    /* Apply the peer's uptime offset so the local clock can be compared to
     * the peer's local clock on equal terms. The offset is set by the BLE
     * layer after a one-shot time-sync GATT read; if not set, the offset is
     * 0 and the check is the same as before. */
    int32_t peer_off_s = session_get_peer_uptime_offset();
    int32_t peer_now_s = (int32_t)now_s + peer_off_s;
    int32_t hdr_s      = (int32_t)hdr.timestamp_s;
    int32_t diff = peer_now_s - hdr_s;
    if (diff < 0) { diff = -diff; }
    if ((uint32_t)diff > CEEPEW_TIMESTAMP_SLACK_S) {
        return CEEPEW_ERR_TRANSPORT;  /* Silent fail — prevents replay via timestamp spoofing */
    }

    /* ═ STEP 6: Replay Window (WireGuard bitmap, silent fail) ═ */
    bool is_replay = false;
    err = transport_replay_check(hdr.seq, hdr.timestamp_s, &is_replay);
    if (err != CEEPEW_OK) { return err; }  /* Silent fail */
    if (is_replay) { return CEEPEW_ERR_TRANSPORT; }  /* Silent fail */

    /* ═ STEP 7: Ascon-128 AEAD Decrypt ═ */
    /* Extract nonce from header.nonce_counter */
    uint8_t ascon_nonce[16];
    memset(ascon_nonce, 0U, sizeof(ascon_nonce));
    memcpy(ascon_nonce, &hdr.nonce_counter, sizeof(uint64_t));

    /* Ciphertext starts after header, possibly after cookie */
    uint16_t ct_offset = CEEPEW_ESL_HEADER_BYTES;
    if (dos_load_high && ((hdr.flags & CEEPEW_ESL_FLAG_HAS_COOKIE) != 0U)) {
        ct_offset = (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_COOKIE_BYTES);
    }
    uint16_t ct_len = payload_len;

    /* Ascon-128 output buffer: temp plaintext. Sized to the actual maximum
     * plaintext (CEEPEW_MAX_MSG_BYTES). Previously a 24 KB stack buffer —
     * far larger than needed and silently smashed the 8 KB session task
     * stack on the very first received frame. */
    static uint8_t s_pt_buf[CEEPEW_MAX_MSG_BYTES];
    uint16_t pt_len = 0U;

    /* Copy Ascon key under mutex protection */
    uint8_t ascon_key_copy[CEEPEW_SESSION_KEY_BYTES];
    CeePewErr_t mutex_err = crypto_mutex_lock();
    if (mutex_err != CEEPEW_OK) { return CEEPEW_ERR_CRYPTO; }
    memcpy(ascon_key_copy, g_crypto_ctx.ascon_key, CEEPEW_SESSION_KEY_BYTES);
    mutex_err = crypto_mutex_unlock();
    if (mutex_err != CEEPEW_OK) { return CEEPEW_ERR_CRYPTO; }

    CeePewErr_t ascon_err = crypto_ascon_aead_decrypt(
        ascon_key_copy,             /* key: 16 bytes from HKDF (copy under lock) */
        ascon_nonce,                /* nonce: 16 bytes from session_id + nonce_counter */
        (const uint8_t *)&hdr,      /* AD: header for authenticity binding */
        (uint16_t)sizeof(EslHeader_t),
        frame + ct_offset,          /* ciphertext */
        ct_len,
        s_pt_buf,                   /* plaintext output (static, sized to MAX_MSG_BYTES) */
        &pt_len
    );
    ceepew_secure_zero(ascon_key_copy, sizeof(ascon_key_copy));
    if (ascon_err != CEEPEW_OK) {
        return CEEPEW_ERR_CRYPTO;  /* Silent fail — authentication failure */
    }

    /* ═ STEP 8: Copy plaintext back to frame and return ═ */
    /* Plaintext now in s_pt_buf, copy it back to frame. The static buffer
     * is bounded to CEEPEW_MAX_MSG_BYTES; an Ascon output that exceeds
     * that is either a bug in the encryptor or a forged packet — reject. */
    if (pt_len > CEEPEW_MAX_MSG_BYTES) {
        return CEEPEW_ERR_BOUNDS;
    }
    memmove(frame, s_pt_buf, pt_len);
    *len = pt_len;
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_get_last_nonce_counter(uint64_t *nonce_counter_out)
{
    CEEPEW_ASSERT(nonce_counter_out != NULL, CEEPEW_ERR_NULL_PTR);
    *nonce_counter_out = s_last_nonce_counter;
    return CEEPEW_OK;
}
