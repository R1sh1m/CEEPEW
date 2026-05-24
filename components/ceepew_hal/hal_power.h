/* components/ceepew_hal/hal_power.h */
#ifndef CEEPEW_HAL_POWER_H
#define CEEPEW_HAL_POWER_H

#include <stdint.h>
#include "ceepew_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_POWER_WAKEUP_UNDEFINED = 0,
    HAL_POWER_WAKEUP_TIMER,
    HAL_POWER_WAKEUP_EXT0,
    HAL_POWER_WAKEUP_GPIO,
    HAL_POWER_WAKEUP_UNKNOWN
} hal_power_wakeup_reason_t;

CeePewErr_t hal_power_init(void);
void hal_power_enter_deepsleep(uint32_t ms);
hal_power_wakeup_reason_t hal_power_get_wakeup_reason(void);

#ifdef __cplusplus
}
#endif

#endif /* CEEPEW_HAL_POWER_H */
