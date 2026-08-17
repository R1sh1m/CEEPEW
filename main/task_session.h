/* main/task_session.h
 *
 * Session Task for Core 1 — Cryptographic transport and RX processing.
 * Drains incoming radio frames from RX queue, posts UI events on message receipt.
 * Main crypto/transport processing entry point.
 *
 * Core 1 provides dedicated compute capacity for ESP-NOW recv callbacks and
 * encryption/decryption operations, while Core 0 handles UI rendering.
 */

#ifndef TASK_SESSION_H
#define TASK_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hal_rgb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Session event queue (posted by radio callbacks, drained by session task) */
extern QueueHandle_t g_session_rx_queue;

/* UI event queue (drained by UI task) — created and owned by session task */
extern QueueHandle_t g_ui_event_queue;

/*
 * Initialize session task state.
 * Called once during system startup (not by task itself).
 * Creates the session RX queue and UI event queue.
 */
void task_session_init(void);

/*
 * Main FreeRTOS task entry point for session task.
 * Runs on Core 1 with 8192-byte stack.
 * Never returns under normal operation (only on FreeRTOS task delete).
 *
 * Main loop:
 * - xQueueReceive from RX queue with 1000ms timeout
 * - Post UI_EVENT_MESSAGE_RX to g_ui_event_queue when frame arrives
 * - Handle queue timeout gracefully (no error, just continue)
 */
void task_session_run(void *pvParameters);

/* Refresh session lifecycle logging and RGB pattern selection. */
CeePewErr_t task_session_sync_visual_state(void);

/* Trigger a transient RGB blink overlay (e.g., on message TX/RX).
 * pattern: the blink pattern to display (e.g., RGB_GREEN_BLINK for TX, RGB_BLUE_BLINK for RX)
 * duration_ms: how long to show the blink before reverting to base state pattern */
void task_session_trigger_rgb_blink(RgbPattern_t pattern, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* TASK_SESSION_H */
