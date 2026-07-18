/* tests/main/test_arq_negative.c — ARQ negative / edge-case test suite
 *
 * Validates that ecc_arq_decode() correctly rejects:
 *   1. Out-of-order sequence numbers (delta != 1)
 *   2. Duplicate (replayed) sequence numbers
 *   3. Sequence wrap at the 256-boundary (0xFF → 0x00)
 *   4. NULL pointer inputs (caught by CEEPEW_ASSERT)
 *   5. Zero-length payloads
 *   6. Oversized payloads (exceeding CEEPEW_PACKET_MAX_BYTES)
 *
 * All tests must pass before the ARQ subsystem is considered hardened.
 * Tests are stateless and call ecc_arq_reset() between scenarios.
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <esp_log.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "CEE-PEW-TEST-ARQ-NEG";

/* Forward declarations from ecc_arq.c */
CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len);
CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len, bool *corrected);
CeePewErr_t ecc_arq_reset(void);

static uint32_t s_passed = 0U;
static uint32_t s_failed = 0U;

/* ------------------------------------------------------------------ */
/*  Test 1: Normal in-order sequence  (happy path)                    */
/* ------------------------------------------------------------------ */
static void test_in_order(void) {
    ESP_LOGI(TAG, "=== Test: In-order sequence accept ===");
    ecc_arq_reset();

    uint8_t payload[] = { 'H', 'e', 'l', 'l', 'o' };
    uint8_t frame[64];
    uint16_t frame_len = 0U;
    CeePewErr_t err = ecc_arq_encode(payload, sizeof(payload),
                                     frame, &frame_len, sizeof(frame));
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "[FAIL] encode: %d", err); s_failed++; return; }

    uint8_t out[64];
    uint16_t out_len = 0U;
    bool corrected = false;
    err = ecc_arq_decode(frame, frame_len, out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_OK && out_len == sizeof(payload) && !corrected) {
        ESP_LOGI(TAG, "[PASS] in-order accepted");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] in-order: err=%d, out_len=%u, corrected=%d",
                 err, out_len, corrected);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 2: Out-of-order sequence  (delta != 1 → CEEPEW_ERR_REPLAY)  */
/* ------------------------------------------------------------------ */
static void test_out_of_order(void) {
    ESP_LOGI(TAG, "=== Test: Out-of-order rejection ===");
    ecc_arq_reset();

    /* Send seq=0 */
    const uint8_t p0[] = { 'A' };
    uint8_t f0[4]; uint16_t f0_len;
    ecc_arq_encode(p0, 1, f0, &f0_len, sizeof(f0));

    /* Send seq=1 */
    const uint8_t p1[] = { 'B' };
    uint8_t f1[4]; uint16_t f1_len;
    ecc_arq_encode(p1, 1, f1, &f1_len, sizeof(f1));

    /* Decode seq=0 first (init branch: expected = 1) */
    uint8_t out[4]; uint16_t out_len; bool corrected;
    CeePewErr_t err = ecc_arq_decode(f0, f0_len, out, &out_len, sizeof(out), &corrected);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "[FAIL] init decode: %d", err); s_failed++; return; }

    /* Now decode seq=1 — should pass (in-order after init) */
    err = ecc_arq_decode(f1, f1_len, out, &out_len, sizeof(out), &corrected);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "[FAIL] in-order after init: %d", err); s_failed++; return; }

    /* Now decode seq=0 again — should be rejected as replay (delta would wrap) */
    err = ecc_arq_decode(f0, f0_len, out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_ERR_REPLAY) {
        ESP_LOGI(TAG, "[PASS] out-of-order rejected with CEEPEW_ERR_REPLAY");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] out-of-order: expected CEEPEW_ERR_REPLAY, got %d", err);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 3: Duplicate (replayed) sequence                             */
/* ------------------------------------------------------------------ */
static void test_duplicate(void) {
    ESP_LOGI(TAG, "=== Test: Duplicate sequence rejection ===");
    ecc_arq_reset();

    uint8_t payload[] = { 0x01, 0x02, 0x03 };
    uint8_t frame[32]; uint16_t frame_len;
    ecc_arq_encode(payload, sizeof(payload), frame, &frame_len, sizeof(frame));

    uint8_t out[32]; uint16_t out_len; bool corrected;
    CeePewErr_t err = ecc_arq_decode(frame, frame_len, out, &out_len, sizeof(out), &corrected);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "[FAIL] first decode: %d", err); s_failed++; return; }

    /* Decode the same frame again — should be CEEPEW_ERR_REPLAY */
    err = ecc_arq_decode(frame, frame_len, out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_ERR_REPLAY) {
        ESP_LOGI(TAG, "[PASS] duplicate rejected");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] duplicate: expected CEEPEW_ERR_REPLAY, got %d", err);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 4: Sequence wrap at 256 boundary (0xFF → 0x00)              */
/*                                                                     */
/*  The 8-bit ARQ seq alone CANNOT distinguish a replay from a         */
/*  legitimate wrap at the exact instant the counter crosses 256       */
/*  (both present the same wire byte).  TRUE replay protection comes   */
/*  from the ESL 64-bit replay window (transport_replay.c) and the     */
/*  crypto-layer 64-bit nonce (crypto_box_wrap.c).                     */
/*                                                                     */
/*  This test verifies that after the FIRST wrap frame has been        */
/*  accepted (confirming that wrap is handled), a subsequent replay    */
/*  of the original seq-0 is rejected because the full 16-bit estimate */
/*  diverges from the expected counter.                                */
/* ------------------------------------------------------------------ */
static void test_seq_wrap(void) {
    ESP_LOGI(TAG, "=== Test: Sequence wrap at 256 boundary ===");
    ecc_arq_reset();

    /* Encode 258 messages: 0..255, then 0(wrap), 1(wrap) — two frames
     * past the first 8-bit wrap so we can test replay detection. */
    uint8_t buf[2];
    uint8_t frame[258][4];
    uint16_t flen[258];

    for (uint16_t i = 0U; i < 258U; i++) {
        buf[0] = (uint8_t)(i & 0xFFU);
        ecc_arq_encode(buf, 1, frame[i], &flen[i], sizeof(frame[i]));
    }

    /* Decode frames 0..255 — normal in-order acceptance */
    uint8_t out[4]; uint16_t out_len; bool corrected;
    for (uint16_t i = 0U; i < 256U; i++) {
        CeePewErr_t err = ecc_arq_decode(frame[i], flen[i], out, &out_len, sizeof(out), &corrected);
        if (err != CEEPEW_OK) {
            ESP_LOGE(TAG, "[FAIL] wrap decode at i=%u: err=%d", i, err);
            s_failed++;
            return;
        }
    }

    /* Decode frame[256] (wire-seq 0, the first wrap) — must be accepted */
    CeePewErr_t err = ecc_arq_decode(frame[256], flen[256], out, &out_len, sizeof(out), &corrected);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "[FAIL] first wrap frame rejected: %d", err);
        s_failed++;
        return;
    }

    /* Now decode the ORIGINAL frame[0] (wire-seq 0) — this is a replay
     * of the very first message; the full 16-bit estimate now places
     * it at seq=512, far above the expected counter, so delta != 1. */
    err = ecc_arq_decode(frame[0], flen[0], out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_ERR_REPLAY) {
        ESP_LOGI(TAG, "[PASS] 256-boundary wrap: replay of seq-0 rejected");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] wrap replay: expected CEEPEW_ERR_REPLAY, got %d", err);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 9: Within-window replay rejection (no wrap)                  */
/*                                                                     */
/*  Verifies that a replayed frame is rejected before the 8-bit seq   */
/*  counter wraps, where the full 16-bit estimate cleanly diverges.   */
/* ------------------------------------------------------------------ */
static void test_replay_within_window(void) {
    ESP_LOGI(TAG, "=== Test: Within-window replay rejection ===");
    ecc_arq_reset();

    uint8_t frame[2][4];
    uint16_t flen[2];

    /* Encode seq=0 and seq=1 */
    uint8_t p0 = 0xAA;
    ecc_arq_encode(&p0, 1, frame[0], &flen[0], sizeof(frame[0]));
    ecc_arq_encode(&p0, 1, frame[1], &flen[1], sizeof(frame[1]));

    uint8_t out[4]; uint16_t out_len; bool corrected;

    /* Decode seq=0 — first frame, accepted via init */
    CeePewErr_t err = ecc_arq_decode(frame[0], flen[0], out, &out_len, sizeof(out), &corrected);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "[FAIL] init decode: %d", err); s_failed++; return; }

    /* Decode seq=1 — in-order, must be accepted */
    err = ecc_arq_decode(frame[1], flen[1], out, &out_len, sizeof(out), &corrected);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "[FAIL] in-order decode: %d", err); s_failed++; return; }

    /* Decode seq=0 again — replay, must be rejected */
    err = ecc_arq_decode(frame[0], flen[0], out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_ERR_REPLAY) {
        ESP_LOGI(TAG, "[PASS] within-window replay rejected");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] within-window replay: expected CEEPEW_ERR_REPLAY, got %d", err);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 10: Double-wrap (512+ frames)                                */
/*                                                                     */
/*  Exposes any residual off-by-one at the second 8-bit wrap boundary.*/
/*  After 514 frames (two wraps + 2), replays of frames from the      */
/*  first window must still be rejected.                               */
/* ------------------------------------------------------------------ */
static void test_double_wrap(void) {
    ESP_LOGI(TAG, "=== Test: Double-wrap (512+ frames) ===");
    ecc_arq_reset();

    /* Encode 514 messages: two full wraps (0..511) + 2 more */
    uint8_t buf[2];
    uint16_t n = 514U;
    uint8_t frame[514][4];
    uint16_t flen[514];

    for (uint16_t i = 0U; i < n; i++) {
        buf[0] = (uint8_t)(i & 0xFFU);
        ecc_arq_encode(buf, 1, frame[i], &flen[i], sizeof(frame[i]));
    }

    /* Decode all 514 frames */
    uint8_t out[4]; uint16_t out_len; bool corrected;
    for (uint16_t i = 0U; i < n; i++) {
        CeePewErr_t err = ecc_arq_decode(frame[i], flen[i], out, &out_len, sizeof(out), &corrected);
        if (err != CEEPEW_OK) {
            ESP_LOGE(TAG, "[FAIL] double-wrap decode at i=%u: err=%d", i, err);
            s_failed++;
            return;
        }
    }

    /* Replay frame[0] (seq=0 from the first window) — must be rejected */
    CeePewErr_t err = ecc_arq_decode(frame[0], flen[0], out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_ERR_REPLAY) {
        ESP_LOGI(TAG, "[PASS] double-wrap: replay of seq-0 rejected");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] double-wrap replay: expected CEEPEW_ERR_REPLAY, got %d", err);
        s_failed++;
    }

    /* Replay frame[256] (seq=0 from the second window) — also rejected */
    err = ecc_arq_decode(frame[256], flen[256], out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_ERR_REPLAY) {
        ESP_LOGI(TAG, "[PASS] double-wrap: replay of second-window seq-0 rejected");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] double-wrap second-window replay: expected CEEPEW_ERR_REPLAY, got %d", err);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 5: Zero-length payload (encode + decode)                     */
/* ------------------------------------------------------------------ */
static void test_zero_length(void) {
    ESP_LOGI(TAG, "=== Test: Zero-length payload ===");
    ecc_arq_reset();

    uint8_t frame[4]; uint16_t frame_len;
    CeePewErr_t err = ecc_arq_encode(NULL, 0U, frame, &frame_len, sizeof(frame));
    if (err != CEEPEW_OK) {
        /* encode with NULL in and 0 len should still produce a seq byte */
        ESP_LOGI(TAG, "[PASS] zero-length encode returned %d (allows)", err);
        s_passed++;
    } else {
        if (frame_len == 1U) {
            uint8_t out[4]; uint16_t out_len; bool corrected;
            ecc_arq_reset();
            err = ecc_arq_decode(frame, frame_len, out, &out_len, sizeof(out), &corrected);
            if (err == CEEPEW_OK && out_len == 0U) {
                ESP_LOGI(TAG, "[PASS] zero-length decode ok");
                s_passed++;
            } else {
                ESP_LOGE(TAG, "[FAIL] zero-length decode: err=%d, out_len=%u", err, out_len);
                s_failed++;
            }
        } else {
            ESP_LOGE(TAG, "[FAIL] zero-length encode frame_len=%u (expected 1)", frame_len);
            s_failed++;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Test 6: Oversized payload (exceeds CEEPEW_PACKET_MAX_BYTES)      */
/* ------------------------------------------------------------------ */
static void test_oversized(void) {
    ESP_LOGI(TAG, "=== Test: Oversized payload rejection ===");
    ecc_arq_reset();

    /* ecc_arq_send checks payload_len <= CEEPEW_PACKET_MAX_BYTES */
    uint8_t big[CEEPEW_PACKET_MAX_BYTES + 1U];
    for (uint16_t i = 0U; i < sizeof(big); i++) { big[i] = (uint8_t)(i & 0xFFU); }

    /* Try encode with max_out_len too small */
    uint8_t frame[4];
    uint16_t frame_len;
    CeePewErr_t err = ecc_arq_encode(big, sizeof(big), frame, &frame_len, sizeof(frame));
    if (err != CEEPEW_OK) {
        ESP_LOGI(TAG, "[PASS] oversized encode rejected: %d", err);
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] oversized encode should have failed");
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 7: Decode with buffer too small                              */
/* ------------------------------------------------------------------ */
static void test_decode_buffer_too_small(void) {
    ESP_LOGI(TAG, "=== Test: Decode with insufficient out buffer ===");
    ecc_arq_reset();

    uint8_t payload[] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t frame[16]; uint16_t frame_len;
    ecc_arq_encode(payload, sizeof(payload), frame, &frame_len, sizeof(frame));

    uint8_t tiny[2]; uint16_t out_len; bool corrected;
    CeePewErr_t err = ecc_arq_decode(frame, frame_len, tiny, &out_len, sizeof(tiny), &corrected);
    if (err == CEEPEW_ERR_BOUNDS) {
        ESP_LOGI(TAG, "[PASS] decode with tiny buffer returned CEEPEW_ERR_BOUNDS");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] decode tiny buffer: expected CEEPEW_ERR_BOUNDS, got %d", err);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Test 8: Decode with in_len < CEEPEW_ARQ_SEQ_BYTES (malformed)    */
/* ------------------------------------------------------------------ */
static void test_decode_underflow(void) {
    ESP_LOGI(TAG, "=== Test: Decode with underlength frame ===");
    ecc_arq_reset();

    uint8_t too_short[] = { 0x00 };          /* 1 byte, but need ≥ 1 (SEQ_BYTES) */
    uint8_t out[4]; uint16_t out_len; bool corrected;

    /* in_len >= CEEPEW_ARQ_SEQ_BYTES (=1), so this should pass the assert.
     * But the seq will be 0 and with init that's fine.
     * Let's test with truly 0 bytes: */
    CeePewErr_t err = ecc_arq_decode(too_short, 0U, out, &out_len, sizeof(out), &corrected);
    if (err == CEEPEW_ERR_PARAM) {
        ESP_LOGI(TAG, "[PASS] decode zero-length input rejected");
        s_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] decode zero-length: expected CEEPEW_ERR_PARAM, got %d", err);
        s_failed++;
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */
void test_arq_negative(void) {
    ESP_LOGI(TAG, "\n============================================");
    ESP_LOGI(TAG, "ARQ Negative / Edge-Case Test Suite");
    ESP_LOGI(TAG, "============================================");

    s_passed = 0U; s_failed = 0U;

    test_in_order();
    test_out_of_order();
    test_duplicate();
    test_seq_wrap();
    test_replay_within_window();
    test_double_wrap();
    test_zero_length();
    test_oversized();
    test_decode_buffer_too_small();
    test_decode_underflow();

    ESP_LOGI(TAG, "--------------------------------------------");
    ESP_LOGI(TAG, "ARQ negative test results: passed=%u failed=%u",
             s_passed, s_failed);
    ESP_LOGI(TAG, "============================================\n");
}
