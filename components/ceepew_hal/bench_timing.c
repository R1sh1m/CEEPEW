/* components/ceepew_hal/bench_timing.c
 *
 * E5: Microsecond sync-pulse instrumentation for one-way OTA latency measurement.
 *
 * Hardware: GPIO14 shared wire between both boards, open-drain with internal
 * pull-up. Assert = drive LOW; release = drive HIGH (high-Z, pulled up). We time
 * the FALLING edge, which is driver-dominated and sharp, and therefore immune to
 * the slow RC rise of the pull-up.
 *
 * All timestamps are esp_timer_get_time() microseconds. The two boards have
 * INDEPENDENT clocks, so only same-clock intervals are meaningful:
 *   TX side: submit -> pulse, pulse -> sendcb
 *   RX side: edge -> rxcb (headline one-way figure), rxcb -> accept
 * A cross-board subtraction is never valid and this module never performs one.
 *
 * MEMORY DESIGN (important): this firmware has effectively ZERO spare DRAM --
 * a 384-entry static ring overflowed `dram0_0_seg` by 21248 B against a budget
 * of ~256 B. Records are therefore NOT buffered. Each record is printed the
 * moment it completes, which is always AFTER every interval it reports:
 *   - TX prints at the ESP-NOW send callback (5 s campaign gap follows),
 *   - RX prints at frame accept (both edge->rxcb and rxcb->accept are frozen).
 * A ~110-byte line at 115200 baud costs ~9 ms, and both print points sit
 * outside every measured window, so this cannot perturb the measurement.
 * Total static state below is well under 100 bytes.
 */

#include "bench_timing.h"

#include <stdio.h>

#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"

#if CONFIG_CEEPEW_BENCH_TIMING

/* Parity with the driver-facing pin. Never used for a plain gpio_set_level on a
 * push-pull pin: the line is shared, so push-pull would short 3.3 V to GND. */
#define SYNC_GPIO_NUM ((gpio_num_t)CONFIG_CEEPEW_PIN_SYNC)

/* An edge landing within this window of our own pulse is OUR edge, not the
 * peer's. The s_self_pulse_active flag alone is not reliable: the GPIO ISR can
 * run after the pulse has already been released. */
#define BENCH_TIMING_SELF_EDGE_GUARD_US 2000ULL

/* ---- Pending TX record (created by the BENCH58 hook, completed by send_cb) */
static volatile uint32_t s_tx_seq        = 0;
static volatile uint64_t s_tx_submit_us  = 0;
static volatile uint64_t s_tx_pulse_us   = 0;
static volatile bool     s_tx_pending    = false;

/* ---- Pending RX record (created at rx_cb, completed at accept) ----------- */
static volatile uint32_t s_rx_seq        = 0;
static volatile uint64_t s_rx_edge_us    = 0;
static volatile uint64_t s_rx_cb_us      = 0;
static volatile bool     s_rx_pending    = false;

/* ---- Own-pulse stamp and ISR edge latch --------------------------------- */
static volatile uint64_t s_last_self_pulse_us    = 0;
static volatile uint64_t s_isr_edge_timestamp_us = 0;
static volatile bool     s_isr_edge_pending      = false;

/* ISR: latch the falling-edge timestamp. IRAM_ATTR, no logging, no allocation,
 * no blocking. Self-vs-peer attribution is decided later against
 * s_last_self_pulse_us, because the ISR may run after the pulse was released. */
static void IRAM_ATTR bench_pulse_isr(void *arg)
{
    (void)arg;
    s_isr_edge_timestamp_us = esp_timer_get_time();
    s_isr_edge_pending = true;
}

void bench_timing_init(void)
{
    s_tx_pending = false;
    s_rx_pending = false;
    s_isr_edge_pending = false;
    s_last_self_pulse_us = 0;

    /* Open-drain so that both boards may drive the shared wire simultaneously
     * without a short: assert = output 0, release = output 1 = high-Z. */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SYNC_GPIO_NUM),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t rc = gpio_config(&io_conf);
    if (rc != ESP_OK) {
        ESP_LOGE("BENCH_TIMING", "gpio_config failed: %d", rc);
        return;
    }

    /* The GPIO ISR service may already be installed by another module; that is
     * not an error for us. */
    static bool s_isr_installed = false;
    if (!s_isr_installed) {
        rc = gpio_install_isr_service(0);
        if (rc == ESP_OK || rc == ESP_ERR_INVALID_STATE) {
            s_isr_installed = true;
        } else {
            ESP_LOGE("BENCH_TIMING", "gpio_install_isr_service failed: %d", rc);
            return;
        }
    }

    rc = gpio_isr_handler_add(SYNC_GPIO_NUM, bench_pulse_isr, NULL);
    if (rc != ESP_OK) {
        ESP_LOGE("BENCH_TIMING", "gpio_isr_handler_add failed: %d", rc);
        return;
    }

    ESP_LOGI("BENCH_TIMING", "sync-pulse instrumentation ready on GPIO%d (input_output_od, negedge)",
             (int)SYNC_GPIO_NUM);
}

uint64_t bench_timing_pulse(void)
{
    uint64_t t_assert = esp_timer_get_time();
    s_last_self_pulse_us = t_assert;

    /* Attribute this pulse to the pending TX record. The BENCH58 hook creates
     * the record a few hundred ms (crypto + ARQ) before the frame reaches the
     * radio, so the next pulse after the record belongs to that frame. */
    if (s_tx_pending && s_tx_pulse_us == 0ULL && t_assert >= s_tx_submit_us) {
        s_tx_pulse_us = t_assert;
    }

    /* Assert: drive LOW. Keep the low period tiny (~5 us) and log nothing
     * inside the timed region. */
    gpio_set_level(SYNC_GPIO_NUM, 0);
    volatile int delay = 20;  /* ~5 us at 240 MHz */
    while (delay-- > 0) {
        __asm__ __volatile__("nop");
    }
    /* Release: drive HIGH = high-Z on an open-drain pin, pulled up. */
    gpio_set_level(SYNC_GPIO_NUM, 1);

    /* Driving the shared line low also latches a falling edge on OUR OWN input
     * path. Discard it so it cannot later be mistaken for the peer's pulse. The
     * guard window in record_rx_cb covers the race where our ISR runs after
     * this clear. */
    s_isr_edge_pending = false;

    return t_assert;
}

void bench_timing_record_tx(uint32_t seq, uint64_t t_submit_us)
{
    s_tx_seq = seq;
    s_tx_submit_us = t_submit_us;
    s_tx_pulse_us = 0;
    s_tx_pending = true;
}

void bench_timing_record_send_cb(uint32_t seq)
{
    (void)seq;  /* single pending slot; seq matching is not needed */

    if (!s_tx_pending) {
        return;  /* transmission not part of the BENCH58 campaign */
    }

    uint64_t t_cb = esp_timer_get_time();
    uint64_t t_submit = s_tx_submit_us;
    uint64_t t_pulse = s_tx_pulse_us;
    uint32_t sq = s_tx_seq;
    s_tx_pending = false;

    printf("BENCH_TIME seq=%lu role=tx submit=%llu pulse=%llu sendcb=%llu edge=%llu rxcb=%llu accept=%llu\n",
           (unsigned long)sq,
           (unsigned long long)t_submit,
           (unsigned long long)t_pulse,
           (unsigned long long)t_cb,
           0ULL, 0ULL, 0ULL);
}

void bench_timing_record_edge(uint64_t t_edge_us)
{
    /* The authoritative edge timestamp is latched by the GPIO ISR in
     * s_isr_edge_timestamp_us; this hook is intentionally inert. */
    (void)t_edge_us;
}

void bench_timing_record_rx_cb(uint32_t seq, uint64_t t_rx_cb_us)
{
    /* Take the PEER's pulse edge if one is pending. An edge belonging to our own
     * recent pulse is rejected, leaving edge_us = 0 = "no peer pulse observed",
     * which the analysis tool reports as a lost pulse rather than silently
     * producing a bogus edge->rxcb interval. */
    uint64_t edge_us = 0;
    if (s_isr_edge_pending) {
        uint64_t cand = s_isr_edge_timestamp_us;
        s_isr_edge_pending = false;
        uint64_t self_us = s_last_self_pulse_us;
        bool is_self = (self_us != 0ULL) &&
                       ((cand >= self_us ? (cand - self_us) : (self_us - cand))
                        < BENCH_TIMING_SELF_EDGE_GUARD_US);
        if (!is_self) {
            edge_us = cand;
        }
    }

    s_rx_seq = seq;
    s_rx_edge_us = edge_us;
    s_rx_cb_us = t_rx_cb_us;
    s_rx_pending = true;
}

void bench_timing_note_accept(uint32_t seq)
{
    (void)seq;
    if (!s_rx_pending) {
        return;
    }

    uint64_t t_accept = esp_timer_get_time();
    uint64_t edge_us = s_rx_edge_us;
    uint64_t t_rx_cb = s_rx_cb_us;
    uint32_t sq = s_rx_seq;
    s_rx_pending = false;

    /* Printed here, after edge->rxcb and rxcb->accept are both frozen. */
    printf("BENCH_TIME seq=%lu role=rx submit=%llu pulse=%llu sendcb=%llu edge=%llu rxcb=%llu accept=%llu\n",
           (unsigned long)sq,
           0ULL, 0ULL, 0ULL,
           (unsigned long long)edge_us,
           (unsigned long long)t_rx_cb,
           (unsigned long long)t_accept);
}

void bench_timing_dump(void)
{
    /* Records are streamed as they complete; there is no buffer to dump. This
     * marker delimits the end of the campaign in the capture log. */
    printf("BENCH_TIME_DUMP_COMPLETE\n");
}

#endif /* CONFIG_CEEPEW_BENCH_TIMING */
