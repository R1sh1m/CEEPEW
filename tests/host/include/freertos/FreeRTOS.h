/* Stub freertos/FreeRTOS.h for host compilation.
 * Provides the minimal FreeRTOS types referenced by the CEE-PEW portable
 * sources (hal_radio.h QueueHandle_t, ecc_arq.c TickType_t/pdMS_TO_TICKS).
 */

#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;

/* Opaque queue handle type. The real FreeRTOS defines this in queue.h but
 * hal_radio.h only needs the name, so it is declared here. */
typedef void * QueueHandle_t;

#define portMAX_DELAY ((TickType_t) 0xFFFFFFFFUL)
#define portTICK_PERIOD_MS ((TickType_t) 1)

#define pdFALSE ((BaseType_t) 0)
#define pdTRUE  ((BaseType_t) 1)
#define pdPASS  ((BaseType_t) 1)
#define pdFAIL  ((BaseType_t) 0)

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_H */
