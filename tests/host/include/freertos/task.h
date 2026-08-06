/* Stub freertos/task.h for host compilation. */

#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

void vTaskDelay(TickType_t xTicksToDelay);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_TASK_H */
