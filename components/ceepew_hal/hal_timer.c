/* components/ceepew_hal/hal_timer.c */

#include "hal_timer.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

CeePewErr_t hal_timer_delay_ms(uint32_t ms) {
    CEEPEW_ASSERT(ms <= 60000U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED, CEEPEW_ERR_BUSY);

    if (ms == 0U) { return CEEPEW_OK; }

    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0U) { ticks = 1U; }

    vTaskDelay(ticks);
    return CEEPEW_OK;
}
