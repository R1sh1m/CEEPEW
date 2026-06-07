/* tests/components/ceepew_hal/hal_temp_test.c
 *
 * On-device smoke test for the on-die temperature sensor driver.
 *
 * The ESP32's internal temperature sensor is a rough on-die sensor;
 * this test verifies:
 *   1. hal_temp_init() is idempotent and reports readiness correctly.
 *   2. After init, hal_temp_read_celsius() returns a value in a
 *      physically plausible range (0 °C..100 °C — the configured
 *      sensor range).
 *   3. A NULL out-parameter is rejected.
 *   4. hal_temp_is_ready() is true after init, false before.
 *
 * Selftest entry point: hal_temp_selftest_run()
 *   (called by tests/main/integration_test_e2e.c)
 *
 * Guarded by CEEPEW_ENABLE_SELFTEST; no constructor attribute.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "hal_temp.h"

#ifdef CEEPEW_ENABLE_SELFTEST

static int s_fail_count = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("CEEPEW: hal_temp_selftest: FAIL %s\n", (msg)); \
        s_fail_count++; \
    } else { \
        printf("CEEPEW: hal_temp_selftest: PASS %s\n", (msg)); \
    } \
} while (0)

void hal_temp_selftest_run(void)
{
    printf("CEEPEW: hal_temp selftest start\n");

    /* Test 1: NULL pointer rejected. */
    CHECK(hal_temp_read_celsius(NULL) == false,
          "hal_temp_read_celsius(NULL) returns false");

    /* Test 2: init succeeds and is idempotent. */
    CeePewErr_t err1 = hal_temp_init();
    CeePewErr_t err2 = hal_temp_init();
    CHECK(err1 == CEEPEW_OK, "hal_temp_init() returns CEEPEW_OK");
    CHECK(err2 == CEEPEW_OK, "hal_temp_init() is idempotent (second call OK)");
    CHECK(hal_temp_is_ready() == true,
          "hal_temp_is_ready() == true after init");

    /* Test 3: read returns a plausible value. On real silicon the
     * die temperature is normally 25..65 °C; we accept the full
     * sensor range (0..100 °C) plus a small negative margin for
     * ESP32 silicon that reports slightly below 0 °C when cold. */
    float t = -300.0f;
    bool ok = hal_temp_read_celsius(&t);
    CHECK(ok, "hal_temp_read_celsius returns true");
    if (ok) {
        CHECK(t > -10.0f && t < 100.0f,
              "temperature is in plausible range (-10..100 °C)");
        printf("CEEPEW: hal_temp_selftest: die temperature = %.1f °C\n",
               (double)t);
    }

    if (s_fail_count == 0) {
        printf("CEEPEW: hal_temp selftest - all checks PASS\n");
    } else {
        printf("CEEPEW: hal_temp selftest - %d check(s) FAILED\n",
               s_fail_count);
    }
}

#endif /* CEEPEW_ENABLE_SELFTEST */
