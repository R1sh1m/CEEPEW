/* main/task_session.c
 *
 * Session Task for Core 1 — Cryptographic transport and RX processing.
 * Drains incoming radio frames from RX queue, posts UI events on message receipt.
 * Phase 4: Adds TTL expiry checking, nonce exhaustion detection, and session lifecycle logging.
 *
 * Design note: The session task provides the main crypto/transport processing
 * loop on Core 1. It receives encrypted frames via g_session_rx_queue (populated
 * by radio callbacks on Core 0), decrypts/verifies them, and notifies the UI
 * by posting events to g_ui_event_queue. This separation allows Core 0 to focus
 * on UI rendering (non-blocking, deterministic 30ms ticks) while Core 1 handles
 * variable-latency cryptographic operations. Phase 4 adds periodic session
 * maintenance checks (TTL, nonce exhaustion) that trigger secure session wipe.
 */

#include "task_session.h"
#include "task_ui.h"
#include "session_fsm.h"
#include "session_msgstore.h"
#include "session_memory.h"
#include "ceepew_security_utils.h"
#include "crypto_ctx.h"
#include "crypto_box_wrap.h"
#include "compress_huffman.h"
#include "ecc_hamming.h"
#include "ecc_crc32.h"
#include "hal_radio.h"
#include "hal_rgb.h"
#include "transport_esl.h"
#include "ui_manager.h"
#include "ceepew_config.h"  /* Phase 4: For constants and logging */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "task_session";

CeePewErr_t crypto_ascon_aead_decrypt(const uint8_t key[16],
                                      const uint8_t nonce[16],
                                      const uint8_t *ad,
                                      uint16_t ad_len,
                                      const uint8_t *ct,
                                      uint16_t ct_len,
                                      uint8_t *pt,
                                      uint16_t *pt_len);
CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32],
                                const uint8_t *msg,
                                uint16_t msg_len,
                                const uint8_t sig[64]);

/* -------------------------------------------------------------------------- */
/* Queue Instances (global, created by task_session_init)                    */
/* -------------------------------------------------------------------------- */

/* Session RX queue: radio callbacks post frames here */
QueueHandle_t g_session_rx_queue = NULL;

/* UI event queue: session task posts UI events here */
QueueHandle_t g_ui_event_queue = NULL;

/* -------------------------------------------------------------------------- */
/* Session Task State                                                         */
/* -------------------------------------------------------------------------- */

/* Session task handle (stored for later reference if needed) */
static TaskHandle_t s_session_task_handle = NULL;

/* Guard against duplicate startup calls. */
static bool s_session_initialised = false;

/* Session processing statistics (for diagnostics) */
typedef struct {
    uint32_t rx_frames_received;
    uint32_t rx_frames_processed;
    uint32_t queue_timeouts;
} SessionStats_t;

static volatile SessionStats_t s_stats = {0U, 0U, 0U};
static bool s_visual_state_ready = false;
static UIState_t s_last_ui_state = UI_STATE_BOOT;
static uint8_t s_last_session_phase = 0U;
static bool s_last_session_active = false;
static RgbPattern_t s_last_rgb_pattern = RGB_OFF;

/* -------------------------------------------------------------------------- */
/* RX Frame Structure                                                         */
/* -------------------------------------------------------------------------- */

static const char *task_session_phase_name(uint8_t phase)
{
    switch (phase) {
        case 0U: return "idle";
        case 1U: return "discovery";
        case 2U: return "pairing";
        case 3U: return "active";
        default: return "unknown";
    }
}

static const char *task_session_ui_name(UIState_t state)
{
    switch (state) {
        case UI_STATE_BOOT: return "boot";
        case UI_STATE_DISCOVERY: return "discovery";
        case UI_STATE_CODE_ENTRY: return "code_entry";
        case UI_STATE_COUNTDOWN: return "countdown";
        case UI_STATE_CONFIRM: return "confirm";
        case UI_STATE_KEYDER: return "keyder";
        case UI_STATE_FINGERPRINT: return "fingerprint";
        case UI_STATE_FINGERPRINT_CONFIRM: return "fingerprint_confirm";
        case UI_STATE_CHAT: return "chat";
        case UI_STATE_CRYPTOGRAM: return "cryptogram";
        case UI_STATE_NONCE_EXHAUSTED: return "nonce_exhausted";
        case UI_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

static const char *task_session_rgb_name(RgbPattern_t pattern)
{
    switch (pattern) {
        case RGB_OFF: return "off";
        case RGB_RED: return "red";
        case RGB_GREEN: return "green";
        case RGB_BLUE: return "blue";
        case RGB_YELLOW: return "yellow";
        case RGB_CYAN: return "cyan";
        case RGB_MAGENTA: return "magenta";
        case RGB_WHITE: return "white";
        case RGB_RED_BLINK: return "red_blink";
        case RGB_GREEN_BLINK: return "green_blink";
        case RGB_BLUE_BLINK: return "blue_blink";
        case RGB_WHITE_PULSE: return "white_pulse";
        case RGB_BLUE_PULSE: return "blue_pulse";
        case RGB_GREEN_PULSE: return "green_pulse";
        case RGB_AMBER_PULSE: return "amber_pulse";
        case RGB_CYAN_PULSE: return "cyan_pulse";
        case RGB_RAINBOW_CYCLE: return "rainbow";
        case RGB_HEARTBEAT: return "heartbeat";
        default: return "unknown";
    }
}

static RgbPattern_t task_session_map_pattern(UIState_t state, bool session_active)
{
    switch (state) {
        case UI_STATE_BOOT:
            return RGB_WHITE_PULSE;  /* One-shot white breathe during boot */
        case UI_STATE_DISCOVERY:
            return RGB_BLUE_PULSE;   /* Smooth blue pulse during scanning */
        case UI_STATE_CODE_ENTRY:
        case UI_STATE_COUNTDOWN:
        case UI_STATE_CONFIRM:
            return RGB_AMBER_PULSE;  /* Smooth amber pulse for user input */
        case UI_STATE_KEYDER:
            return RGB_YELLOW;       /* Solid yellow during key derivation */
        case UI_STATE_FINGERPRINT:
        case UI_STATE_FINGERPRINT_CONFIRM:
            return RGB_CYAN_PULSE;   /* Smooth cyan pulse for verification */
        case UI_STATE_CHAT:
            return session_active ? RGB_GREEN : RGB_OFF;
        case UI_STATE_NONCE_EXHAUSTED:
        case UI_STATE_ERROR:
            return RGB_RED_BLINK;    /* Red blink for errors */
        default:
            return RGB_OFF;
    }
}

CeePewErr_t task_session_sync_visual_state(void)
{
    uint8_t phase = session_get_phase();
    bool active = session_is_active();
    UIState_t ui_state = g_ui_ctx.current_state;
    RgbPattern_t pattern = task_session_map_pattern(ui_state, active);
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);

    bool changed = (!s_visual_state_ready ||
                    phase != s_last_session_phase ||
                    active != s_last_session_active ||
                    ui_state != s_last_ui_state ||
                    pattern != s_last_rgb_pattern);

    if (changed) {
        ESP_LOGI(TAG, "[%llu ms] phase=%s ui=%s active=%u rgb=%s",
                 (unsigned long long)now_ms,
                 task_session_phase_name(phase),
                 task_session_ui_name(ui_state),
                 active ? 1U : 0U,
                 task_session_rgb_name(pattern));
    }

    if (!s_visual_state_ready || pattern != s_last_rgb_pattern) {
        CeePewErr_t err = rgb_set_pattern(pattern);
        if (err != CEEPEW_OK) {
            return err;
        }
    }

    s_visual_state_ready = true;
    s_last_session_phase = phase;
    s_last_session_active = active;
    s_last_ui_state = ui_state;
    s_last_rgb_pattern = pattern;
    return CEEPEW_OK;
}

/* -------------------------------------------------------------------------- */
/* RX Processing                                                              */
/* -------------------------------------------------------------------------- */

/*
 * Process a received frame from the radio.
 * 
 * In the initial implementation, this simply posts a UI event.
 * Later sprints will add:
 * - transport_esl_process_incoming() for CRC/FEC/decryption
 * - session_mac_lock_check() for peer verification
 * - Crypto verification (Ascon-128, Ed25519)
 * 
 * All failures result in silent discard (no NACK, no log).
 *
 * PARAMETERS:
 *   frame: Received frame (not NULL, len > 0)
 *
 * RETURNS:
 *   CEEPEW_OK — Frame accepted and UI event posted
 *   CEEPEW_ERR_NULL_PTR — frame is NULL
 *   CEEPEW_ERR_BOUNDS — Frame length is zero
 */
static CeePewErr_t process_rx_frame(const RadioFrame_t *frame)
{
    CEEPEW_ASSERT(frame != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(frame->payload_len > 0U && frame->payload_len <= CEEPEW_PACKET_MAX_BYTES,
                  CEEPEW_ERR_BOUNDS);
    if (!session_is_active()) {
        return CEEPEW_OK;
    }

    static uint8_t s_work_frame[CEEPEW_PACKET_MAX_BYTES];
    static uint8_t s_fec_out[CEEPEW_FEC_BUF_MAX];
    static uint8_t s_box_ct[CEEPEW_MAX_MSG_BYTES + 64U];
    static uint8_t s_ascon_ct[CEEPEW_MAX_MSG_BYTES + CEEPEW_ASCON_TAG_BYTES];
    static uint8_t s_plain[CEEPEW_MAX_MSG_BYTES];
    static uint8_t s_nonce[24U];
    static uint8_t s_peer_pk[32U];
    static uint8_t s_ascon_key[16U];
    static uint8_t s_sig[64U];

    CeePewErr_t err = CEEPEW_OK;
    uint16_t work_len = frame->payload_len;
    memcpy(s_work_frame, frame->payload, frame->payload_len);

    err = transport_esl_process_incoming(s_work_frame, &work_len, frame->src_mac, 0U);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    bool corrected = false;
    uint16_t fec_out_len = (uint16_t)sizeof(s_fec_out);
    err = ecc_hamming_decode(s_work_frame, work_len, s_fec_out, &fec_out_len, &corrected);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    if (fec_out_len < 64U) { return CEEPEW_OK; }
    uint16_t box_ct_len = (uint16_t)(fec_out_len - 64U);
    uint16_t sig_off = box_ct_len;
    memcpy(s_box_ct, s_fec_out, box_ct_len);
    memcpy(s_sig, s_fec_out + sig_off, 64U);

    uint64_t rx_nonce_counter = 0ULL;
    err = transport_esl_get_last_nonce_counter(&rx_nonce_counter);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }
    err = session_get_nonce(s_nonce);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }
    for (uint8_t i = 0U; i < 8U; i++) {
        s_nonce[8U + i] = (uint8_t)((rx_nonce_counter >> (i * 8U)) & 0xFFU);
    }
    err = session_get_peer_public_key(s_peer_pk);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }
    err = crypto_box_decrypt(&g_crypto_ctx, s_nonce, s_peer_pk,
                             s_box_ct, box_ct_len, s_ascon_ct, &box_ct_len);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    uint8_t ascon_nonce[16U];
    memcpy(ascon_nonce, s_nonce, sizeof(ascon_nonce));
    err = session_get_session_key(s_ascon_key);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    uint16_t plain_len = (uint16_t)sizeof(s_plain);
    err = crypto_ascon_aead_decrypt(s_ascon_key, ascon_nonce, NULL, 0U,
                                    s_ascon_ct, box_ct_len, s_plain, &plain_len);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    err = crypto_eddsa_verify(s_peer_pk, s_box_ct, box_ct_len, s_sig);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    uint8_t local_device_id[CEEPEW_DEVICE_ID_BYTES];
    uint8_t fingerprint[CEEPEW_FINGERPRINT_BYTES];
    err = session_get_device_id(local_device_id);
    if (err == CEEPEW_OK) {
        err = session_compute_fingerprint(s_peer_pk, local_device_id, fingerprint);
        if (err == CEEPEW_OK) {
            memcpy(g_ui_ctx.fingerprint, fingerprint, CEEPEW_FINGERPRINT_BYTES);
            memcpy(g_ui_ctx.peer_mac, frame->src_mac, CEEPEW_DEVICE_ID_BYTES);
            g_ui_ctx.fingerprint_confirmed = false;
            if (g_ui_ctx.current_state != UI_STATE_CHAT &&
                g_ui_ctx.current_state != UI_STATE_FINGERPRINT_CONFIRM) {
                (void)ui_manager_transition_to(UI_STATE_FINGERPRINT);
                g_ui_ctx.transition_ready = true;
            }
        }
    }
    ceepew_secure_zero(local_device_id, sizeof(local_device_id));
    ceepew_secure_zero(fingerprint, sizeof(fingerprint));

    uint16_t decoded_len = (uint16_t)sizeof(s_work_frame);
    err = compress_huffman_decompress(s_plain, plain_len, s_work_frame, &decoded_len,
                                      (uint16_t)sizeof(s_work_frame));
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    err = msg_store_add(s_box_ct, box_ct_len, decoded_len, 0U);
    if (err != CEEPEW_OK) { return CEEPEW_OK; }

    UIEvent_t ui_event;
    memset(&ui_event, 0U, sizeof(ui_event));
    ui_event.type = UI_EVENT_MESSAGE_RECEIVED;
    ui_event.param = (uint32_t)msg_store_count();
    memcpy(ui_event.payload.message_rx.device_id, frame->src_mac,
           CEEPEW_DEVICE_ID_BYTES);
    ui_event.payload.message_rx.msg_id = (uint16_t)(msg_store_count() - 1U);

    BaseType_t result = xQueueSendToBack(g_ui_event_queue, &ui_event, 0U);
    (void)result;

    (void)session_update_last_message_time();

    s_stats.rx_frames_processed++;
    return CEEPEW_OK;
}

/* -------------------------------------------------------------------------- */
/* Task Entry Points                                                          */
/* -------------------------------------------------------------------------- */

/*
 * Initialize session task state and queues.
 * Called once from app_main before task creation.
 *
 * Creates:
 * - g_session_rx_queue: depth = CEEPEW_QUEUE_DEPTH, element size = RxFrame_t*
 * - g_ui_event_queue: depth = CEEPEW_UI_EVENT_QUEUE_DEPTH, element size = UIEvent_t
 */
void task_session_init(void)
{
    if (s_session_initialised && g_session_rx_queue != NULL && g_ui_event_queue != NULL) {
        return;
    }

    /* Use the radio HAL RX queue directly so the session task consumes the
     * actual frames produced by the ESP-NOW callback path. */
    if (g_session_rx_queue == NULL) {
        g_session_rx_queue = hal_radio_get_rx_queue();
        CEEPEW_ASSERT_VOID(g_session_rx_queue != NULL);
    }

    /* Create UI event queue — session task posts UI events here */
    if (g_ui_event_queue == NULL) {
        g_ui_event_queue = xQueueCreate(CEEPEW_UI_EVENT_QUEUE_DEPTH, sizeof(UIEvent_t));
        CEEPEW_ASSERT_VOID(g_ui_event_queue != NULL);
    }

    /* Initialize statistics */
    s_stats.rx_frames_received = 0U;
    s_stats.rx_frames_processed = 0U;
    s_stats.queue_timeouts = 0U;
    s_visual_state_ready = false;
    s_session_initialised = true;

    ESP_LOGI(TAG, "Session task state initialized");
    (void)task_session_sync_visual_state();
}

/*
 * Main FreeRTOS task loop for Core 1.
 * Runs indefinitely, processing RX frames and posting UI events.
 * Only exits if FreeRTOS deletes the task.
 *
 * Main loop (1000ms timeout):
 * 1. xQueueReceive from g_session_rx_queue
 * 2. If frame received: process_rx_frame() and post UI_EVENT_MESSAGE_RX
 * 3. If timeout (pdFAIL): continue (no error)
 * 4. Handle any errors gracefully (silent discard, no crash)
 */
void task_session_run(void *pvParameters)
{
    (void)pvParameters;  /* Unused parameter */

    s_session_task_handle = xTaskGetCurrentTaskHandle();
    CEEPEW_ASSERT_VOID(s_session_task_handle != NULL);

    ESP_LOGI(TAG, "Session task started on Core %d", xPortGetCoreID());

    /* Main loop: drain RX queue and process frames */
    for (;;) {
        if (g_session_rx_queue == NULL || g_ui_event_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        /* RX queue timeout: 1000ms per requirement */
        const TickType_t rx_timeout = pdMS_TO_TICKS(1000U);

        /* Receive raw radio frame from RX queue */
        RadioFrame_t frame = {0};
        BaseType_t result = xQueueReceive(g_session_rx_queue, &frame, rx_timeout);

        if (result == pdPASS) {
            /* Frame received from queue */
            s_stats.rx_frames_received++;

            /* Process the frame (decrypt, verify, etc.) */
            CeePewErr_t err = process_rx_frame(&frame);
            if (err != CEEPEW_OK) {
                continue;
            }

            /* Frame ownership: if frame was allocated, it should be freed here.
             * For now (initial implementation), frame is assumed to be
             * statically allocated by the radio callback.
             * Future sprints may use region_alloc and require cleanup here.
             */
        } else if (result == pdFAIL) {
            /* Queue receive timeout (1000ms) — this is normal, no error */
            s_stats.queue_timeouts++;
            
            /* Phase 4: Periodic session maintenance (check TTL, nonce exhaustion, etc) */
            if (session_is_active()) {
                /* Check if nonce counter is near exhaustion */
                uint64_t nonce_counter = session_get_nonce_counter();
                if (nonce_counter >= CEEPEW_NONCE_HARD_LIMIT) {
                    CEEPEW_LOG(TAG, "Nonce exhaustion detected (counter=%llu >= limit=%llu)",
                               (unsigned long long)nonce_counter,
                               (unsigned long long)CEEPEW_NONCE_HARD_LIMIT);
                    g_ui_ctx.error_start_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
                    (void)ui_manager_transition_to(UI_STATE_NONCE_EXHAUSTED);
                    g_ui_ctx.transition_ready = true;
                    continue;
                }

                /* Check if message TTL has expired */
                uint32_t idle_seconds = 0U;
                CeePewErr_t ttl_err = session_get_idle_seconds(&idle_seconds);
                if (ttl_err == CEEPEW_OK) {
                    uint32_t ttl_limit = CEEPEW_MESSAGE_TTL_S; /* 1 hour by default */
                    if (idle_seconds >= ttl_limit) {
                        CEEPEW_LOG(TAG, "Message TTL expired (idle=%lu >= limit=%lu)",
                                   (unsigned long)idle_seconds,
                                   (unsigned long)ttl_limit);
                        session_wipe();
                        continue;
                    }
                }
            }
            /* Continue to next iteration */
        } else {
            /* Unexpected return value from xQueueReceive */
            CEEPEW_LOG(TAG, "Unexpected xQueueReceive result: %d", (int)result);
        }

        /* Periodic session maintenance (placeholder for later sprints) */
        /* - Check nonce exhaustion
         * - Expire old messages
         * - Check session TTL
         * - Advance replay windows
         */
    }
}
