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
 *   7. crypto_box decrypt (silent fail)
 *   8. Ascon-128 AEAD decrypt + tag verify (silent fail — must not produce response)
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
#include "../crypto/crypto_hmac.h"
#include "../../main/session_fsm.h"
#include "ceepew_security_utils.h"
#include "ceepew_pipeline.h"
#include "ceepew_region.h"
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

/* Forward declaration for DoS cookie verification (defined below) */
static CeePewErr_t dos_verify_cookie(const uint8_t sender_mac[6],
                                     uint32_t timestamp_rounded,
                                     const uint8_t received_cookie[CEEPEW_COOKIE_BYTES]);

/* ──────────────────────────────────────────────────────────────────────────── */
/* ESL Receive Pipeline — 7 stages composed via Pipeline_t                     */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Shared context for the ESL receive pipeline stages.
 * Most fields are filled by stage 1 (DoS) and consumed by stage 7 (strip). */
typedef struct {
    const uint8_t *peer_mac;       /* from caller */
    uint32_t       queue_depth;     /* from caller */
    bool           dos_load_high;   /* set by DoS stage */
    bool           cookie_present;  /* set by DoS stage */
    uint16_t       payload_offset;  /* computed by DoS stage, used by strip */
    EslHeader_t    hdr;             /* parsed header (filled by stage 1) */
} EslPipelineCtx_t;

/* Stage 1: DoS guard — MAC-based cookie validation if queue is high.
 * Parses the header into ctx, then checks for DoS cookie. */
static CeePewErr_t stage_esl_dos(Region_t *region, const uint8_t *in,
                                  uint16_t in_len, uint8_t **out,
                                  uint16_t *out_len, void *stage_ctx)
{
    (void)region;
    EslPipelineCtx_t *ctx = (EslPipelineCtx_t *)stage_ctx;
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);

    if (in_len < CEEPEW_ESL_HEADER_BYTES + CEEPEW_ESL_CRC_BYTES) {
        *out = (uint8_t *)in;
        *out_len = in_len;
        return CEEPEW_ERR_PARAM;
    }

    memcpy(&ctx->hdr, in, sizeof(ctx->hdr));

    uint16_t frame_len = in_len;
    uint16_t payload_len = (uint16_t)(frame_len - CEEPEW_ESL_HEADER_BYTES - CEEPEW_ESL_CRC_BYTES);

    ctx->dos_load_high = (ctx->queue_depth > CEEPEW_DOS_QUEUE_THRESHOLD);
    ctx->payload_offset = CEEPEW_ESL_HEADER_BYTES;
    ctx->cookie_present = false;

    if (ctx->dos_load_high) {
        if ((ctx->hdr.flags & CEEPEW_ESL_FLAG_HAS_COOKIE) == 0U) {
            ESP_LOGD(TAG, "DoS: No cookie found, high queue depth (%lu)", ctx->queue_depth);
            *out = (uint8_t *)in;
            *out_len = in_len;
            return CEEPEW_ERR_TRANSPORT;
        }
        if (payload_len < CEEPEW_COOKIE_BYTES) {
            ESP_LOGW(TAG, "DoS: Cookie missing or truncated (payload_len=%u)", payload_len);
            *out = (uint8_t *)in;
            *out_len = in_len;
            return CEEPEW_ERR_TRANSPORT;
        }
        uint8_t rx_cookie[CEEPEW_COOKIE_BYTES];
        memcpy(rx_cookie, in + CEEPEW_ESL_HEADER_BYTES, CEEPEW_COOKIE_BYTES);
        uint32_t ts_rounded = (ctx->hdr.timestamp_s / 10U) * 10U;
        CeePewErr_t err = dos_verify_cookie(ctx->peer_mac, ts_rounded, rx_cookie);
        ceepew_secure_zero(rx_cookie, sizeof(rx_cookie));
        if (err != CEEPEW_OK) {
            ESP_LOGD(TAG, "DoS: Cookie verification failed (err=%d)", err);
            *out = (uint8_t *)in;
            *out_len = in_len;
            return err;
        }
        ctx->payload_offset = (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_COOKIE_BYTES);
        ctx->cookie_present = true;
    }

    *out = (uint8_t *)in;
    *out_len = in_len;
    return CEEPEW_OK;
}

/* Stage 2: MAC lock — constant-time peer identity verification */
static CeePewErr_t stage_esl_mac(Region_t *region, const uint8_t *in,
                                  uint16_t in_len, uint8_t **out,
                                  uint16_t *out_len, void *stage_ctx)
{
    (void)region;
    EslPipelineCtx_t *ctx = (EslPipelineCtx_t *)stage_ctx;
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);

    CEEPEW_ASSERT(s_esl_callbacks_registered, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_mac_cb != NULL, CEEPEW_ERR_PARAM);
    CeePewErr_t err = s_mac_cb(ctx->peer_mac);
    if (err != CEEPEW_OK) {
        ESP_LOGW(TAG, "MAC lock failed: peer MAC not in session (err=%d)", err);
        *out = (uint8_t *)in;
        *out_len = in_len;
        return err;
    }
    *out = (uint8_t *)in;
    *out_len = in_len;
    return CEEPEW_OK;
}

/* Stage 3: Magic + Version validation */
static CeePewErr_t stage_esl_magic(Region_t *region, const uint8_t *in,
                                    uint16_t in_len, uint8_t **out,
                                    uint16_t *out_len, void *stage_ctx)
{
    (void)region;
    (void)stage_ctx;
    EslHeader_t hdr;
    memcpy(&hdr, in, sizeof(hdr));

    if ((hdr.magic0 != CEEPEW_ESL_MAGIC0) || (hdr.magic1 != CEEPEW_ESL_MAGIC1) ||
        (hdr.version != CEEPEW_ESL_VERSION)) {
        ESP_LOGW(TAG, "Malformed frame: magic=%02x%02x version=%u (expected %02x%02x v%u)",
                 hdr.magic0, hdr.magic1, hdr.version,
                 CEEPEW_ESL_MAGIC0, CEEPEW_ESL_MAGIC1, CEEPEW_ESL_VERSION);
        *out = (uint8_t *)in;
        *out_len = in_len;
        return CEEPEW_ERR_TRANSPORT;
    }
    *out = (uint8_t *)in;
    *out_len = in_len;
    return CEEPEW_OK;
}

/* Stage 4: CRC-32 validation */
static CeePewErr_t stage_esl_crc(Region_t *region, const uint8_t *in,
                                  uint16_t in_len, uint8_t **out,
                                  uint16_t *out_len, void *stage_ctx)
{
    (void)region;
    (void)stage_ctx;
    if (in_len < CEEPEW_ESL_CRC_BYTES) {
        *out = (uint8_t *)in;
        *out_len = in_len;
        return CEEPEW_ERR_PARAM;
    }
    uint32_t rx_crc = 0U;
    memcpy(&rx_crc, in + in_len - CEEPEW_ESL_CRC_BYTES, sizeof(rx_crc));
    uint32_t calc_crc = esp_crc32_le(0U, in, (size_t)(in_len - CEEPEW_ESL_CRC_BYTES));
    if (rx_crc != calc_crc) {
        ESP_LOGD(TAG, "CRC mismatch: rx=%08lx calc=%08lx", rx_crc, calc_crc);
        *out = (uint8_t *)in;
        *out_len = in_len;
        return CEEPEW_ERR_TRANSPORT;
    }
    *out = (uint8_t *)in;
    *out_len = in_len;
    return CEEPEW_OK;
}

/* Stage 5: Timestamp validation — ±CEEPEW_TIMESTAMP_SLACK_S window */
static CeePewErr_t stage_esl_timestamp(Region_t *region, const uint8_t *in,
                                        uint16_t in_len, uint8_t **out,
                                        uint16_t *out_len, void *stage_ctx)
{
    (void)region;
    (void)stage_ctx;
    EslHeader_t hdr;
    memcpy(&hdr, in, sizeof(hdr));

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    int32_t peer_off_s = session_get_peer_uptime_offset();
    uint32_t peer_now_s = now_s + (uint32_t)peer_off_s;
    uint32_t hdr_s = hdr.timestamp_s;
    uint32_t diff = (peer_now_s > hdr_s) ? (peer_now_s - hdr_s) : (hdr_s - peer_now_s);
    if (diff > CEEPEW_TIMESTAMP_SLACK_S) {
        ESP_LOGW(TAG, "ESL discard: timestamp outside tolerance (diff=%d slack=%u)",
                 (int)diff, (unsigned)CEEPEW_TIMESTAMP_SLACK_S);
        *out = (uint8_t *)in;
        *out_len = in_len;
        return CEEPEW_ERR_TRANSPORT;
    }
    *out = (uint8_t *)in;
    *out_len = in_len;
    return CEEPEW_OK;
}

/* Stage 6: Replay window (WireGuard bitmap, silent fail) */
static CeePewErr_t stage_esl_replay(Region_t *region, const uint8_t *in,
                                     uint16_t in_len, uint8_t **out,
                                     uint16_t *out_len, void *stage_ctx)
{
    (void)region;
    (void)stage_ctx;
    EslHeader_t hdr;
    memcpy(&hdr, in, sizeof(hdr));

    bool is_replay = false;
    CeePewErr_t err = transport_replay_check(hdr.seq, hdr.timestamp_s, &is_replay);
    if (err != CEEPEW_OK) {
        ESP_LOGW(TAG, "ESL discard: replay check error (err=%d seq=%lu)",
                 (int)err, (unsigned long)hdr.seq);
        *out = (uint8_t *)in;
        *out_len = in_len;
        return err;
    }
    if (is_replay) {
        ESP_LOGW(TAG, "ESL discard: replay detected (seq=%lu)", (unsigned long)hdr.seq);
        *out = (uint8_t *)in;
        *out_len = in_len;
        return CEEPEW_ERR_TRANSPORT;
    }
    *out = (uint8_t *)in;
    *out_len = in_len;
    return CEEPEW_OK;
}

/* Stage 7: Strip ESL header (+ cookie if present).
 * Produces the clean payload by memmove-ing it to the start of the buffer. */
static CeePewErr_t stage_esl_strip(Region_t *region, const uint8_t *in,
                                    uint16_t in_len, uint8_t **out,
                                    uint16_t *out_len, void *stage_ctx)
{
    (void)region;
    EslPipelineCtx_t *ctx = (EslPipelineCtx_t *)stage_ctx;
    CEEPEW_ASSERT(ctx != NULL, CEEPEW_ERR_NULL_PTR);

    uint16_t frame_len = in_len;
    uint16_t payload_len = (uint16_t)(frame_len - CEEPEW_ESL_HEADER_BYTES - CEEPEW_ESL_CRC_BYTES);
    if (ctx->cookie_present) {
        payload_len = (uint16_t)(payload_len - CEEPEW_COOKIE_BYTES);
    }

    /* In-place move: shift payload to the start of the writable buffer */
    uint8_t *writable = (uint8_t *)in;
    memmove(writable, in + ctx->payload_offset, payload_len);

    *out = writable;
    *out_len = payload_len;
    return CEEPEW_OK;
}

/* Build-once ESL ingress pipeline handle */
static Pipeline_t s_esl_pipeline;
static bool s_esl_pipeline_built = false;

/* Per-call pipeline context shared across stages that need it.
 * Filled by esl_rx() before pipeline_run(), consumed by
 * stage_esl_dos, stage_esl_mac, and stage_esl_strip. */
static EslPipelineCtx_t s_esl_pipeline_ctx;

static CeePewErr_t esl_build_pipeline(void)
{
    CeePewErr_t err;

    err = pipeline_reset(&s_esl_pipeline);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_esl_pipeline, stage_esl_dos, &s_esl_pipeline_ctx);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_esl_pipeline, stage_esl_mac, &s_esl_pipeline_ctx);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_esl_pipeline, stage_esl_magic, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_esl_pipeline, stage_esl_crc, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_esl_pipeline, stage_esl_timestamp, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_esl_pipeline, stage_esl_replay, NULL);
    if (err != CEEPEW_OK) { return err; }

    err = pipeline_add_stage(&s_esl_pipeline, stage_esl_strip, &s_esl_pipeline_ctx);
    if (err != CEEPEW_OK) { return err; }

    s_esl_pipeline_built = true;
    return CEEPEW_OK;
}

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
    /* Wipe the DoS server secret so it does not persist across sessions.
     * server_secret is an HMAC key for anti-DoS cookies — not a traffic
     * encryption key, but the project policy requires zeroing all key
     * material on session end. */
    ceepew_secure_zero(s_dos_ctx.server_secret, sizeof(s_dos_ctx.server_secret));
    s_dos_ctx.last_rotate_time = 0U;
    s_dos_ctx_initialized = false;

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
    err = crypto_hmac_sha256(s_dos_ctx.server_secret, (uint16_t)sizeof(s_dos_ctx.server_secret),
                             hmac_input, (uint32_t)sizeof(hmac_input), full_hmac);
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

    /* Reject stale cookies outside the rotation window — prevents cookie replay
     * beyond the server secret lifetime. A cookie issued within the last
     * CEEPEW_COOKIE_ROTATE_S seconds is considered fresh. */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000LL);
    uint32_t cookie_age = (now > timestamp_rounded) ? (now - timestamp_rounded) : 0U;
    if (cookie_age > CEEPEW_COOKIE_ROTATE_S) {
        return CEEPEW_ERR_TRANSPORT;  /* Silent fail — stale cookie */
    }

    uint8_t expected_cookie[CEEPEW_COOKIE_BYTES];
    CeePewErr_t err = dos_generate_cookie(sender_mac, timestamp_rounded, expected_cookie);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(expected_cookie, CEEPEW_COOKIE_BYTES);
        return err;
    }
    bool match = ceepew_ct_equal(expected_cookie, received_cookie, CEEPEW_COOKIE_BYTES);
    ceepew_secure_zero(expected_cookie, CEEPEW_COOKIE_BYTES);
    if (!match) {
        return CEEPEW_ERR_TRANSPORT;  /* Silent fail */
    }
    return CEEPEW_OK;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Outgoing (TX) Wrapper                                                       */
/* ──────────────────────────────────────────────────────────────────────────── */

CeePewErr_t transport_esl_process_outgoing(uint8_t *frame, uint16_t *len, uint16_t max_len, uint64_t nonce_counter) {
    CEEPEW_ASSERT(frame != NULL && len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT((uint32_t)(*len) + (uint32_t)CEEPEW_ESL_HEADER_BYTES + (uint32_t)CEEPEW_ESL_CRC_BYTES <= CEEPEW_PACKET_MAX_BYTES, CEEPEW_ERR_BOUNDS);
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

CeePewErr_t transport_esl_process_incoming(uint8_t *frame, uint16_t *len,
                                            const uint8_t peer_mac[6],
                                            uint32_t queue_depth) {
    CEEPEW_ASSERT(frame != NULL && len != NULL && peer_mac != NULL, CEEPEW_ERR_NULL_PTR);
    if (*len < (uint16_t)(CEEPEW_ESL_HEADER_BYTES + CEEPEW_ESL_CRC_BYTES)) {
        return CEEPEW_ERR_PARAM;
    }

    if (!s_dos_ctx_initialized) { dos_init(); }

    /* Extract nonce_counter from header before pipeline stages process it.
     * The stage_esl_strip stage moves payload data which would overwrite the
     * header bytes, so we snapshot this here while the header is intact. */
    {
        EslHeader_t tmp;
        memcpy(&tmp, frame, sizeof(tmp));
        s_last_nonce_counter = tmp.nonce_counter;
    }

    /* Build ESL pipeline on first use */
    if (!s_esl_pipeline_built) {
        CeePewErr_t err = esl_build_pipeline();
        if (err != CEEPEW_OK) { return err; }
    }

    /* Set up per-call pipeline context (shared via static so stages
     * registered via pipeline_add_stage can see it). Reset all fields
     * before each run — pipeline stages run sequentially on one core,
     * so there is no concurrency concern. */
    ceepew_secure_zero(&s_esl_pipeline_ctx, sizeof(s_esl_pipeline_ctx));
    s_esl_pipeline_ctx.peer_mac = peer_mac;
    s_esl_pipeline_ctx.queue_depth = queue_depth;
    s_esl_pipeline_ctx.payload_offset = CEEPEW_ESL_HEADER_BYTES;

    /* Run the 7-stage ESL ingress pipeline (DoS → MAC → Magic → CRC →
     * Timestamp → Replay → Strip). All stages pass the frame buffer through
     * until the final strip stage which memmove-s the payload to the front. */
    uint8_t *output = NULL;
    uint16_t output_len = 0U;
    CeePewErr_t err = pipeline_run(&s_esl_pipeline, &g_region,
                                    frame, *len, &output, &output_len);
    if (err != CEEPEW_OK) { return err; }

    *len = output_len;
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

CeePewErr_t transport_esl_process_pfs_handshake(const uint8_t *payload, uint16_t len,
                                                 const uint8_t peer_mac[6],
                                                 uint8_t peer_pfs_pubkey_out[32])
{
    CEEPEW_ASSERT(payload != NULL && peer_pfs_pubkey_out != NULL, CEEPEW_ERR_NULL_PTR);
    if (len < 1U + 32U) {
        return CEEPEW_ERR_BOUNDS;
    }

    /* Extract message type */
    uint8_t msg_type = payload[0] & CEEPEW_ESL_MSG_TYPE_MASK;
    if (msg_type != CEEPEW_ESL_MSG_TYPE_PFS_INIT && msg_type != CEEPEW_ESL_MSG_TYPE_PFS_RESP) {
        return CEEPEW_ERR_PARAM;
    }

    /* Extract 32-byte PFS public key */
    memcpy(peer_pfs_pubkey_out, payload + 1U, 32U);

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
