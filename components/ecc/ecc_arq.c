/* components/ecc/ecc_arq.c */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "../ceepew_hal/hal_radio.h"

#define CEEPEW_ARQ_SEQ_BYTES 1U

/* Exponential backoff base timing (in milliseconds) for ARQ retries:
 * Attempt 0: 100ms base + ±10% jitter
 * Attempt 1: 200ms base + ±10% jitter
 * Attempt 2: 400ms base + ±10% jitter
 */
#define CEEPEW_ARQ_BACKOFF_BASE_MS 100U
#define CEEPEW_ARQ_JITTER_PERCENT 10U

/* Hop synchronization state */
static volatile bool s_arq_hop_paused = false;
static uint32_t s_arq_pause_start_ms = 0U;

/* Calculate exponential backoff time with ±jitter_percent deviation.
 *
 * For attempt N:
 *   base_ms = (CEEPEW_ARQ_BACKOFF_BASE_MS << N)
 *   jitter_offset = base_ms * (random_signed_percent / 100)
 *   actual_wait = base_ms + jitter_offset
 *
 * This spreads retransmissions over exponentially increasing windows,
 * reducing collision probability when multiple devices retry simultaneously.
 */
static uint16_t ecc_arq_compute_backoff_ms(uint8_t attempt) {
    CEEPEW_ASSERT(attempt < CEEPEW_ARQ_MAX_RETRIES, 0U);

    /* Calculate base backoff: 100ms << attempt => 100, 200, 400 ms */
    uint16_t base_ms = (uint16_t)(CEEPEW_ARQ_BACKOFF_BASE_MS << attempt);

    /* Generate random jitter in range [-10%, +10%]
     * esp_random() returns 0..0xFFFFFFFF uniformly
     * We map this to a signed percentage offset: -10 to +10
     */
    uint32_t random_val = esp_random();
    int16_t jitter_percent = (int16_t)((random_val % (2U * CEEPEW_ARQ_JITTER_PERCENT + 1U))
                                       - CEEPEW_ARQ_JITTER_PERCENT);

    /* Apply jitter: base_ms + (base_ms * jitter_percent / 100)
     * Use int32_t intermediate to avoid overflow before division.
     */
    int32_t offset_ms = (int32_t)base_ms * jitter_percent / 100;
    uint16_t actual_wait_ms = (uint16_t)((int32_t)base_ms + offset_ms);

    /* Ensure result is within bounds */
    CEEPEW_ASSERT(actual_wait_ms > 0U && actual_wait_ms <= (base_ms + (base_ms / 10U)), 0U);

    return actual_wait_ms;
}

/* Hop synchronization callbacks — called from hal_radio hop task.
 * pre_hop: Pause ARQ retransmit timer ~10ms before channel change.
 * post_hop: Resume ARQ retransmit timer ~10ms after channel change.
 * These must be fast and non-blocking (hop task context). */
static void ecc_arq_pre_hop_cb(void)
{
    s_arq_hop_paused = true;
    s_arq_pause_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void ecc_arq_post_hop_cb(void)
{
    s_arq_hop_paused = false;
    s_arq_pause_start_ms = 0U;
}

/* Initialize ARQ hop synchronization — register callbacks with hal_radio.
 * Call once after hal_radio_init() and before starting channel hopping. */
CeePewErr_t ecc_arq_init_hop_sync(void)
{
    return hal_radio_set_hop_sync_callbacks(ecc_arq_pre_hop_cb, ecc_arq_post_hop_cb);
}

static uint16_t s_tx_seq = 0U;
static uint16_t s_expected_rx_seq = 0U;  /* full 16-bit counter tracking accepted frames */
static bool s_rx_init = false;

/* Reset ARQ engine state — securely zeroes sequence counters for clean
 * slate chat. Called from session_clear_sync_barrier_internal() at the
 * instant the post-derive sync barrier is verified. This ensures no stale
 * sequence numbers from the pairing exchange contaminate the active session.
 *
 * SECURITY: Uses ceepew_secure_zero() rather than direct assignment so that
 * sequence-state residues are not left in RAM after session teardown,
 * preventing stale-state side-channel leakage across session boundaries.
 *
 * DEFENSE-IN-DEPTH: The ARQ 8-bit wire seq is an ordering/duplicate-rejection
 * mechanism only.  Its 256-frame window is too small to provide cryptographic
 * replay protection across wrap boundaries — after exhausting the 8-bit space
 * an attacker can replay a frame whose wire-seq collides with the next
 * legitimate wrap value.  TRUE replay protection is provided by:
 *   1. ESL replay window (transport_replay.c) — 64-bit WireGuard-style bitmap
 *      sliding over msg_id.
 *   2. Crypto nonce (crypto_box_wrap.c) — 64-bit counter with parity invariant
 *      and 2^56 hard limit; no nonce can ever repeat.
 * Either layer independently prevents the ARQ-layer wrap ambiguity from
 * becoming an authentication bypass. */
CeePewErr_t ecc_arq_reset(void)
{
    ceepew_secure_zero(&s_tx_seq, sizeof(s_tx_seq));
    ceepew_secure_zero(&s_expected_rx_seq, sizeof(s_expected_rx_seq));
    ceepew_secure_zero(&s_rx_init, sizeof(s_rx_init));
    return CEEPEW_OK;
}

/* Forward-declare transport functions provided elsewhere */
CeePewErr_t transport_espnow_send(const uint8_t *peer_mac, const uint8_t *data, uint16_t len);
CeePewErr_t transport_wait_ack(const uint8_t *peer_mac, uint16_t seq, uint32_t timeout_ms);
CeePewErr_t transport_espnow_rendezvous_drive(void);

CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, uint16_t max_out_len){
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT((uint32_t)in_len + CEEPEW_ARQ_SEQ_BYTES <= max_out_len, CEEPEW_ERR_BOUNDS);

    out[0] = (uint8_t)(s_tx_seq++ & 0xFFU);
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

    if (!s_rx_init) {
        /* First frame since reset: accept unconditionally.
         * Advance the 16-bit counter so that after N frames expected=N,
         * fixing the off-by-one where the init path omitted the increment
         * that every other accepted frame performs. */
        s_rx_init = true;
        s_expected_rx_seq = (uint16_t)(seq + 1U);
    } else {
        /* Reconstruct the full 16-bit seq from the 8-bit wire byte:
         * use the upper 8 bits of the expected counter and fall back
         * through a +0x100 step if the estimate undershoots.  This
         * correctly disambiguates wraps at the 256-boundary from replays
         * of frames in the preceding window, unlike the previous formula
         * which compared only modulo-256 and conflated wrap with replay. */
        uint16_t seq_estimate = (s_expected_rx_seq & 0xFF00U) | seq;
        if (seq_estimate < s_expected_rx_seq) {
            seq_estimate += 0x100U;
        }
        uint16_t last_accepted = s_expected_rx_seq - 1U;
        uint16_t delta = seq_estimate - last_accepted;
        if (delta != 1U) {
            *corrected = false;
            return CEEPEW_ERR_REPLAY;
        }
        s_expected_rx_seq = seq_estimate + 1U;
    }

    uint16_t payload_len = (uint16_t)(in_len - CEEPEW_ARQ_SEQ_BYTES);
    for (uint16_t i = 0U; i < payload_len; i++){ out[i] = in[1U + i]; }
    *out_len = payload_len;
    *corrected = false;
    return CEEPEW_OK;
}

/* Stop-and-Wait ARQ sender using transport_espnow_send() and transport_wait_ack()
 *
 * Implements exponential backoff with jitter on timeout:
 * - Attempt 0: 100ms base ± 10% jitter
 * - Attempt 1: 200ms base ± 10% jitter
 * - Attempt 2: 400ms base ± 10% jitter
 */
CeePewErr_t ecc_arq_send(const uint8_t peer_mac[6], const uint8_t *payload, uint16_t payload_len){
    CEEPEW_ASSERT(peer_mac != NULL && payload != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(payload_len > 0U && payload_len <= CEEPEW_PACKET_MAX_BYTES, CEEPEW_ERR_PARAM);

    uint8_t frame[CEEPEW_PACKET_MAX_BYTES];
    uint16_t frame_len = 0U;

    /* Prepare frame with sequence byte */
    CeePewErr_t err = ecc_arq_encode(payload, payload_len, frame, &frame_len, sizeof(frame));
    if (err != CEEPEW_OK) { return err; }

    uint16_t seq = s_tx_seq - 1;  /* Last sequence used by encode */

    for (uint8_t attempt = 0U; attempt < CEEPEW_ARQ_MAX_RETRIES; attempt++){
        /* Wait if a channel hop is in progress (paused by pre_hop_cb).
         * This prevents transmitting during the ~20ms window when the radio
         * is switching channels, which would cause unnecessary retries. */
        while (s_arq_hop_paused) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        /* Consume stale send-completion signal (if any) so the
         * subsequent transport_wait_ack waits on THIS send, not a
         * leftover from a previous attempt. */
        (void)transport_wait_ack(peer_mac, seq, 0U);
        err = hal_radio_send(frame, frame_len);
        if (err != CEEPEW_OK){ return err; }

        /* Wait for ACK (stubbed in platforms without ACK path) */
        CeePewErr_t ack_err = transport_wait_ack(peer_mac, seq, CEEPEW_ARQ_TIMEOUT_MS);
        if (ack_err == CEEPEW_OK){
            return CEEPEW_OK; /* Success */
        }
        else if (ack_err == CEEPEW_ERR_TIMEOUT || ack_err == CEEPEW_ERR_HW){
            /* Timeout: apply exponential backoff with jitter before retry.
             * Skip delay on final attempt (all retries exhausted after this loop).
             * Also respect hop pauses during backoff. */
            if (attempt < CEEPEW_ARQ_MAX_RETRIES - 1U) {
                uint16_t backoff_ms = ecc_arq_compute_backoff_ms(attempt);
                uint32_t backoff_start = (uint32_t)(esp_timer_get_time() / 1000ULL);
                while ((uint32_t)(esp_timer_get_time() / 1000ULL) - backoff_start < backoff_ms) {
                    if (s_arq_hop_paused) {
                        /* Extend backoff by pause duration */
                        backoff_start += 2;  /* Small adjustment */
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            continue;
        }
        else {
            return ack_err;
        }
    }
    return CEEPEW_ERR_MAX_RETRIES;
}
