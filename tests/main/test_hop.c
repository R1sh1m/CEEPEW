/* tests/main/test_hop.c
 * Unit test for transport_hop permutation determinism.
 *
 * Invoked from tests/main/integration_test_e2e.c:integration_tests_run_all()
 * via the test_hop_determinism() entry point. No constructor — the
 * diagnostic harness drives the call deterministically after hardware
 * initialization.
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "transport_hop.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "TEST_HOP";

void test_hop_determinism(void){
    CryptoCtx_t ctx = {0};
    ctx.session_active = true;
    /* Fixed key */
    for (uint8_t i = 0; i < CEEPEW_SESSION_KEY_BYTES; i++) { ctx.ascon_key[i] = (uint8_t)i; }

    uint64_t nonce_counter = (uint64_t)(12345ULL << CEEPEW_HOP_SHIFT); /* pick a nonce that yields non-zero hop_idx */

    uint8_t ch1 = 0U;
    uint8_t ch2 = 0U;

    CeePewErr_t err = transport_get_current_channel(&ctx, nonce_counter, &ch1);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "transport_get_current_channel failed: %d", (int)err);
        return;
    }

    /* Repeat with same nonce to ensure deterministic output */
    err = transport_get_current_channel(&ctx, nonce_counter, &ch2);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "transport_get_current_channel failed (2): %d", (int)err);
        return;
    }

    if (ch1 == ch2) {
        ESP_LOGI(TAG, "[PASS] deterministic channel: %u", (unsigned)ch1);
    } else {
        ESP_LOGE(TAG, "[FAIL] channel mismatch: %u vs %u", (unsigned)ch1, (unsigned)ch2);
    }
}
