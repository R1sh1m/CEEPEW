/* components/ceepew_hal/hal_pins.c */

#include "hal_pins.h"
#include "ceepew_assert.h"
#include <stdint.h>

/* Validate pin layout at runtime (no-op if compile-time asserts pass).
 * Non-trivial function: two CEEPEW_ASSERT checks required by project rules.
 */
CeePewErr_t hal_pins_validate(void) {
    CEEPEW_ASSERT(((int)CEEPEW_PIN_I2C_SDA) >= 0 && ((int)CEEPEW_PIN_I2C_SDA) <= 39, CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(((int)CEEPEW_PIN_I2C_SCL) >= 0 && ((int)CEEPEW_PIN_I2C_SCL) <= 39, CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(CEEPEW_PIN_I2C_SDA != -1, CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(CEEPEW_PIN_I2C_SCL != -1, CEEPEW_ERR_PINS);
    /* Compile-time uniqueness checks provided by header macro */
    CEEPEW_PINS_ASSERT_UNIQUE();
    return CEEPEW_OK;
}
