#include "task_arch.h"
#include "ui_manager.h"
#include "hal_input.h"
#include "../../main/session_fsm.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "task_arch";

static QueueHandle_t s_session_queue = NULL;
static esp_timer_handle_t s_session_timer = NULL;
static TaskHandle_t s_ui_task_handle = NULL;
static TaskHandle_t s_session_task_handle = NULL;
static bool s_task_arch_initialised = false;

static void session_timer_cb(void *arg);

/* Forward declarations for task functions (defined in main) */
extern void task_ui_run(void *pvParameters);
extern void task_session_run(void *pvParameters);

QueueHandle_t task_arch_get_session_queue(void){
    return s_session_queue;
}

CeePewErr_t task_arch_init(void){
    if (s_task_arch_initialised) {
        CEEPEW_ASSERT(s_session_queue != NULL && s_session_timer != NULL, CEEPEW_ERR_INTERNAL);
        return CEEPEW_OK;
    }

    /* Create session queue */
    s_session_queue = xQueueCreate(CEEPEW_QUEUE_DEPTH, sizeof(void *));
    if (s_session_queue == NULL) { return CEEPEW_ERR_ALLOC; }

    /* Create periodic esp_timer for session housekeeping (1s) */
    const esp_timer_create_args_t timer_args = {
        .callback = &session_timer_cb,
        .arg = NULL,
        .name = "session_timer"
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_session_timer);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }
    CEEPEW_ASSERT(s_session_timer != NULL, CEEPEW_ERR_INTERNAL);

    /* Tasks are now created directly by app_main via xTaskCreatePinnedToCore() */

    /* Start periodic timer at 1 second */
    err = esp_timer_start_periodic(s_session_timer, 1000000ULL);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    s_task_arch_initialised = true;
    ESP_LOGI(TAG, "task_arch initialized");
    return CEEPEW_OK;
}

CeePewErr_t task_arch_deinit(void){
    if (!s_task_arch_initialised) {
        return CEEPEW_OK;
    }

    CEEPEW_ASSERT(s_session_queue != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_session_timer != NULL, CEEPEW_ERR_NULL_PTR);

    /* Stop timer */
    esp_timer_stop(s_session_timer);
    esp_timer_delete(s_session_timer);
    s_session_timer = NULL;

    /* Delete tasks if they exist */
    if (s_ui_task_handle != NULL) { vTaskDelete(s_ui_task_handle); s_ui_task_handle = NULL; }
    if (s_session_task_handle != NULL) { vTaskDelete(s_session_task_handle); s_session_task_handle = NULL; }

    /* Delete queue */
    vQueueDelete(s_session_queue);
    s_session_queue = NULL;
    s_task_arch_initialised = false;

    ESP_LOGI(TAG, "task_arch deinitialized");
    return CEEPEW_OK;
}

static void session_timer_cb(void *arg){
    (void)arg;
    /* Periodic housekeeping: advance replay windows, retry ARQ, etc. */
    /* uint8_t phase = session_get_phase(); */
    /* (void)phase; */
}
