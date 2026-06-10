/* main/session_memory.h
 *
 * Shared inter-task memory structures for UI ↔ Session communication.
 * Defines event types and structures posted from session task to UI task
 * via FreeRTOS queue (g_ui_event_queue).
 *
 * Event flow:
 *   Transport RX callbacks → Session task → UIEvent_t → UI task → Display update
 *   Input (ADC, GPIO) → Session task → UIEvent_t → UI task → Display update
 *
 * Memory layout:
 *   UIEvent_t is exactly 28 bytes (fits in cache line, efficient for queue).
 *   All unions and payloads remain within 28 bytes total size.
 */

#ifndef SESSION_MEMORY_H
#define SESSION_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "../components/ceepew_hal/ui_manager.h" /* for UIState_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* UI Event Type Enumeration                                                  */
/* ========================================================================== */

/**
 * @enum UIEventType
 *
 * Event types posted from session task to UI task via g_ui_event_queue.
 * Each event signals a state change, user input, or incoming data that
 * requires UI update (display render, LED pattern change, etc).
 *
 * Security notes:
 * - MESSAGE_RECEIVED is only posted after full crypto validation
 * - SESSION_* events are posted after state machine verification
 * - No sensitive data is carried in events (device ID, msg_id only)
 *
 * Usage:
 *   UIEvent_t evt = {
 *       .type = UI_EVENT_MESSAGE_RECEIVED,
 *       .param = sender_mac_hash,  // device ID for lookup
 *       .payload.message_rx = { .device_id = {...}, .msg_id = 42 }
 *   };
 *   xQueueSend(g_ui_event_queue, &evt, portMAX_DELAY);
 */
typedef enum {
    /** Message received and decrypted successfully.
     *  Payload: message_rx { device_id[6], msg_id }
     *  Param: sender device identifier (hash or index)
     */
    UI_EVENT_MESSAGE_RECEIVED = 0,

    /** Session state changed (idle, discovery, pairing, active, closing).
     *  Payload: state_changed { new_state }
     *  Param: old state value (for state machine verification)
     */
    UI_EVENT_SESSION_STATE_CHANGED,

    /** Incoming pairing request from peer device.
     *  Payload: pairing_request { device_id[6] }
     *  Param: peer device index or identifier
     */
    UI_EVENT_PAIRING_REQUEST,

    /** Pairing completed, session now fully established.
     *  Payload: session_established { reserved[2] }
     *  Param: session_id high 32 bits
     */
    UI_EVENT_SESSION_ESTABLISHED,

    /** Button pressed (click or hold detected).
     *  Payload: button_pressed { press_type }
     *  Param: button identifier (0=main, 1=reserved)
     */
    UI_EVENT_BUTTON_PRESSED,

    /** Potentiometer value changed (filtered).
     *  Payload: pot_changed { raw_value[2] }
     *  Param: pot ADC normalized to 0-1000 range
     */
    UI_EVENT_POT_CHANGED,

    /** Diagnostic mode entered (DIAG button held).
     *  Payload: (unused)
     *  Param: DIAG startup flags
     */
    UI_EVENT_DIAG_MODE_ENTER,

    /** Diagnostic mode exited (timeout or exit button pressed).
     *  Payload: (unused)
     *  Param: DIAG exit reason code
     */
    UI_EVENT_DIAG_MODE_EXIT,

    /* Reserved for future extensions (pairing phases, encryption progress, etc) */
} UIEventType;

/* ========================================================================== */
/* UI Event Payload Union                                                     */
/* ========================================================================== */

/**
 * @union UIEventPayload
 *
 * Type-specific event data. Each payload occupies exactly 16 bytes
 * to maintain fixed UIEvent_t size (28 bytes).
 *
 * Union design ensures:
 * - No wasted space (all variants same size)
 * - Type-safe access via correct member based on event.type
 * - Caller responsibility to access correct union member
 *
 * Example (MESSAGE_RECEIVED):
 *   if (evt.type == UI_EVENT_MESSAGE_RECEIVED) {
 *       process_message(evt.payload.message_rx.device_id,
 *                       evt.payload.message_rx.msg_id);
 *   }
 */
typedef union __attribute__((packed)) {
    /** UI_EVENT_MESSAGE_RECEIVED payload */
    struct __attribute__((packed)) {
        uint8_t  device_id[6];  /**< Sender MAC address (6 bytes) */
        uint16_t msg_id;        /**< Message ID in store (16-bit) */
        uint8_t  reserved[4];   /**< Padding to 16 bytes */
    } message_rx;

    /** UI_EVENT_SESSION_STATE_CHANGED payload */
    struct __attribute__((packed)) {
        uint8_t  new_state;     /**< New session state enum value */
        uint8_t  old_state;     /**< Previous state (for verification) */
        uint8_t  transition_reason;  /**< Reason for state change */
        uint8_t  reserved[13];  /**< Padding to 16 bytes */
    } state_changed;

    /** UI_EVENT_PAIRING_REQUEST payload */
    struct __attribute__((packed)) {
        uint8_t  device_id[6];  /**< Peer MAC address */
        uint8_t  reserved[10];  /**< Padding to 16 bytes */
    } pairing_request;

    /** UI_EVENT_SESSION_ESTABLISHED payload */
    struct __attribute__((packed)) {
        uint32_t session_id_hi; /**< Session ID high 32 bits */
        uint8_t  reserved[12];  /**< Padding to 16 bytes */
    } session_established;

    /** UI_EVENT_BUTTON_PRESSED payload */
    struct __attribute__((packed)) {
        uint8_t  press_type;    /**< 0=click, 1=long_hold, etc */
        uint8_t  reserved[15];  /**< Padding to 16 bytes */
    } button_pressed;

    /** UI_EVENT_POT_CHANGED payload */
    struct __attribute__((packed)) {
        uint16_t raw_value;     /**< Raw ADC value (0-4095) */
        uint8_t  reserved[14];  /**< Padding to 16 bytes */
    } pot_changed;

    /** UI_EVENT_DIAG_MODE_ENTER/EXIT payload */
    struct __attribute__((packed)) {
        uint8_t  flags;         /**< DIAG initialization flags */
        uint8_t  reserved[15];  /**< Padding to 16 bytes */
    } diag;

    /** Raw access for debugging */
    uint8_t raw[16];

} UIEventPayload;

/* ========================================================================== */
/* Complete UI Event Structure                                                */
/* ========================================================================== */

/**
 * @struct UIEvent_t
 *
 * Complete event message posted to g_ui_event_queue.
 * Fixed 28 bytes: efficient for queue transport and stack allocation.
 * Layout:
 *   [0]     UIEventType type (1 byte enum)
 *   [1-3]   uint32_t param (3 unused bytes + 4-byte value)
 *   [4-19]  UIEventPayload union (16 bytes)
 *   [20-27] reserved (8 bytes for future expansion)
 *
 * Total: 28 bytes (32-byte cache line aligned when heap allocated)
 *
 * Security: No sensitive data (keys, plaintext) ever stored in events.
 * Only references (device IDs, message IDs) that allow UI to look up
 * full data from session state only after validation.
 *
 * Usage:
 *   UIEvent_t evt = {0};
 *   evt.type = UI_EVENT_MESSAGE_RECEIVED;
 *   evt.param = sender_device_index;
 *   memcpy(evt.payload.message_rx.device_id, peer_mac, 6);
 *   evt.payload.message_rx.msg_id = 42;
 *   xQueueSend(g_ui_event_queue, &evt, portMAX_DELAY);
 */
typedef struct __attribute__((packed)) {
    UIEventType     type;           /**< Event type discriminant (1 byte) */
    uint32_t        param;          /**< Generic parameter (device ID, mode, etc) */
    UIEventPayload  payload;        /**< Type-specific payload (16 bytes) */
    uint32_t        reserved;       /**< For future use (4 bytes padding) */
} UIEvent_t;

/* Verify struct size */
_Static_assert(sizeof(UIEvent_t) == 28, "UIEvent_t must be exactly 28 bytes");
_Static_assert(sizeof(UIEventPayload) == 16, "UIEventPayload must be exactly 16 bytes");

/* ========================================================================== */
/* UI Event Queue Configuration                                               */
/* ========================================================================== */

/**
 * @def CEEPEW_UI_EVENT_QUEUE_DEPTH
 *
 * Maximum number of UIEvent_t items that can be queued.
 * Queue is created and owned by session task (task_session_init).
 * UI task (task_ui_run) drains events at 30ms intervals.
 *
 * Sizing rationale:
 * - Button clicks: 1-2 events per ~200ms (5 per second max)
 * - Pot changes: 1-2 events per ~50ms render tick (worst: 20/sec)
 * - Messages: 1 per received frame (bursty, typical 1-3/sec)
 * - State changes: 1-2 per session lifecycle
 * Depth=16 allows ~3 seconds of worst-case button spam before overflow
 *
 * Overflow strategy:
 * - If queue is full, session task MUST drop the oldest event (xQueueOverwrite)
 * - UI task never blocks on queue receive (timeout: 30ms)
 * - This ensures UI responsiveness even under event flood
 */
#define CEEPEW_UI_EVENT_QUEUE_DEPTH 16U

/* ========================================================================== */
/* Event Queue Handle (Created by task_session_init)                          */
/* ========================================================================== */

/**
 * @var g_ui_event_queue
 *
 * FreeRTOS queue for inter-task communication (session → UI).
 * Type: QueueHandle_t holding UIEvent_t items.
 * Capacity: CEEPEW_UI_EVENT_QUEUE_DEPTH items.
 * Created: task_session_init() (runs during system init, not from task).
 * Destroyed: Never (persistent for session lifetime).
 *
 * Usage from session task:
 *   UIEvent_t evt = {.type = UI_EVENT_MESSAGE_RECEIVED, ...};
 *   xQueueSend(g_ui_event_queue, &evt, portMAX_DELAY);
 *
 * Usage from UI task:
 *   UIEvent_t evt;
 *   if (xQueueReceive(g_ui_event_queue, &evt, pdMS_TO_TICKS(30))) {
 *       process_ui_event(&evt);
 *   }
 */
extern QueueHandle_t g_ui_event_queue;

/* ========================================================================== */
/* UI Context Lock (Phase 2.C1)                                                */
/* ========================================================================== */
/* g_ui_ctx is written by the UI task (Core 0) and read by the session task
 * (Core 1). To prevent torn reads of multi-byte fields and inconsistent
 * multi-field reads, all session-task accesses go through the snapshot
 * helpers below, which take this mutex around the field copy. */
extern SemaphoreHandle_t g_ui_ctx_lock;

/* Take / release the UI context mutex. session_ui_ctx_lock() blocks if
 * held; session_ui_ctx_unlock() is non-blocking. Safe to nest (recursive). */
void session_ui_ctx_lock(void);
void session_ui_ctx_unlock(void);

/* Snapshot the fields the session task reads most often. Use these in
 * hot paths instead of direct g_ui_ctx.X reads — a single take/copy/release
 * is cheaper than 7 take/release pairs in a function body. */
void session_ui_get_state_snapshot(UIState_t *out_state);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_MEMORY_H */
