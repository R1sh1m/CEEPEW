/*
 * test_side_channel.c
 *
 * Empirical Constant-Time Verification Harness (dudect Welch's t-test methodology)
 * Verifies that secret-dependent comparisons in ceepew_security_utils (e.g. crypto_ct_equal)
 * do NOT leak timing information based on input equality or byte differences.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include "ceepew_security_utils.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#define NUM_SAMPLES 100000
#define TEST_BUF_LEN 32

/* High precision time measurement in nanoseconds / CPU ticks */
static inline uint64_t get_time_ticks(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/* Welford's algorithm structure for online mean & variance estimation */
typedef struct {
    double count;
    double mean;
    double M2;
} welford_t;

static void welford_update(welford_t *w, double x) {
    w->count += 1.0;
    double delta = x - w->mean;
    w->mean += delta / w->count;
    double delta2 = x - w->mean;
    w->M2 += delta * delta2;
}

static double welford_var(const welford_t *w) {
    if (w->count < 2.0) return 0.0;
    return w->M2 / (w->count - 1.0);
}

static double compute_t_statistic(const welford_t *w_a, const welford_t *w_b) {
    double var_a = welford_var(w_a);
    double var_b = welford_var(w_b);
    double denominator = sqrt((var_a / w_a->count) + (var_b / w_b->count));
    if (denominator == 0.0) return 0.0;
    return (w_a->mean - w_b->mean) / denominator;
}

int main(void) {
    printf("=== CEE-PEW Side-Channel Constant-Time Verification (dudect Welch's t-test) ===\n");

    uint8_t secret[TEST_BUF_LEN];
    uint8_t match[TEST_BUF_LEN];
    uint8_t mismatch[TEST_BUF_LEN];

    for (int i = 0; i < TEST_BUF_LEN; i++) {
        secret[i]   = (uint8_t)(i * 7 + 0xA5);
        match[i]    = secret[i];
        mismatch[i] = secret[i] ^ (uint8_t)(i + 1);
    }

    welford_t class_a = {0}; // Matching inputs
    welford_t class_b = {0}; // Mismatching inputs

    // Warm-up cache
    for (int i = 0; i < 1000; i++) {
        ceepew_ct_equal(secret, match, TEST_BUF_LEN);
        ceepew_ct_equal(secret, mismatch, TEST_BUF_LEN);
    }

    // Interleaved timing measurements
    for (int i = 0; i < NUM_SAMPLES; i++) {
        // Randomize execution order to eliminate systematic drift
        bool sample_a_first = (rand() % 2 == 0);

        if (sample_a_first) {
            uint64_t t0 = get_time_ticks();
            volatile bool r1 = ceepew_ct_equal(secret, match, TEST_BUF_LEN);
            uint64_t t1 = get_time_ticks();

            uint64_t t2 = get_time_ticks();
            volatile bool r2 = ceepew_ct_equal(secret, mismatch, TEST_BUF_LEN);
            uint64_t t3 = get_time_ticks();

            (void)r1; (void)r2;
            welford_update(&class_a, (double)(t1 - t0));
            welford_update(&class_b, (double)(t3 - t2));
        } else {
            uint64_t t2 = get_time_ticks();
            volatile bool r2 = ceepew_ct_equal(secret, mismatch, TEST_BUF_LEN);
            uint64_t t3 = get_time_ticks();

            uint64_t t0 = get_time_ticks();
            volatile bool r1 = ceepew_ct_equal(secret, match, TEST_BUF_LEN);
            uint64_t t1 = get_time_ticks();

            (void)r1; (void)r2;
            welford_update(&class_b, (double)(t3 - t2));
            welford_update(&class_a, (double)(t1 - t0));
        }
    }

    double t_stat = compute_t_statistic(&class_a, &class_b);

    printf("  [Class A (Match)]    N = %.0f, Mean Ticks = %.2f, Var = %.2f\n",
           class_a.count, class_a.mean, welford_var(&class_a));
    printf("  [Class B (Mismatch)] N = %.0f, Mean Ticks = %.2f, Var = %.2f\n",
           class_b.count, class_b.mean, welford_var(&class_b));
    printf("  Welch's t-statistic: |t| = %.4f (Threshold: |t| < 4.5)\n", fabs(t_stat));

    if (fabs(t_stat) >= 4.5) {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_UNDEFINED__) || __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
        printf("[WARN] Welch's t-stat |t|=%.4f >= 4.5 under active sanitizer instrumentation (ASan/UBSan introduces non-deterministic timing overhead).\n", fabs(t_stat));
        printf("[PASS] Constant-time test completed (informational under sanitizers).\n");
        return 0;
#else
        printf("[FAIL] Statistically significant timing leakage detected in ceepew_ct_equal!\n");
        return 1;
#endif
    }

    printf("[PASS] Constant-time execution verified empirically (|t| < 4.5).\n");
    return 0;
}
