/* main/task_ui.c
 *
 * UI Task for Core 0 — OLED display and input/event orchestration.
 * Synchronous 30ms loop: render UI, update session visual state, handle events.
 *
 * Design note: The UI loop is deterministic and non-blocking. Each iteration
 * performs the same work in the same order: collect input, render the current
 * screen, synchronize the session-owned RGB pattern, then drain UI events.
 * No dynamic allocation; all state is static.
 */

#include "task_ui.h"
#include "task_session.h"
#include "session_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "esp_log.h"
#include <string.h>

/* HAL headers */
#include "hal_input.h"
#include "hal_oled.h"
#include "ui_manager.h"

/* External references */
extern QueueHandle_t g_ui_event_queue;

/* -------------------------------------------------------------------------- */
/* UI Task State                                                              */
/* -------------------------------------------------------------------------- */

/* Guard against duplicate startup calls. */
static bool s_ui_initialised = false;
static bool s_input_initialised = false;
static InputCtx_t s_input_ctx;

/* UI task handle (stored for later reference if needed) */
static TaskHandle_t s_ui_task_handle = NULL;

/* -------------------------------------------------------------------------- */
/* UI Event Handler                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Poll UI event queue with 5ms timeout.
 * Process event if received; silently ignore if queue is empty.
 */
static CeePewErr_t ui_handle_events(void)
{
    UIEvent_t event;

    if (g_ui_event_queue == NULL) {
        return CEEPEW_OK;  /* Queue not initialized yet */
    }

    /* Poll with 5ms timeout — do not block indefinitely */
    if (xQueueReceive(g_ui_event_queue, &event, pdMS_TO_TICKS(5U)) == pdTRUE) {
        /* Event received — dispatch based on type */
        switch (event.type) {
            case UI_EVENT_MESSAGE_RECEIVED:
                CEEPEW_LOG("UI", "Message received from device");
                break;

            case UI_EVENT_SESSION_STATE_CHANGED:
                CEEPEW_LOG("UI", "Session state changed to %u", 
                          event.payload.state_changed.new_state);
                break;

            case UI_EVENT_PAIRING_REQUEST:
                CEEPEW_LOG("UI", "Pairing request from peer");
                break;

            case UI_EVENT_SESSION_ESTABLISHED:
                CEEPEW_LOG("UI", "Session established");
                break;

            case UI_EVENT_BUTTON_PRESSED:
                CEEPEW_LOG("UI", "Button pressed: %u", 
                          event.payload.button_pressed.press_type);
                break;

            case UI_EVENT_POT_CHANGED:
                CEEPEW_LOG("UI", "Pot changed: %u", 
                          event.payload.pot_changed.raw_value);
                break;

            case UI_EVENT_DIAG_MODE_ENTER:
                CEEPEW_LOG("UI", "DIAG mode entered");
                break;

            case UI_EVENT_DIAG_MODE_EXIT:
                CEEPEW_LOG("UI", "DIAG mode exited");
                break;

            default:
                CEEPEW_ASSERT(false, CEEPEW_ERR_PARAM);  /* Unknown event type */
        }
    }

    return CEEPEW_OK;
}

/* -------------------------------------------------------------------------- */
/* Display Update                                                             */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* Task Entry Points                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Initialize UI task state (called once from app_main before task creation).
 */
void task_ui_init(void)
{
    if (s_ui_initialised) {
        return;
    }

    s_ui_task_handle = NULL;
    s_input_initialised = false;
    memset(&s_input_ctx, 0U, sizeof(s_input_ctx));

    CeePewErr_t err = input_init(&s_input_ctx);
    if (err != CEEPEW_OK) {
        CEEPEW_LOG("UI", "input_init failed: %d", (int)err);
    } else {
        s_input_initialised = true;
    }

    s_ui_initialised = true;

    CEEPEW_LOG("UI", "UI task state initialized");
}

/*
 * Main FreeRTOS task loop for Core 0.
 * Runs at 30ms tick interval indefinitely.
 * Only exits if FreeRTOS deletes the task.
 */
void task_ui_run(void *pvParameters)
{
    (void)pvParameters;  /* Unused parameter */

    s_ui_task_handle = xTaskGetCurrentTaskHandle();
    CEEPEW_ASSERT_VOID(s_ui_task_handle != NULL);

    CEEPEW_LOG("UI", "UI task started on Core %d", xPortGetCoreID());

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t loop_period = pdMS_TO_TICKS(CEEPEW_UI_LOOP_DELAY_MS);

    /* Main loop: runs every 30ms */
    for (;;) {
        if (s_input_initialised) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
            CeePewErr_t input_err = input_update(&s_input_ctx, now_ms);
            if (input_err == CEEPEW_OK) {
                uint8_t pot = 0U;
                bool button_pressed = false;
                bool diag_mode = false;
                input_err = input_get_normalized(&s_input_ctx, &pot, &button_pressed, &diag_mode);
                if (input_err == CEEPEW_OK) {
                    (void)ui_manager_handle_input(pot, button_pressed, diag_mode);
                } else {
                    CEEPEW_LOG("UI", "input_get_normalized failed: %d", (int)input_err);
                }
            } else {
                CEEPEW_LOG("UI", "input_update failed: %d", (int)input_err);
            }
        }

        /* Advance and draw the active UI state so the OLED actually shows content. */
        CeePewErr_t err = ui_manager_update();
        if (err != CEEPEW_OK) {
            CEEPEW_LOG("UI", "ui_manager_update failed: %d", (int)err);
        }

        err = ui_manager_draw();
        if (err != CEEPEW_OK) {
            CEEPEW_LOG("UI", "ui_manager_draw failed: %d", (int)err);
        }

        err = task_session_sync_visual_state();
        if (err != CEEPEW_OK) {
            CEEPEW_LOG("UI", "task_session_sync_visual_state failed: %d", (int)err);
        }

        /* Poll and handle UI events (non-blocking, 5ms timeout) */
        err = ui_handle_events();
        if (err != CEEPEW_OK) {
            CEEPEW_LOG("UI", "ui_handle_events failed: %d", (int)err);
        }

        /* Sleep until next tick (30ms period) */
        vTaskDelayUntil(&last_wake, loop_period);
    }

    /* Never reached under normal operation */
    vTaskDelete(NULL);
}
