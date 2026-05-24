#ifndef TASK_ARCH_H
#define TASK_ARCH_H

#include <stdint.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize task architecture: create tasks, queues, and timers. */
CeePewErr_t task_arch_init(void);

/* Deinitialize task architecture: stop timers, delete tasks and queues. */
CeePewErr_t task_arch_deinit(void);

/* Get handle to session inbound queue for transport layer to post events */
QueueHandle_t task_arch_get_session_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* TASK_ARCH_H */
