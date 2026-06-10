/* components/ceepew_hal/hal_temp.c
 *
 * Die-temperature sensor driver for ESP32-WROOM-32.
 * See hal_temp.h for the hardware-accuracy caveat.
 */
#include "hal_temp.h"

#include "ceepew_assert.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_rom_sys.h"

#define HAL_TEMP_SAMPLES_PER_READ 4U
#define HAL_TEMP_TAG              "hal_temp"

extern uint8_t temprature_sens_read(void);

static bool s_ready = false;

CeePewErr_t hal_temp_init(void)
{
    ESP_LOGW(HAL_TEMP_TAG,
             "die-temp sensor initialised; accuracy is ±10 C and the "
             "reading is biased upward by WiFi/BT self-heating (not a "
             "board-ambient value).");
    s_ready = true;
    return CEEPEW_OK;
}

bool hal_temp_is_ready(void)
{
    return s_ready;
}

bool hal_temp_read_celsius(float *out_celsius)
{
    CEEPEW_ASSERT(out_celsius != NULL, CEEPEW_ERR_NULL_PTR);

    if ((out_celsius == NULL) || (!s_ready)) {
        return false;
    }

    uint32_t accum_f = 0U;
    for (uint32_t i = 0U; i < HAL_TEMP_SAMPLES_PER_READ; i++) {
        accum_f += (uint32_t)temprature_sens_read();
    }
    const float avg_f = (float)accum_f / (float)HAL_TEMP_SAMPLES_PER_READ;
    *out_celsius = (avg_f - 32.0f) / 1.8f;
    return true;
}
