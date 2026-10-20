/* main/bench_timing.h
 *
 * E5: Microsecond sync-pulse instrumentation for one-way OTA latency measurement.
 * 
 * Design:
 * - Shared wire: GPIO14 on both boards, open-drain with pull-up.
 * - Assert = drive LOW; release = drive HIGH (high-Z, pulled up).
 * - Time the FALLING edge (driver-dominated, sharp).
 * - Both boards pulse and sense on the same pin during BENCH58.
 * - Self-induced edges are tagged (s_self_pulse_active flag).
 *
 * All timestamps are esp_timer_get_time() microseconds.
 * NEVER subtract timestamps across boards — only same-clock intervals are meaningful:
 *   TX side: t_submit → t_pulse, t_pulse → t_send_cb
 *   RX side: t_edge → t_rx_cb, t_rx_cb → t_accept
 */

#ifndef BENCH_TIMING_H
#define BENCH_TIMING_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_CEEPEW_BENCH_TIMING

/* Initialize GPIO14 as INPUT_OUTPUT_OD with pull-up, install falling-edge ISR.
 * Call once at startup. No logging, no allocation, no blocking. */
void bench_timing_init(void);

/* Assert sync line LOW, timestamp, release. Keep low ~5 µs.
 * Returns the timestamp (µs) when the line was asserted. */
uint64_t bench_timing_pulse(void);

/* Record a TX submission event. Stores t_submit for later pairing. */
void bench_timing_record_tx(uint32_t seq, uint64_t t_submit_us);

/* Record ESP-NOW send callback invocation. */
void bench_timing_record_send_cb(uint32_t seq);

/* Record RX edge detection (from ISR). */
void bench_timing_record_edge(uint64_t t_edge_us);

/* Record RX callback entry. */
void bench_timing_record_rx_cb(uint32_t seq, uint64_t t_rx_cb_us);

/* Record frame accept (post-decrypt/verify). */
void bench_timing_note_accept(uint32_t seq);

/* Dump all records as single-line printf output.
 * Format: BENCH_TIME seq=<n> role=<tx|rx> submit=<u64> pulse=<u64> sendcb=<u64> edge=<u64> rxcb=<u64> accept=<u64>
 * Call after BENCH58 campaign completes. */
void bench_timing_dump(void);

#else /* !CONFIG_CEEPEW_BENCH_TIMING */

static inline void bench_timing_init(void) {}
static inline uint64_t bench_timing_pulse(void) { return 0; }
static inline void bench_timing_record_tx(uint32_t seq, uint64_t t_submit_us) { (void)seq; (void)t_submit_us; }
static inline void bench_timing_record_send_cb(uint32_t seq) { (void)seq; }
static inline void bench_timing_record_edge(uint64_t t_edge_us) { (void)t_edge_us; }
static inline void bench_timing_record_rx_cb(uint32_t seq, uint64_t t_rx_cb_us) { (void)seq; (void)t_rx_cb_us; }
static inline void bench_timing_note_accept(uint32_t seq) { (void)seq; }
static inline void bench_timing_dump(void) {}

#endif /* CONFIG_CEEPEW_BENCH_TIMING */

#ifdef __cplusplus
}
#endif

#endif /* BENCH_TIMING_H */
