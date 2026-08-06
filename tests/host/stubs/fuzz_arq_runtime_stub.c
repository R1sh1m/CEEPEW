/* tests/host/stubs/fuzz_arq_runtime_stub.c - host stubs for the
 * ESP-IDF / HAL runtime dependencies of components/ecc/ecc_arq.c when
 * compiled for the fuzz_arq fuzz harness.
 *
 * NOTE: unlike arq_runtime_stub.c, this file intentionally does NOT
 * provide transport_espnow_send / transport_wait_ack /
 * transport_espnow_rendezvous_drive - the fuzz_arq.c harness defines
 * its own inline transport stubs, and duplicate definitions would
 * break the link.
 */

#include <stdint.h>
#include "ceepew_assert.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

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
