/* components/ceepew_hal/hal_gpio.c */

#include "hal_gpio.h"
#include "hal_pins.h"
#include "ceepew_assert.h"

#include "driver/gpio.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

static bool s_initialised = false;

CeePewErr_t hal_gpio_init(void)
{
    CEEPEW_ASSERT(!s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(GPIO_IS_VALID_GPIO(CEEPEW_PIN_BUTTON) &&
                  GPIO_IS_VALID_GPIO(CEEPEW_PIN_DIAG_SWITCH), CEEPEW_ERR_PINS);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CEEPEW_PIN_BUTTON) |
                        (1ULL << CEEPEW_PIN_DIAG_SWITCH),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t rc = gpio_config(&cfg);
    CEEPEW_ASSERT(rc == ESP_OK, CEEPEW_ERR_HW);

    s_initialised = true;
    return CEEPEW_OK;
}
