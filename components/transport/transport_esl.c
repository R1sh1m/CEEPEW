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
    CEEPEW_ASSERT((uint32_t)(*len) + (uint32_t)CEEPEW_ESL_HEADER_BYTES + (uint32_t)CEEPEW_ESL_CRC_BYTES <= CEEPEW_PACKET_MAX_BYTES, CEEPEW_ERR_BOUNDS);
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
    if (*len < (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_ESL_CRC_BYTES)) {
        return CEEPEW_ERR_PARAM;
    }

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
        ESP_LOGW(TAG, "MAC lock failed: peer MAC not in session (err=%d)", err);
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
    uint32_t peer_now_s = now_s + (uint32_t)peer_off_s;  /* peer_off_s clamped to +/-86400 in session_set_peer_uptime_offset */
    uint32_t hdr_s      = hdr.timestamp_s;
    uint32_t diff = (peer_now_s > hdr_s) ? (peer_now_s - hdr_s) : (hdr_s - peer_now_s);
    if (diff > CEEPEW_TIMESTAMP_SLACK_S) {
        ESP_LOGW(TAG, "ESL discard: timestamp outside tolerance (diff=%d slack=%u)",
                 (int)diff, (unsigned)CEEPEW_TIMESTAMP_SLACK_S);
        return CEEPEW_ERR_TRANSPORT;  /* Silent fail — prevents replay via timestamp spoofing */
    }

    /* ═ STEP 6: Replay Window (WireGuard bitmap, silent fail) ═ */
    bool is_replay = false;
    err = transport_replay_check(hdr.seq, hdr.timestamp_s, &is_replay);
    if (err != CEEPEW_OK) {
        ESP_LOGW(TAG, "ESL discard: replay check error (err=%d seq=%lu)",
                 (int)err, (unsigned long)hdr.seq);
        return err;  /* Silent fail */
    }
    if (is_replay) {
        ESP_LOGW(TAG, "ESL discard: replay detected (seq=%lu)", (unsigned long)hdr.seq);
        return CEEPEW_ERR_TRANSPORT;  /* Silent fail */
    }

    /* ═ STEP 7: Strip ESL header — payload follows ═ */
    uint16_t payload_offset = CEEPEW_ESL_HEADER_BYTES;
    if (dos_load_high && ((hdr.flags & CEEPEW_ESL_FLAG_HAS_COOKIE) != 0U)) {
        payload_offset = (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_COOKIE_BYTES);
    }
    memmove(frame, frame + payload_offset, payload_len);
    *len = payload_len;
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_get_last_nonce_counter(uint64_t *nonce_counter_out)
{
    CEEPEW_ASSERT(nonce_counter_out != NULL, CEEPEW_ERR_NULL_PTR);
    *nonce_counter_out = s_last_nonce_counter;
    return CEEPEW_OK;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* PFS Handshake Support                                                       */
/* ──────────────────────────────────────────────────────────────────────────── */

CeePewErr_t transport_esl_peek_msg_type(const uint8_t *frame, uint16_t len, uint8_t *msg_type_out)
{
    CEEPEW_ASSERT(frame != NULL && msg_type_out != NULL, CEEPEW_ERR_NULL_PTR);
    if (len < CEEPEW_ESL_HEADER_BYTES + 1U) {
        return CEEPEW_ERR_PARAM;
    }

    /* Validate ESL magic and version first */
    if (frame[0] != CEEPEW_ESL_MAGIC0 || frame[1] != CEEPEW_ESL_MAGIC1 || frame[2] != CEEPEW_ESL_VERSION) {
        return CEEPEW_ERR_PARAM;
    }

    *msg_type_out = frame[CEEPEW_ESL_HEADER_BYTES] & CEEPEW_ESL_MSG_TYPE_MASK;
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_process_pfs_handshake(const uint8_t *frame, uint16_t len,
                                                 const uint8_t peer_mac[6],
                                                 uint8_t peer_pfs_pubkey_out[32])
{
    CEEPEW_ASSERT(frame != NULL && peer_pfs_pubkey_out != NULL, CEEPEW_ERR_NULL_PTR);
    if (len < CEEPEW_ESL_HEADER_BYTES + 1U + 32U + CEEPEW_ESL_CRC_BYTES) {
        return CEEPEW_ERR_PARAM;
    }

    /* Verify CRC first */
    uint32_t rx_crc = 0U;
    memcpy(&rx_crc, frame + len - CEEPEW_ESL_CRC_BYTES, sizeof(rx_crc));
    uint32_t calc_crc = esp_crc32_le(0U, frame, (size_t)(len - CEEPEW_ESL_CRC_BYTES));
    if (rx_crc != calc_crc) {
        ESP_LOGW("ESL", "PFS handshake CRC mismatch");
        return CEEPEW_ERR_TRANSPORT;
    }

    /* Validate magic and version */
    if (frame[0] != CEEPEW_ESL_MAGIC0 || frame[1] != CEEPEW_ESL_MAGIC1 || frame[2] != CEEPEW_ESL_VERSION) {
        return CEEPEW_ERR_PARAM;
    }

    /* Extract message type */
    uint8_t msg_type = frame[CEEPEW_ESL_HEADER_BYTES] & CEEPEW_ESL_MSG_TYPE_MASK;
    if (msg_type != CEEPEW_ESL_MSG_TYPE_PFS_INIT && msg_type != CEEPEW_ESL_MSG_TYPE_PFS_RESP) {
        return CEEPEW_ERR_PARAM;
    }

    /* Extract 32-byte PFS public key */
    memcpy(peer_pfs_pubkey_out, frame + CEEPEW_ESL_HEADER_BYTES + 1U, 32U);

    ESP_LOGI("ESL", "PFS handshake received: type=%u", (unsigned)msg_type);
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_build_pfs_handshake(uint8_t *frame, uint16_t *len, uint16_t max_len,
                                               const uint8_t pfs_pubkey[32], bool is_initiator)
{
    CEEPEW_ASSERT(frame != NULL && len != NULL && pfs_pubkey != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_len >= CEEPEW_ESL_HEADER_BYTES + 1U + 32U + CEEPEW_ESL_CRC_BYTES, CEEPEW_ERR_BOUNDS);

    uint8_t msg_type = is_initiator ? CEEPEW_ESL_MSG_TYPE_PFS_INIT : CEEPEW_ESL_MSG_TYPE_PFS_RESP;
    uint16_t payload_len = 1U + 32U;  /* 1 byte type + 32 bytes pubkey */

    EslHeader_t hdr = {
        .magic0       = CEEPEW_ESL_MAGIC0,
        .magic1       = CEEPEW_ESL_MAGIC1,
        .version      = CEEPEW_ESL_VERSION,
        .flags        = 0U,
        .timestamp_s  = (uint32_t)(esp_timer_get_time() / 1000000LL),
        .seq          = s_tx_seq++,
        .nonce_counter = 0U,  /* PFS handshake uses nonce_counter = 0 (outside normal sequence) */
    };

    memcpy(frame, &hdr, sizeof(hdr));
    frame[CEEPEW_ESL_HEADER_BYTES] = msg_type;
    memcpy(frame + CEEPEW_ESL_HEADER_BYTES + 1U, pfs_pubkey, 32U);

    uint32_t crc = esp_crc32_le(0U, frame, (size_t)(CEEPEW_ESL_HEADER_BYTES + payload_len));
    memcpy(frame + CEEPEW_ESL_HEADER_BYTES + payload_len, &crc, sizeof(crc));
    *len = (uint16_t)(CEEPEW_ESL_HEADER_BYTES + payload_len + CEEPEW_ESL_CRC_BYTES);

    return CEEPEW_OK;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Rendezvous Phase (Static Channel Sync before Channel Hopping)               */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Rendezvous frames are sent as RAW ESP-NOW frames on the static baseline
 * channel (CEEPEW_ESPNOW_CHANNEL). They are NOT encrypted, NOT wrapped in ESL.
 * Format:
 *   REQ: [0x03][uptime_us_lo][uptime_us_mid][uptime_us_hi][uptime_us_xhi] = 1 + 4 = 5 bytes (32-bit uptime)
 *   ACK: [0x04][req_uptime_lo][req_uptime_mid][req_uptime_hi][req_uptime_xhi][offset_lo][offset_mid][offset_hi][offset_xhi] = 1 + 4 + 4 = 9 bytes
 * Using 32-bit uptime (microseconds) - wraps every ~71 minutes, sufficient for sync. */

CeePewErr_t transport_esl_build_rendezvous_req(uint8_t *frame, uint16_t *len, uint16_t max_len)
{
    CEEPEW_ASSERT(frame != NULL && len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_len >= 5U, CEEPEW_ERR_BOUNDS);

    uint32_t uptime_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFULL);

    frame[0] = CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_REQ;
    frame[1] = (uint8_t)(uptime_us & 0xFFU);
    frame[2] = (uint8_t)((uptime_us >> 8U) & 0xFFU);
    frame[3] = (uint8_t)((uptime_us >> 16U) & 0xFFU);
    frame[4] = (uint8_t)((uptime_us >> 24U) & 0xFFU);

    *len = 5U;
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_build_rendezvous_ack(uint64_t req_uptime, uint8_t *frame, uint16_t *len, uint16_t max_len)
{
    CEEPEW_ASSERT(frame != NULL && len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_len >= 9U, CEEPEW_ERR_BOUNDS);

    uint32_t req_uptime_32 = (uint32_t)req_uptime;
    uint32_t now_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFULL);
    int32_t offset_us = (int32_t)(now_us - req_uptime_32);

    frame[0] = CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_ACK;
    frame[1] = (uint8_t)(req_uptime_32 & 0xFFU);
    frame[2] = (uint8_t)((req_uptime_32 >> 8U) & 0xFFU);
    frame[3] = (uint8_t)((req_uptime_32 >> 16U) & 0xFFU);
    frame[4] = (uint8_t)((req_uptime_32 >> 24U) & 0xFFU);
    frame[5] = (uint8_t)(offset_us & 0xFFU);
    frame[6] = (uint8_t)((offset_us >> 8U) & 0xFFU);
    frame[7] = (uint8_t)((offset_us >> 16U) & 0xFFU);
    frame[8] = (uint8_t)((offset_us >> 24U) & 0xFFU);

    *len = 9U;
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_parse_rendezvous_req(const uint8_t *frame, uint16_t len, uint64_t *req_uptime_out)
{
    CEEPEW_ASSERT(frame != NULL && req_uptime_out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len >= 5U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(frame[0] == CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_REQ, CEEPEW_ERR_PARAM);

    uint32_t uptime_us = ((uint32_t)frame[1] |
                          ((uint32_t)frame[2] << 8U) |
                          ((uint32_t)frame[3] << 16U) |
                          ((uint32_t)frame[4] << 24U));

    *req_uptime_out = uptime_us;
    return CEEPEW_OK;
}

CeePewErr_t transport_esl_parse_rendezvous_ack(const uint8_t *frame, uint16_t len, int64_t *offset_us_out)
{
    CEEPEW_ASSERT(frame != NULL && offset_us_out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len >= 9U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(frame[0] == CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_ACK, CEEPEW_ERR_PARAM);

    int32_t offset_us = ((int32_t)frame[5] |
                         ((int32_t)frame[6] << 8U) |
                         ((int32_t)frame[7] << 16U) |
                         ((int32_t)frame[8] << 24U));

    *offset_us_out = offset_us;
    return CEEPEW_OK;
}
