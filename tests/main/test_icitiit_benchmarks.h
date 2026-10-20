/* tests/main/test_icitiit_benchmarks.h
 *
 * STEP 2 on-device micro-benchmark for the ICIT-IIT paper.
 * Self-contained stage timings (Xtensa LX6 @ 240 MHz) over the worst-case
 * 1-fragment message chain. See test_icitiit_benchmarks.c for details.
 */

#ifndef TEST_ICITIIT_BENCHMARKS_H
#define TEST_ICITIIT_BENCHMARKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run all on-device benchmarks. Logs results to ESP_LOG.
 * Zero-heap: static buffers only. Safe to call once from
 * integration_tests_run_all(). */
void test_icitiit_paper_benchmarks(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_ICITIIT_BENCHMARKS_H */
