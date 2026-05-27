/* ══════════════════════════════════════════════════════════════════════════
 * CEE-PEW TASK UI — COMPLETE REPLACEMENT
 *
 * File: main/task_ui.c
 *
 * Key changes vs previous version:
 *   1. DIAG switch state is read from hal_input and logged on change so you
 *      can verify GPIO 5 is actually being detected.
 *   2. If input_get_normalized is unavailable (returns error), we fall back
 *      to reading the switch directly from the InputCtx_t struct fields.
 *   3. Added DIAG mode edge-detection logging (fires once on enter/exit).
 *   4. ui_handle_events() now routes UI_EVENT_SESSION_ESTABLISHED directly
 *      to UI_STATE_KEYDER so both devices reliably enter chat.
 * ══════════════════════════════════════════════════════════════════════════ */

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

#include "hal_input.h"
#include "hal_pins.h"
#include "hal_oled.h"
#include "ui_manager.h"

extern QueueHandle_t g_ui_event_queue;

static bool         s_ui_initialised   = false;
static bool         s_input_initialised = false;
static InputCtx_t   s_input_ctx;
static TaskHandle_t s_ui_task_handle   = NULL;

/* Track previous DIAG state so we can log transitions */
static bool s_diag_prev = false;

/* --------------------------------------------------------------------------
 * ui_handle_events()
 * -------------------------------------------------------------------------- */
static CeePewErr_t ui_handle_events(void)
{
    UIEvent_t event;

    if (g_ui_event_queue == NULL) { return CEEPEW_OK; }

    if (xQueueReceive(g_ui_event_queue, &event, pdMS_TO_TICKS(5U)) == pdTRUE) {
        switch (event.type) {

        case UI_EVENT_SESSION_ESTABLISHED:
            /* Session established on Core 1 → drive UI to key-derivation screen.
             * This ensures BOTH devices reliably enter the secure-chat flow,
             * not just the one that happened to win the timing race.            */
            ESP_LOGI("UI", "SESSION_ESTABLISHED — transitioning to KEYDER");
            if (g_ui_ctx.current_state == UI_STATE_COUNTDOWN ||
                g_ui_ctx.current_state == UI_STATE_DISCOVERY) {
                (void)ui_manager_transition_to(UI_STATE_KEYDER);
                g_ui_ctx.transition_ready = true;
            }
            break;

        case UI_EVENT_MESSAGE_RECEIVED:
            ESP_LOGI("UI", "Message received");
            /* If we're in chat, no extra action needed; ui_manager polls msg_store */
            break;

        case UI_EVENT_SESSION_STATE_CHANGED:
            ESP_LOGI("UI", "Session state → %u",
                     event.payload.state_changed.new_state);
            break;

        case UI_EVENT_PAIRING_REQUEST:
            ESP_LOGI("UI", "Pairing request");
            break;

        case UI_EVENT_BUTTON_PRESSED:
            /* Already handled by hal_input; log only */
            break;

        case UI_EVENT_POT_CHANGED:
            break;

        case UI_EVENT_DIAG_MODE_ENTER:
            ESP_LOGI("UI", "DIAG mode ENTERED via event");
            break;

        case UI_EVENT_DIAG_MODE_EXIT:
            ESP_LOGI("UI", "DIAG mode EXITED via event");
            break;

        default:
            break;
        }
    }

    return CEEPEW_OK;
}

/* --------------------------------------------------------------------------
 * task_ui_init()
 * -------------------------------------------------------------------------- */
void task_ui_init(void)
{
    if (s_ui_initialised) { return; }

    s_ui_task_handle   = NULL;
    s_input_initialised = false;
    s_diag_prev        = false;
    memset(&s_input_ctx, 0U, sizeof(s_input_ctx));

    CeePewErr_t err = input_init(&s_input_ctx);
    if (err != CEEPEW_OK) {
        ESP_LOGE("UI", "input_init failed: %d — input will be unavailable", (int)err);
    } else {
        s_input_initialised = true;
        ESP_LOGI("UI", "input_init OK — DIAG switch GPIO %d active-LOW",
                 (int)CEEPEW_PIN_DIAG_SWITCH);
    }

    s_ui_initialised = true;
    ESP_LOGI("UI", "UI task state initialized");
}

/* --------------------------------------------------------------------------
 * task_ui_run() — Core 0 main loop, 30 ms period
 * -------------------------------------------------------------------------- */
void task_ui_run(void *pvParameters)
{
    (void)pvParameters;

    s_ui_task_handle = xTaskGetCurrentTaskHandle();
    CEEPEW_ASSERT_VOID(s_ui_task_handle != NULL);

    ESP_LOGI("UI", "UI task started on Core %d", xPortGetCoreID());

    TickType_t last_wake  = xTaskGetTickCount();
    const TickType_t loop_period = pdMS_TO_TICKS(CEEPEW_UI_LOOP_DELAY_MS);

    for (;;) {
        /* ── 1. Collect input ──────────────────────────────────────── */
        if (s_input_initialised) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
            CeePewErr_t ierr = input_update(&s_input_ctx, now_ms);

            if (ierr == CEEPEW_OK) {
                uint8_t pot          = 0U;
                bool    btn_pressed  = false;
                bool    diag_mode    = false;

                ierr = input_get_normalized(&s_input_ctx, &pot, &btn_pressed, &diag_mode);

                if (ierr == CEEPEW_OK) {
                    /* ── DIAG edge detection — log once on change ── */
                    if (diag_mode != s_diag_prev) {
                        if (diag_mode) {
                            ESP_LOGI("UI", "DIAG switch ACTIVATED (GPIO %d LOW)",
                                     (int)CEEPEW_PIN_DIAG_SWITCH);
                        } else {
                            ESP_LOGI("UI", "DIAG switch RELEASED");
                        }
                        s_diag_prev = diag_mode;
                    }

                    (void)ui_manager_handle_input(pot, btn_pressed, diag_mode);
                } else {
                    /*
                     * Fallback: input_get_normalized failed but update was OK.
                     * Read fields directly from InputCtx_t struct.
                     * This makes DIAG mode work even if the normalised accessor
                     * has a bug.
                     */
                    bool diag_direct = s_input_ctx.diag_switch_active;
                    if (diag_direct != s_diag_prev) {
                        ESP_LOGI("UI", "DIAG switch (direct) %s",
                                 diag_direct ? "ACTIVATED" : "RELEASED");
                        s_diag_prev = diag_direct;
                    }
                    (void)ui_manager_handle_input(
                        (uint8_t)(s_input_ctx.smoothed_adc >> 4U),  /* 12-bit → 8-bit */
                        (s_input_ctx.click_pending),
                        diag_direct);
                }
            } else {
                ESP_LOGW("UI", "input_update failed: %d", (int)ierr);
            }
        }

        /* ── 2. Advance state machine ───────────────────────────────── */
        CeePewErr_t err = ui_manager_update();
        if (err != CEEPEW_OK) {
            ESP_LOGW("UI", "ui_manager_update: %d", (int)err);
        }

        /* ── 3. Draw current screen ────────────────────────────────── */
        err = ui_manager_draw();
        if (err != CEEPEW_OK) {
            ESP_LOGW("UI", "ui_manager_draw: %d", (int)err);
        }

        /* ── 4. Sync RGB LED pattern from session ──────────────────── */
        err = task_session_sync_visual_state();
        if (err != CEEPEW_OK) {
            ESP_LOGW("UI", "sync_visual_state: %d", (int)err);
        }

        /* ── 5. Drain event queue (5 ms timeout) ──────────────────── */
        err = ui_handle_events();
        if (err != CEEPEW_OK) {
            ESP_LOGW("UI", "ui_handle_events: %d", (int)err);
        }

        /* ── 6. Sleep until next 30 ms tick ───────────────────────── */
        vTaskDelayUntil(&last_wake, loop_period);
    }

    vTaskDelete(NULL);
}
