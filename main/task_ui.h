/* main/task_ui.h
 *
 * UI Task for Core 0 — OLED display and RGB LED control.
 * Runs at 30ms tick interval, handles UI events from g_ui_event_queue.
 * Maintains RGB heartbeat pattern and clears display each tick.
 */

#ifndef TASK_UI_H
#define TASK_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_config.h"
#include "ceepew_assert.h"

/* UI events are defined in session_memory.h (UIEvent_t, UIEventType) */

/*
 * Initialize UI task state.
 * Called once during system startup (not by task itself).
 */
void task_ui_init(void);

/*
 * Main FreeRTOS task entry point for UI task.
 * Runs on Core 0 with 4096-byte stack.
 * Never returns under normal operation (only on FreeRTOS task delete).
 */
void task_ui_run(void *pvParameters);

#endif /* TASK_UI_H */
