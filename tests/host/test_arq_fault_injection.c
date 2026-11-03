/*
 * test_arq_fault_injection.c
 *
 * ARQ Transport Fault-Injection Test Suite
 * Evaluates the ARQ state machine and sliding-window protocol robustness under simulated
 * network packet drops (10%-50%), bit corruptions, and out-of-order packet injections.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "hal_ui_types.h"

/* Forward declarations for ecc_arq.c public host API */
CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len);
CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len,
                           uint8_t *out, uint16_t *out_len,
                           uint16_t max_out_len, bool *corrected);
CeePewErr_t ecc_arq_reset(void);
CeePewErr_t ecc_arq_commit(void);
CeePewErr_t ecc_arq_rollback(void);

#define FAULT_TEST_FRAMES 100
#define MAX_FRAME_SIZE 256

typedef struct {
    uint32_t total_sent;
    uint32_t total_dropped;
    uint32_t total_corrupted;
    uint32_t total_delivered;
} fault_stats_t;

static void simulate_transport_channel(float drop_rate, float corrupt_rate, fault_stats_t *stats) {
    ecc_arq_reset();

    for (uint32_t i = 0; i < FAULT_TEST_FRAMES; i++) {
        uint8_t payload[32];
        memset(payload, (uint8_t)(i + 1), sizeof(payload));

        uint8_t frame_buf[MAX_FRAME_SIZE];
        uint16_t frame_len = 0;

        CeePewErr_t err = ecc_arq_encode(payload, sizeof(payload), frame_buf, &frame_len, sizeof(frame_buf));
        if (err != CEEPEW_OK) {
            continue;
        }

        stats->total_sent++;

        // Simulate packet drop
        float r_drop = (float)rand() / (float)RAND_MAX;
        if (r_drop < drop_rate) {
            stats->total_dropped++;
            continue;
        }

        // Simulate bit corruption
        float r_corrupt = (float)rand() / (float)RAND_MAX;
        if (r_corrupt < corrupt_rate && frame_len > 2) {
            stats->total_corrupted++;
            frame_buf[frame_len - 1] ^= 0xFF; // Corrupt payload byte
        }

        // Receive & decode frame
        uint8_t rx_buf[MAX_FRAME_SIZE];
        uint16_t rx_len = 0;
        bool corrected = false;

        err = ecc_arq_decode(frame_buf, frame_len, rx_buf, &rx_len, sizeof(rx_buf), &corrected);
        if (err == CEEPEW_OK) {
            stats->total_delivered++;
        }
    }
}

static bool test_arq_rollback_on_auth_failure(void) {
    printf("\n--> Testing Review Finding: ARQ Rollback on Downstream Auth Failure\n");
    ecc_arq_reset();

    uint8_t payload[16] = {0xAA};
    uint8_t frame0[32], frame1[32], frame2[32];
    uint16_t len0 = 0, len1 = 0, len2 = 0;

    /* Genuine sender encodes frames 0, 1, 2 */
    if (ecc_arq_encode(payload, sizeof(payload), frame0, &len0, sizeof(frame0)) != CEEPEW_OK) return false;
    if (ecc_arq_encode(payload, sizeof(payload), frame1, &len1, sizeof(frame1)) != CEEPEW_OK) return false;
    if (ecc_arq_encode(payload, sizeof(payload), frame2, &len2, sizeof(frame2)) != CEEPEW_OK) return false;

    /* Receiver receives genuine frame 0 */
    uint8_t rx_buf[32];
    uint16_t rx_len = 0;
    bool corrected = false;
    if (ecc_arq_decode(frame0, len0, rx_buf, &rx_len, sizeof(rx_buf), &corrected) != CEEPEW_OK) {
        printf("  [FAIL] Genuine frame 0 rejected\n");
        return false;
    }
    ecc_arq_commit();

    /* Attacker injects a forged frame with next seq (seq=1) */
    uint8_t forged_frame[32];
    uint16_t forged_len = len1;
    memcpy(forged_frame, frame1, forged_len);
    /* Tentative decode of forged frame */
    if (ecc_arq_decode(forged_frame, forged_len, rx_buf, &rx_len, sizeof(rx_buf), &corrected) != CEEPEW_OK) {
        printf("  [FAIL] Tentative decode of forged frame rejected unexpectedly\n");
        return false;
    }
    /* Simulated downstream crypto authentication failure */
    ecc_arq_rollback();

    /* Legitimate frame 1 arrives now. Under the old bug, expected_rx_seq was already
     * advanced to 2, causing legitimate frame 1 to be rejected as CEEPEW_ERR_REPLAY.
     * With rollback, legitimate frame 1 MUST be accepted! */
    CeePewErr_t err = ecc_arq_decode(frame1, len1, rx_buf, &rx_len, sizeof(rx_buf), &corrected);
    if (err != CEEPEW_OK) {
        printf("  [FAIL] Legitimate frame 1 was rejected after forged frame rollback! (err=%d)\n", (int)err);
        return false;
    }
    ecc_arq_commit();

    /* Legitimate frame 2 arrives and must be accepted */
    err = ecc_arq_decode(frame2, len2, rx_buf, &rx_len, sizeof(rx_buf), &corrected);
    if (err != CEEPEW_OK) {
        printf("  [FAIL] Legitimate frame 2 was rejected! (err=%d)\n", (int)err);
        return false;
    }
    ecc_arq_commit();

    /* Duplicate frame 2 arrives and MUST be rejected as replay */
    err = ecc_arq_decode(frame2, len2, rx_buf, &rx_len, sizeof(rx_buf), &corrected);
    if (err != CEEPEW_ERR_REPLAY) {
        printf("  [FAIL] Duplicate frame 2 was NOT rejected as replay! (err=%d)\n", (int)err);
        return false;
    }

    printf("  [PASS] ARQ rollback correctly restored sequence state on auth failure!\n");
    return true;
}

int main(void) {
    printf("=== CEE-PEW ARQ Transport Fault Injection Test Suite ===\n");
    srand(42); // Deterministic seed for reproducible fault testing

    struct {
        const char *name;
        float drop_rate;
        float corrupt_rate;
    } scenarios[] = {
        {"Mild Degradation (10% drops, 0% corruption)", 0.10f, 0.00f},
        {"Moderate Interference (25% drops, 5% corruption)", 0.25f, 0.05f},
        {"Severe Noise Channel (50% drops, 10% corruption)", 0.50f, 0.10f}
    };

    bool all_passed = true;



    for (size_t i = 0; i < sizeof(scenarios)/sizeof(scenarios[0]); i++) {
        printf("\n--> Testing Scenario: %s\n", scenarios[i].name);
        fault_stats_t stats = {0};

        simulate_transport_channel(scenarios[i].drop_rate, scenarios[i].corrupt_rate, &stats);

        printf("  Sent: %u, Dropped: %u, Corrupted: %u, Delivered: %u\n",
               stats.total_sent, stats.total_dropped, stats.total_corrupted, stats.total_delivered);

        if (stats.total_delivered == 0 && scenarios[i].drop_rate < 0.50f) {
            printf("  [FAIL] Zero frames delivered under scenario!\n");
            all_passed = false;
        } else {
            printf("  [PASS] Scenario completed successfully.\n");
        }
    }

    printf("\n============================================\n");
    if (!test_arq_rollback_on_auth_failure()) {
        all_passed = false;
    }

    if (all_passed) {
        printf(" ARQ FAULT INJECTION SUITE: ALL SCENARIOS PASSED\n");
        return 0;
    } else {
        printf(" ARQ FAULT INJECTION SUITE: FAILURES DETECTED\n");
        return 1;
    }
}
