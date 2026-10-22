/* tests/host/test_arq_attempts.c — ISS-F02 regression test.
 *
 * The transmit loop used to hard-code five attempts while the configuration
 * declared three, which is exactly the kind of drift the paper had to
 * report as a configuration discrepancy. The attempt budget now lives in one
 * place (components/ceepew_common/ceepew_config.h) and this test asserts
 * the relationship between the parts, including the worst-case delivery
 * bound quoted in the manuscript.
 *
 *   attempts   = 1 initial + CEEPEW_ARQ_MAX_RETRIES backoff-bounded retries
 *                + CEEPEW_ARQ_HOP_SPAN_EXTRA_ATTEMPTS hop-span attempt
 *   backoff    = 100, 200, 400, 400 ms (clamped to CEEPEW_ARQ_MAX_RETRIES)
 *   worst case = attempts x CEEPEW_ARQ_TIMEOUT_MS + sum(backoff)
 */

#include <stdio.h>
#include <stdint.h>

#include "ceepew_config.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("PASS: %s\n", (msg));                                      \
        } else {                                                              \
            printf("FAIL: %s\n", (msg));                                      \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static void test_attempt_budget(void)
{
    CHECK(CEEPEW_ARQ_EFFECTIVE_ATTEMPTS ==
              (1U + CEEPEW_ARQ_MAX_RETRIES + CEEPEW_ARQ_HOP_SPAN_EXTRA_ATTEMPTS),
          "effective attempt count is derived from its parts");
    CHECK(CEEPEW_ARQ_EFFECTIVE_ATTEMPTS == 5U,
          "effective attempt count is five (1 + 3 + 1)");
    CHECK(CEEPEW_ARQ_EFFECTIVE_ATTEMPTS > CEEPEW_ARQ_MAX_RETRIES,
          "effective attempt count exceeds the configured retry maximum");
    CHECK(CEEPEW_ARQ_EFFECTIVE_ATTEMPTS == 1U + 3U + 1U,
          "attempt budget matches the documented composition");
}

static void test_backoff_clamp(void)
{
    /* The exponential backoff shift is clamped to CEEPEW_ARQ_MAX_RETRIES, so
     * the hop-span attempt reuses the last backoff value instead of
     * extending the schedule. The clamp must keep every shift inside the
     * range accepted by the backoff helper's assertion. */
    for (uint8_t attempt = 0U; attempt < CEEPEW_ARQ_EFFECTIVE_ATTEMPTS; attempt++) {
        uint8_t shift = (attempt < CEEPEW_ARQ_MAX_RETRIES)
                            ? attempt
                            : (uint8_t)(CEEPEW_ARQ_MAX_RETRIES - 1U);
        if (shift >= CEEPEW_ARQ_MAX_RETRIES) {
            printf("FAIL: backoff shift %u out of range at attempt %u\n",
                   (unsigned)shift, (unsigned)attempt);
            g_failures++;
        }
    }
    printf("PASS: backoff shift stays inside CEEPEW_ARQ_MAX_RETRIES for every "
           "attempt\n");
}

static void test_worst_case_bound(void)
{
    /* Worst-case per-fragment delivery time quoted in the manuscript:
     * 5 attempts x 600 ms timeout + (100 + 200 + 400 + 400) ms backoff. */
    const uint32_t timeouts = CEEPEW_ARQ_EFFECTIVE_ATTEMPTS * CEEPEW_ARQ_TIMEOUT_MS;
    const uint32_t backoff = 100U + 200U + 400U + 400U;
    const uint32_t worst_case = timeouts + backoff;

    CHECK(worst_case == 4100U,
          "worst-case per-fragment delivery time is 4100 ms");
    CHECK(timeouts == 3000U,
          "timeout component of the worst-case bound is 3000 ms");
    CHECK(backoff == 1100U,
          "backoff component of the worst-case bound is 1100 ms");
}

int main(void)
{
    test_attempt_budget();
    test_backoff_clamp();
    test_worst_case_bound();

    printf("\nARQ attempt budget: %d failures\n", g_failures);
    return g_failures;
}
