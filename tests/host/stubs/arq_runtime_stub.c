/* tests/host/stubs/arq_runtime_stub.c — host stubs for the runtime
 * dependencies of components/ecc/ecc_arq.c.
 *
 * ecc_arq.c (and its included hal_radio.h) reference the following
 * ESP-IDF / HAL / transport symbols. None of them are reached by the
 * ARQ sync-exchange tests (which exercise ecc_arq_encode/decode/reset
 * only), but they must LINK, so they are provided here as no-ops.
 */

#include <stdint.h>
#include "ceepew_assert.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

/* ── ESP-IDF timing / random (stubs) ─────────────────────────────── */

uint32_t esp_random(void)
{
    return 0x12345678U;
}

int64_t esp_timer_get_time(void)
{
    return 0;
}

void vTaskDelay(TickType_t xTicksToDelay)
{
    (void)xTicksToDelay;
}

/* ── hal_radio functions referenced by ecc_arq.c (stubs) ─────────── */

CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len)
{
    (void)buf; (void)len;
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_hop_sync_callbacks(void (*pre_hop_cb)(void),
                                             void (*post_hop_cb)(void))
{
    (void)pre_hop_cb; (void)post_hop_cb;
    return CEEPEW_OK;
}

/* ── transport functions forward-declared by ecc_arq.c (stubs) ───── */

CeePewErr_t transport_espnow_send(const uint8_t *peer_mac, const uint8_t *data, uint16_t len)
{
    (void)peer_mac; (void)data; (void)len;
    return CEEPEW_OK;
}

CeePewErr_t transport_wait_ack(const uint8_t *peer_mac, uint16_t seq, uint32_t timeout_ms)
{
    (void)peer_mac; (void)seq; (void)timeout_ms;
    return CEEPEW_OK;
}

CeePewErr_t transport_espnow_rendezvous_drive(void)
{
    return CEEPEW_OK;
}
