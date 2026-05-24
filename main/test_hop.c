/* main/test_hop.c
 * Unit test for transport_hop permutation determinism.
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "transport_hop.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "TEST_HOP";

void test_hop_determinism(void){
    CryptoCtx_t ctx = {0};
    /* Fixed key */
    for (uint8_t i = 0; i < CEEPEW_SESSION_KEY_BYTES; i++) { ctx.ascon_key[i] = (uint8_t)i; }
    ctx.nonce_counter = (uint64_t) (12345ULL << CEEPEW_HOP_SHIFT); /* pick a nonce that yields non-zero hop_idx */

    uint8_t ch1 = 0U;
    uint8_t ch2 = 0U;

    CeePewErr_t err = transport_get_current_channel(&ctx, &ch1);
    if (err != CEEPEW_OK) {
        ESP_LOGE(TAG, "transport_get_current_channel failed: %d", (int)err);
        return;
    }

    /* Repeat with same ctx to ensure deterministic output */
    err = transport_get_current_channel(&ctx, &ch2);
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

/* Optionally call test on startup - keep non-intrusive: user can invoke via integration tests. */
__attribute__((constructor)) static void run_test_on_startup(void){
    test_hop_determinism();
}
