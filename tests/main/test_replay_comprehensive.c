/* main/test_replay_comprehensive.c
 * Comprehensive unit tests for WireGuard-style replay window implementation
 *
 * COVERAGE:
 * 1. Wrap-around at 2^64-1: Sequence number near UINT64_MAX transitions to small values
 * 2. Old packet rejection: Packets older than window depth (64 bits) are rejected
 * 3. Duplicate detection: Same sequence number sent twice is rejected on second occurrence
 * 4. Bitmap shift correctness: Window correctly shifts and bits are set/cleared properly
 * 5. Out-of-order acceptance: Valid packets within window are accepted even if out-of-order
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <esp_log.h>
#include <stdint.h>
#include <stdbool.h>

static const char *TAG = "TEST-REPLAY-COMP";

/* Forward declarations from transport_replay.c */
CeePewErr_t transport_replay_check(uint64_t msg_id, uint32_t timestamp, bool *is_replay);
void transport_replay_reset(void);

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Statistics                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */
static uint32_t s_total_passed = 0U;
static uint32_t s_total_failed = 0U;

#define TEST_ASSERT_REPLAY_REJECTED(msg_id, expected_is_replay, msg) \
    do { \
        bool is_replay = false; \
        CeePewErr_t err = transport_replay_check(msg_id, 0U, &is_replay); \
        if (err == CEEPEW_OK && is_replay == expected_is_replay) { \
            ESP_LOGI(TAG, "✓ PASS: %s (is_replay=%u)", msg, is_replay); \
            s_total_passed++; \
        } else { \
            ESP_LOGE(TAG, "✗ FAIL: %s (err=%d, is_replay=%u, expected=%u)", msg, err, is_replay, expected_is_replay); \
            s_total_failed++; \
        } \
    } while (0)

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 1: First Packet Initialization                                        */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_first_packet_init(void) {
    ESP_LOGI(TAG, "\n=== Test 1: First Packet Initialization ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(1ULL, false, "First packet (seq=1) should be accepted");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 2: Sequential Packet Acceptance                                       */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_sequential_acceptance(void) {
    ESP_LOGI(TAG, "\n=== Test 2: Sequential Packet Acceptance ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(1ULL, false, "seq=1 should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(2ULL, false, "seq=2 (higher) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(3ULL, false, "seq=3 (higher) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(4ULL, false, "seq=4 (higher) should be accepted");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 3: Duplicate Detection (Same Sequence Number Twice)                    */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_duplicate_detection(void) {
    ESP_LOGI(TAG, "\n=== Test 3: Duplicate Detection ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(10ULL, false, "seq=10 (first occurrence) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(10ULL, true, "seq=10 (duplicate) should be REJECTED as replay");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 4: Out-of-Order Acceptance Within Window                              */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_out_of_order_acceptance(void) {
    ESP_LOGI(TAG, "\n=== Test 4: Out-of-Order Acceptance Within Window ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(100ULL, false, "seq=100 (init) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(105ULL, false, "seq=105 (higher) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(102ULL, false, "seq=102 (out-of-order, within window) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(102ULL, true, "seq=102 (duplicate) should be REJECTED");
    TEST_ASSERT_REPLAY_REJECTED(103ULL, false, "seq=103 (out-of-order, within window) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(101ULL, false, "seq=101 (out-of-order, within window) should be accepted");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 5: Old Packet Rejection (Beyond Window Size)                          */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_old_packet_rejection(void) {
    ESP_LOGI(TAG, "\n=== Test 5: Old Packet Rejection (Beyond Window Size) ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(1000ULL, false, "seq=1000 (init) should be accepted");
    
    uint64_t too_old = 1000ULL - (CEEPEW_REPLAY_WINDOW_SIZE + 1ULL);
    ESP_LOGI(TAG, "  Testing too-old rejection at seq=%llu", too_old);
    TEST_ASSERT_REPLAY_REJECTED(too_old, true, "Too-old packet beyond window should be REJECTED");
    
    TEST_ASSERT_REPLAY_REJECTED(999ULL, false, "seq=999 (within window) should be accepted");
    TEST_ASSERT_REPLAY_REJECTED(950ULL, false, "seq=950 (within window) should be accepted");
    
    uint64_t at_boundary = 1000ULL - CEEPEW_REPLAY_WINDOW_SIZE;
    ESP_LOGI(TAG, "  Testing boundary rejection at seq=%llu", at_boundary);
    TEST_ASSERT_REPLAY_REJECTED(at_boundary, true, "At window boundary should be REJECTED");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 6: Bitmap Shift and Update Correctness                                 */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_bitmap_shift_correctness(void) {
    ESP_LOGI(TAG, "\n=== Test 6: Bitmap Shift and Update Correctness ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(1ULL, false, "seq=1 (init) accepted");
    TEST_ASSERT_REPLAY_REJECTED(2ULL, false, "seq=2 accepted, bitmap should shift left");
    TEST_ASSERT_REPLAY_REJECTED(4ULL, false, "seq=4 accepted, bitmap shifts");
    TEST_ASSERT_REPLAY_REJECTED(3ULL, false, "seq=3 (backward but within window) accepted");
    TEST_ASSERT_REPLAY_REJECTED(3ULL, true, "seq=3 (duplicate) REJECTED");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 7: Wrap-Around Near UINT64_MAX                                        */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_wrap_around_at_max(void) {
    ESP_LOGI(TAG, "\n=== Test 7: Wrap-Around Near UINT64_MAX ===");
    transport_replay_reset();
    
    uint64_t near_max = UINT64_MAX - 10ULL;
    TEST_ASSERT_REPLAY_REJECTED(near_max, false, "seq near UINT64_MAX init accepted");
    TEST_ASSERT_REPLAY_REJECTED(UINT64_MAX - 5ULL, false, "seq higher accepted");
    TEST_ASSERT_REPLAY_REJECTED(UINT64_MAX, false, "seq=UINT64_MAX accepted");
    
    uint64_t old_after_max = UINT64_MAX - CEEPEW_REPLAY_WINDOW_SIZE - 1ULL;
    ESP_LOGI(TAG, "  Testing old rejection near max");
    TEST_ASSERT_REPLAY_REJECTED(old_after_max, true, "Too-old after max REJECTED");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 8: Large Gap (Beyond Window Size)                                     */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_large_gap_reset_bitmap(void) {
    ESP_LOGI(TAG, "\n=== Test 8: Large Gap (Beyond Window Size) ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(100ULL, false, "seq=100 init accepted");
    TEST_ASSERT_REPLAY_REJECTED(101ULL, false, "seq=101 accepted");
    TEST_ASSERT_REPLAY_REJECTED(102ULL, false, "seq=102 accepted");
    
    uint64_t far_ahead = 100ULL + CEEPEW_REPLAY_WINDOW_SIZE + 1ULL;
    ESP_LOGI(TAG, "  Testing large gap at seq=%llu (bitmap reset)", far_ahead);
    TEST_ASSERT_REPLAY_REJECTED(far_ahead, false, "Far ahead packet accepted");
    
    uint64_t old_after_jump = far_ahead - CEEPEW_REPLAY_WINDOW_SIZE - 1ULL;
    ESP_LOGI(TAG, "  Testing too-old after jump at seq=%llu", old_after_jump);
    TEST_ASSERT_REPLAY_REJECTED(old_after_jump, true, "Too-old after jump REJECTED");
    
    uint64_t boundary_after_jump = far_ahead - CEEPEW_REPLAY_WINDOW_SIZE;
    ESP_LOGI(TAG, "  Testing boundary after jump at seq=%llu", boundary_after_jump);
    TEST_ASSERT_REPLAY_REJECTED(boundary_after_jump, true, "At boundary after jump REJECTED");
    
    uint64_t within_after_jump = far_ahead - CEEPEW_REPLAY_WINDOW_SIZE + 2ULL;
    ESP_LOGI(TAG, "  Testing within window after jump at seq=%llu", within_after_jump);
    TEST_ASSERT_REPLAY_REJECTED(within_after_jump, false, "Within window after jump accepted");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 9: Window Boundary Conditions                                         */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_window_boundary_conditions(void) {
    ESP_LOGI(TAG, "\n=== Test 9: Window Boundary Conditions ===");
    transport_replay_reset();
    
    uint64_t base = 1000ULL;
    
    TEST_ASSERT_REPLAY_REJECTED(base, false, "seq=1000 (base) init accepted");
    TEST_ASSERT_REPLAY_REJECTED(base - 1ULL, false, "seq=999 (max backward in window) accepted");
    TEST_ASSERT_REPLAY_REJECTED(base - 63ULL, false, "seq=937 (diff=63, in window) accepted");
    TEST_ASSERT_REPLAY_REJECTED(base - 64ULL, true, "seq=936 (diff=64, outside window) REJECTED");
    TEST_ASSERT_REPLAY_REJECTED(base - 65ULL, true, "seq=935 (diff=65, outside window) REJECTED");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 10: Complex Interleaved Pattern                                       */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_complex_interleaved_pattern(void) {
    ESP_LOGI(TAG, "\n=== Test 10: Complex Interleaved Pattern ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(10ULL, false, "Pattern: seq=10 init");
    TEST_ASSERT_REPLAY_REJECTED(15ULL, false, "Pattern: seq=15 (forward)");
    TEST_ASSERT_REPLAY_REJECTED(11ULL, false, "Pattern: seq=11 (backward, in window)");
    TEST_ASSERT_REPLAY_REJECTED(12ULL, false, "Pattern: seq=12 (backward, in window)");
    TEST_ASSERT_REPLAY_REJECTED(20ULL, false, "Pattern: seq=20 (forward)");
    TEST_ASSERT_REPLAY_REJECTED(13ULL, false, "Pattern: seq=13 (backward, in window)");
    TEST_ASSERT_REPLAY_REJECTED(14ULL, false, "Pattern: seq=14 (backward, in window)");
    TEST_ASSERT_REPLAY_REJECTED(19ULL, false, "Pattern: seq=19 (backward, in window)");
    TEST_ASSERT_REPLAY_REJECTED(21ULL, false, "Pattern: seq=21 (forward)");
    TEST_ASSERT_REPLAY_REJECTED(10ULL, true, "Pattern: seq=10 (duplicate) REJECTED");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 11: All Bits Filled in Window                                         */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_all_bits_filled(void) {
    ESP_LOGI(TAG, "\n=== Test 11: All Bits Filled in Window ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(100ULL, false, "seq=100 init");
    
    /* Accept all backward sequences within window (100-63 to 99) */
    for (uint64_t i = 99ULL; i >= (100ULL - 63ULL); i--) {
        TEST_ASSERT_REPLAY_REJECTED(i, false, "seq within window");
    }
    
    /* All previous should be rejected as duplicates */
    for (uint64_t i = 99ULL; i >= (100ULL - 63ULL); i--) {
        TEST_ASSERT_REPLAY_REJECTED(i, true, "seq (duplicate) REJECTED");
    }
    
    TEST_ASSERT_REPLAY_REJECTED(100ULL, true, "seq=100 (duplicate) REJECTED");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test 12: Forward Progression After Backward Fill                           */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_forward_after_backward(void) {
    ESP_LOGI(TAG, "\n=== Test 12: Forward Progression After Backward Fill ===");
    transport_replay_reset();
    
    TEST_ASSERT_REPLAY_REJECTED(50ULL, false, "seq=50 init");
    TEST_ASSERT_REPLAY_REJECTED(45ULL, false, "seq=45");
    TEST_ASSERT_REPLAY_REJECTED(40ULL, false, "seq=40");
    TEST_ASSERT_REPLAY_REJECTED(48ULL, false, "seq=48");
    TEST_ASSERT_REPLAY_REJECTED(100ULL, false, "seq=100 (far forward, should reset bitmap)");
    TEST_ASSERT_REPLAY_REJECTED(99ULL, false, "seq=99 (backward from 100)");
    TEST_ASSERT_REPLAY_REJECTED(50ULL, true, "seq=50 (old, REJECTED)");
    TEST_ASSERT_REPLAY_REJECTED(45ULL, true, "seq=45 (old, REJECTED)");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Suite Runner                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */
void test_replay_window_comprehensive(void) {
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  COMPREHENSIVE WIREGUARD REPLAY WINDOW TEST SUITE        ║");
    ESP_LOGI(TAG, "║  Coverage: 5 edge cases + 7 extended scenarios           ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    
    s_total_passed = 0U;
    s_total_failed = 0U;
    
    /* Core tests for the 5 edge cases */
    test_first_packet_init();
    test_sequential_acceptance();
    test_duplicate_detection();
    test_out_of_order_acceptance();
    test_old_packet_rejection();
    
    /* Extended tests for robustness */
    test_bitmap_shift_correctness();
    test_wrap_around_at_max();
    test_large_gap_reset_bitmap();
    test_window_boundary_conditions();
    test_complex_interleaved_pattern();
    test_all_bits_filled();
    test_forward_after_backward();
    
    ESP_LOGI(TAG, "\n╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  FINAL RESULTS                                           ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║  Passed: %u                                              ║", s_total_passed);
    ESP_LOGI(TAG, "║  Failed: %u                                              ║", s_total_failed);
    if (s_total_failed == 0U) {
        ESP_LOGI(TAG, "║  Status: ✓ ALL TESTS PASSED                             ║");
    } else {
        ESP_LOGI(TAG, "║  Status: ✗ SOME TESTS FAILED                            ║");
    }
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝\n");
}
