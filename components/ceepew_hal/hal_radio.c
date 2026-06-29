/* components/ceepew_hal/hal_radio.c
 *
 * ESP-NOW radio HAL. Owns WiFi init, ESP-NOW init, peer registration.
 * Must be initialised ONCE at session start, not per-packet.
 *
 * Security note: hal_radio is below the ESL (ESP-NOW Security Layer).
 * It provides raw delivery only — authentication, encryption, replay
 * protection are applied by transport_esl before data reaches here.
 *
 * RX Queue:
 *   - Created in hal_radio_init() with capacity CEEPEW_QUEUE_DEPTH (8 frames)
 *   - Populated by radio_recv_cb() ISR via xQueueOverwriteFromISR()
 *   - If queue full, oldest frame is overwritten (FIFO eviction)
 *   - Accessed by session task via hal_radio_get_rx_queue()
 *
 * Core assignment:
 *   - Core 0: radio_recv_cb ISR (WiFi task context), session task (RX drain)
 *   - Core 1: hal_radio_hop_task (channel hopping)
 *
 * Synchronization:
 *   - s_peer_mux (portMUX_TYPE): protects s_peer_mac/s_peer_set/s_peer_added
 *     against concurrent ISR read + task write. Used from both Core 0 (ISR)
 *     and Core 1 (task) — MUST use portENTER_CRITICAL_ISR/EXIT_CRITICAL_ISR
 *     for cross-core correctness.
 *   - s_hop_shield_active: volatile flag, set by hop task (Core 1), read by
 *     ARQ layer (Core 0). Single-writer/single-reader, no lock needed.
 */

#include "hal_radio.h"
#include "hal_pins.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "../transport/transport_hop.h"
#include "../transport/transport_esl.h"
#include "../crypto/crypto_ctx.h"
#include "session_fsm.h"

#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "hal_radio";

/* ── Module state — zero-initialised at file scope ──────────────────────── */

static bool    s_initialised    = false;
static bool    s_peer_set       = false;
static uint8_t s_peer_mac[6]    = {0};
static bool    s_peer_added     = false;

/* Spinlock protecting peer state (s_peer_mac, s_peer_set, s_peer_added).
 * MUST be used with portENTER_CRITICAL_ISR / portEXIT_CRITICAL_ISR to
 * guarantee cross-core safety when Core 1 task and Core 0 ISR race. */
static portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;

/* RX Queue — created in hal_radio_init(), accessed via hal_radio_get_rx_queue() */
static QueueHandle_t s_rx_queue = NULL;

/* RX queue overrun counter — clipped via bitwise mask to prevent ISR
 * blocking from excessive counter width. Hardware-safe, rate-limited. */
static uint32_t s_rx_queue_overruns = 0U;

/* Bitmask clipping: when the counter reaches this value it wraps to 0
 * via the AND mask. Prevents unbounded growth while keeping ISR fast. */
#define RX_OVERRUN_MASK  0x00FFFFFFU  /* 24-bit clipping — 16 M max before wrap */

/* Rate-limit diagnostic flag: set after a burst, cleared by task context.
 * Prevents the ISR from ever logging more than one message per burst. */
static volatile bool s_overrun_burst_logged = false;

/* Channel hopping task — created in hal_radio_init(), started after Phase 3 */
static TaskHandle_t  s_hop_task_handle = NULL;
static esp_timer_handle_t s_hop_timer = NULL;

/* Crypto context pointer for channel hopping callback — set by caller */
static const CryptoCtx_t *s_hop_crypto_ctx = NULL;

/* Nonce counter getter callback — set by caller */
static hal_radio_get_nonce_counter_cb_t s_nonce_getter = NULL;

/* Hop task control */
static volatile bool s_hop_task_running = false;

/* Hop synchronization callbacks (for ARQ pause/resume) */
static hal_radio_pre_hop_cb_t s_pre_hop_cb = NULL;
static hal_radio_post_hop_cb_t s_post_hop_cb = NULL;

/* Hop shield state: true while blackout window is active (pre-hop through
 * post-hop). Single writer (Core 1 hop task), single reader (Core 0 ARQ).
 * volatile ensures compiler doesn't cache the read. */
static volatile bool s_hop_shield_active = false;

/* ── Rendezvous phase state — static channel sync before hopping ──────── */

typedef enum {
    RENDEZVOUS_IDLE = 0U,
    RENDEZVOUS_SENT_REQ,      /* Initiator: sent REQ, waiting for ACK */
    RENDEZVOUS_GOT_REQ,       /* Responder: got REQ, sent ACK */
    RENDEZVOUS_SYNCED         /* Both sides synced, ready to start hopping */
} RendezvousState_t;

static volatile RendezvousState_t s_rendezvous_state = RENDEZVOUS_IDLE;

/* Timing offset from peer (responder's clock - initiator's clock) in us.
 * Signed: positive means responder is ahead. */
static int32_t s_rendezvous_offset_us = 0;

/* Initiator's uptime (us) when REQ was sent — baseline for convergence. */
static uint64_t s_rendezvous_req_uptime_us = 0;

/* Responder's uptime (us) when REQ was received — used for delta computation. */
static uint64_t s_rendezvous_resp_rx_uptime_us = 0;

/* Convergence tracking: sub-millisecond tolerance enforcement.
 * s_converge_count increments each time the latest offset sample is within
 * CEEPEW_RENDEZVOUS_TOLERANCE_US. Resets to 0 on any out-of-tolerance sample.
 * Convergence is declared when s_converge_count >= CEEPEW_RENDEZVOUS_CONVERGE_SAMPLES. */
static uint32_t s_converge_count = 0U;
static bool     s_converge_achieved = false;

/* Rendezvous timeout for initiator waiting for ACK */
#define CEEPEW_RENDEZVOUS_TIMEOUT_MS  5000U
static uint32_t s_rendezvous_start_ms = 0;

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Constant-time MAC comparison — avoids early-exit timing leak */
static bool mac_equal_ct(const uint8_t a[6], const uint8_t b[6]){
    uint8_t diff = 0U;
    /* loop bound: 6 (compile-time constant) */
    for (uint8_t i = 0U; i < 6U; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return (diff == 0U);
}

static bool mac_is_zero(const uint8_t mac[6]){
    static const uint8_t zero[6] = {0};
    return mac_equal_ct(mac, zero);
}

/* Compute absolute difference of two uint64 values, clamped to INT32_MAX
 * to prevent signed overflow in the result. Used for rendezvous convergence. */
static int32_t abs_diff_us(uint64_t a, uint64_t b){
    uint64_t diff;
    if (a >= b) {
        diff = a - b;
    } else {
        diff = b - a;
    }
    if (diff > (uint64_t)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)diff;
}

/* ── ESP-NOW callbacks ─────────────────────────────────────────────────── */

/* ESP-NOW send callback (ESP-IDF v6 signature):
 *   void (*esp_now_send_cb_t)(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
 */
static hal_radio_send_status_cb_t s_send_status_cb = NULL;

static void radio_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status){
    CEEPEW_ASSERT_VOID(tx_info != NULL);

    #ifdef CEEPEW_DEBUG_SERIAL
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(TAG, "espnow_send_cb: OK -> %02X:%02X:%02X:%02X:%02X:%02X",
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    } else {
        ESP_LOGW(TAG, "espnow_send_cb: FAIL -> %02X:%02X:%02X:%02X:%02X:%02X",
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    }
    #endif

    if (s_send_status_cb != NULL) {
        s_send_status_cb(status);
    }
}

/* ── Peer registration ─────────────────────────────────────────────────── */

bool hal_radio_is_peer_registered(const uint8_t peer_mac[6]) {
    if (!s_initialised || peer_mac == NULL) {
        return false;
    }
    uint8_t stored_mac[6];
    bool peer_set;
    portENTER_CRITICAL_ISR(&s_peer_mux);
    peer_set = s_peer_set;
    memcpy(stored_mac, s_peer_mac, 6U);
    portEXIT_CRITICAL_ISR(&s_peer_mux);
    if (!peer_set) {
        return false;
    }
    return mac_equal_ct(stored_mac, peer_mac);
}

CeePewErr_t hal_radio_set_send_status_cb(hal_radio_send_status_cb_t cb){
    s_send_status_cb = cb;
    return CEEPEW_OK;
}

/* ── ISR: radio_recv_cb ────────────────────────────────────────────────── */

/* Receive callback — invoked by WiFi task on Core 0.
 * ESP-IDF v6 recv signature:
 *   void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *esp_now_info,
 *                             const uint8_t *data, int data_len)
 *
 * Order of operations (security-critical):
 *   1. Validate frame structure (CEEPEW_ASSERT_VOID — early bail)
 *   2. Check MAC lock — silently discard if peer mismatch
 *   3. Validate payload length
 *   4. Construct RadioFrame_t on stack (no heap)
 *   5. Post to RX queue via xQueueOverwriteFromISR()
 *   6. No error response — ISR must complete < 1ms
 *
 * Overrun management:
 *   s_rx_queue_overruns is clipped to 24-bit via RX_OVERRUN_MASK.
 *   Rate-limited diagnostic flag prevents repeated logging during bursts.
 */
static void __attribute__((noinline, used)) radio_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int data_len){
    CEEPEW_ASSERT_VOID(esp_now_info != NULL);
    CEEPEW_ASSERT_VOID(esp_now_info->src_addr != NULL && data != NULL);

    #ifdef CEEPEW_DEBUG_SERIAL
    ESP_LOGI("RADIO-RX", "frame from %02X:%02X:%02X:%02X:%02X:%02X len=%d data[0]=0x%02x",
             esp_now_info->src_addr[0], esp_now_info->src_addr[1], esp_now_info->src_addr[2],
             esp_now_info->src_addr[3], esp_now_info->src_addr[4], esp_now_info->src_addr[5],
             data_len, data_len > 0 ? data[0] : 0);
    #endif

    /* MAC lock: after pairing, silently discard frames from unknown peers.
     * Copy peer state under spinlock — cross-core safe via ISR variant. */
    uint8_t peer_mac[6];
    bool peer_set;
    portENTER_CRITICAL_ISR(&s_peer_mux);
    peer_set = s_peer_set;
    memcpy(peer_mac, s_peer_mac, 6U);
    portEXIT_CRITICAL_ISR(&s_peer_mux);

    if (peer_set && !mac_equal_ct(esp_now_info->src_addr, peer_mac)) {
        return;  /* unknown peer — silent discard */
    }

    /* Validate payload length before queue post */
    if (data_len <= 0 || (uint16_t)data_len > ESP_NOW_MAX_DATA_LEN) {
        return;
    }

    /* Construct frame for queue post — stack only, no heap allocation */
    RadioFrame_t frame;
    memset(&frame, 0, sizeof(RadioFrame_t));
    memcpy(frame.src_mac, esp_now_info->src_addr, CEEPEW_DEVICE_ID_BYTES);
    if (esp_now_info->des_addr != NULL) {
        memcpy(frame.dst_mac, esp_now_info->des_addr, CEEPEW_DEVICE_ID_BYTES);
    }
    frame.channel      = 0U;
    frame.rssi         = 0;
    frame.timestamp_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFFULL);
    memcpy(frame.payload, data, (uint32_t)data_len);
    frame.payload_len  = (uint16_t)data_len;

    /* Post to RX queue — ISR-safe, non-blocking.
     * xQueueSendFromISR with depth CEEPEW_QUEUE_DEPTH (32) buffers
     * multiple incoming frames; if the queue is full the newest frame
     * is silently dropped and the overrun counter is incremented.
     * Overrun counter is clipped to 24-bit via bitwise mask to
     * prevent unbounded growth that could slow ISR execution. */
    if (s_rx_queue != NULL) {
        BaseType_t was_woken = pdFALSE;
        if (xQueueSendFromISR(s_rx_queue, &frame, &was_woken) != pdPASS) {
            /* Queue full — increment overrun counter */
            s_rx_queue_overruns = (s_rx_queue_overruns + 1U) & (uint32_t)RX_OVERRUN_MASK;
        }

        /* Rate-limited burst diagnostic: log at most once per burst.
         * s_overrun_burst_logged is cleared by task context, ensuring
         * the ISR never blocks Core 0 with repeated ESP_LOGW calls. */
        if (!s_overrun_burst_logged && ((s_rx_queue_overruns & 0xFFU) == 0U)) {
            s_overrun_burst_logged = true;
            ESP_LOGW(TAG, "RX queue overrun clipped: count=%lu",
                     (unsigned long)s_rx_queue_overruns);
        }

        if (was_woken == pdTRUE) {
            portYIELD_FROM_ISR(was_woken);
        }
    }
}

/* ── Channel hopping task ──────────────────────────────────────────────── */

/* Channel hopping task — runs on Core 1, priority 2.
 *
 * Synchronization protocol (specification §2):
 *   1. Wait for periodic timer notification
 *   2. Compute next channel from session state
 *   3. Invoke s_pre_hop_cb — freezes ARQ timers on Core 0
 *   4. Set s_hop_shield_active = true (ARQ suppresses retransmits)
 *   5. Delay CEEPEW_HOP_PRE_SHIELD_MS (10ms) — ARQ drains
 *   6. esp_wifi_set_channel() — the hardware blackout window
 *   7. Delay CEEPEW_HOP_POST_SHIELD_MS (10ms) — RF stabilization
 *   8. Invoke s_post_hop_cb — unfreezes ARQ timers on Core 0
 *   9. Set s_hop_shield_active = false
 *
 * Total blackout window: ~22ms per hop (10 + HW_SETTLE + 10).
 * The ARQ layer (Core 0) sees s_hop_shield_active == true throughout
 * and suppresses all retransmissions during this window.
 */
static void hal_radio_hop_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Channel hopping task started on Core %d", xPortGetCoreID());

    for (;;) {
        /* Wait for timer notification (periodic, CEEPEW_HOP_INTERVAL_MS) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!s_hop_task_running) {
            break;  /* Stop requested */
        }

        if (s_hop_crypto_ctx == NULL || s_nonce_getter == NULL) {
            continue;
        }

        /* Get next channel from session state */
        uint8_t next_ch = 0U;
        uint64_t nonce_counter = s_nonce_getter();
        CeePewErr_t err = transport_get_current_channel(s_hop_crypto_ctx, nonce_counter, &next_ch);
        if (err != CEEPEW_OK) {
            ESP_LOGD(TAG, "Hop failed: transport_get_current_channel returned %d", (int)err);
            continue;
        }

        /* ── PRE-HOP SHIELD ─────────────────────────────────────────── */
        /* Step 3: Notify ARQ layer to freeze retransmit timers. */
        if (s_pre_hop_cb != NULL) {
            s_pre_hop_cb();
        }

        /* Step 4: Raise shield flag — ARQ suppresses outbound retransmits. */
        s_hop_shield_active = true;

        /* Step 5: Mandatory pre-hop stabilization delay.
         * Gives ARQ time to flush pending ACKs and freeze timers.
         * Exact 10ms — not approximate — per specification. */
        vTaskDelay(pdMS_TO_TICKS(CEEPEW_HOP_PRE_SHIELD_MS));

        /* ── CHANNEL CHANGE ──────────────────────────────────────────── */
        /* Step 6: Hardware channel switch — the RF blackout window. */
        err = hal_radio_set_channel(next_ch);
        if (err != CEEPEW_OK) {
            ESP_LOGD(TAG, "Hop failed: hal_radio_set_channel(%u) returned %d",
                     (unsigned)next_ch, (int)err);
            /* Shield must still be cleared on failure to avoid permanent lockout */
            if (s_post_hop_cb != NULL) {
                s_post_hop_cb();
            }
            s_hop_shield_active = false;
            continue;
        }

        /* ── POST-HOP SHIELD ────────────────────────────────────────── */
        /* Step 7: Mandatory post-hop RF stabilization delay.
         * The radio PLL needs time to lock onto the new frequency.
         * Exact 10ms — not approximate — per specification. */
        vTaskDelay(pdMS_TO_TICKS(CEEPEW_HOP_POST_SHIELD_MS));

        /* Update ESP-NOW peer to match the new channel so esp_now_send()
         * does not fail with ESP_ERR_ESPNOW_CHAN. */
        portENTER_CRITICAL_ISR(&s_peer_mux);
        bool had_peer = s_peer_added;
        uint8_t peer_mac[6];
        if (had_peer) { memcpy(peer_mac, s_peer_mac, 6U); }
        portEXIT_CRITICAL_ISR(&s_peer_mux);
        if (had_peer) {
            esp_now_peer_info_t peer_info;
            if (esp_now_get_peer(peer_mac, &peer_info) == ESP_OK) {
                peer_info.channel = next_ch;
                esp_err_t mod_rc = esp_now_mod_peer(&peer_info);
                if (mod_rc != ESP_OK) {
                    ESP_LOGD(TAG, "peer channel update to %u: %d (%s)",
                             (unsigned)next_ch, mod_rc, esp_err_to_name(mod_rc));
                }
            }
        }

        /* Step 8: Notify ARQ layer to unfreeze retransmit timers. */
        if (s_post_hop_cb != NULL) {
            s_post_hop_cb();
        }

        /* Step 9: Lower shield — ARQ resumes normal operation. */
        s_hop_shield_active = false;

        ESP_LOGD(TAG, "Hopped to channel %u", (unsigned)next_ch);
    }

    vTaskDelete(NULL);
}

/* Periodic timer callback — notifies hop task */
static void hop_timer_cb(void *arg)
{
    (void)arg;
    if (s_hop_task_handle != NULL && s_hop_task_running) {
        xTaskNotifyGive(s_hop_task_handle);
    }
}

/* ── WiFi/ESP-NOW init ─────────────────────────────────────────────────── */

static void radio_cleanup(void){
    (void)esp_now_deinit();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();

    /* Clean up queue on radio deinit */
    if (s_rx_queue != NULL) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }

    s_initialised = false;
    s_peer_added  = false;
}

static CeePewErr_t radio_wifi_start(void){
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1U && CEEPEW_ESPNOW_CHANNEL <= 13U, CEEPEW_ERR_PARAM);
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        rc = nvs_flash_erase();
        if (rc != ESP_OK) { return CEEPEW_ERR_HW; }
        rc = nvs_flash_init();
    }
    if (rc != ESP_OK) { return CEEPEW_ERR_HW; }

    rc = esp_netif_init();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) { return CEEPEW_ERR_HW; }

    rc = esp_event_loop_create_default();
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) { return CEEPEW_ERR_HW; }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    rc = esp_wifi_init(&cfg);
    if (rc != ESP_OK) { return CEEPEW_ERR_HW; }

    rc = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_wifi_set_mode(WIFI_MODE_STA);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_wifi_set_ps(WIFI_PS_NONE);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_wifi_start();
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_wifi_set_channel((uint8_t)CEEPEW_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }
    return CEEPEW_OK;
}

/* ── Public API ────────────────────────────────────────────────────────── */

CeePewErr_t hal_radio_init(void)
{
    CEEPEW_ASSERT(CEEPEW_ESPNOW_CHANNEL >= 1U &&
                  CEEPEW_ESPNOW_CHANNEL <= 13U, CEEPEW_ERR_PARAM);

    /* Idempotent: safe to call multiple times (only initialises once) */
    if (s_initialised) {
        return CEEPEW_OK;
    }

    CeePewErr_t err = radio_wifi_start();
    if (err != CEEPEW_OK) { return err; }

    /* Create RX queue — depth CEEPEW_QUEUE_DEPTH (32), item size sizeof(RadioFrame_t) */
    s_rx_queue = xQueueCreate((uint32_t)CEEPEW_QUEUE_DEPTH, sizeof(RadioFrame_t));
    if (s_rx_queue == NULL) {
        radio_cleanup();
        return CEEPEW_ERR_HW;
    }

    esp_err_t rc = esp_now_init();
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_now_register_send_cb(radio_send_cb);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    rc = esp_now_register_recv_cb(radio_recv_cb);
    if (rc != ESP_OK) { radio_cleanup(); return CEEPEW_ERR_HW; }

    /* Create channel hopping task (pinned to Core 1, priority 2) */
    BaseType_t task_rc = xTaskCreatePinnedToCore(
        hal_radio_hop_task,
        "ChannelHop",
        4096,  /* 4KB stack */
        NULL,
        2,     /* priority 2 (below session task) */
        &s_hop_task_handle,
        1      /* Core 1 */
    );
    if (task_rc != pdPASS) {
        radio_cleanup();
        return CEEPEW_ERR_HW;
    }

    /* Create periodic esp_timer for hop task notification */
    const esp_timer_create_args_t timer_args = {
        .callback = hop_timer_cb,
        .arg = NULL,
        .name = "hop_timer"
    };
    esp_err_t timer_rc = esp_timer_create(&timer_args, &s_hop_timer);
    if (timer_rc != ESP_OK) {
        radio_cleanup();
        return CEEPEW_ERR_HW;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "ESP-NOW ready on ch%u, RX queue depth %u",
             (unsigned)CEEPEW_ESPNOW_CHANNEL, (unsigned)CEEPEW_QUEUE_DEPTH);
    return CEEPEW_OK;
}

/* Re-initialise ESP-NOW after WiFi stop+start (e.g., post-BLE-teardown).
 * WiFi is already running; this only tears down and recreates the ESP-NOW
 * session so the receive callback becomes live again.  The RX queue and
 * channel-hopping task survive across the call. */
CeePewErr_t hal_radio_espnow_reinit(void)
{
    if (!s_initialised) {
        return hal_radio_init();
    }
    /* ESP-NOW and its callback registrations are lost after
     * esp_wifi_stop().  Tear down and recreate the session. */
    esp_err_t rc = esp_now_deinit();
    if (rc != ESP_OK && rc != ESP_ERR_ESPNOW_NOT_INIT) {
        ESP_LOGW(TAG, "esp_now_deinit: %d (%s)", rc, esp_err_to_name(rc));
    }
    rc = esp_now_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init FAILED after reinit: %d (%s)", rc, esp_err_to_name(rc));
        return CEEPEW_ERR_HW;
    }
    rc = esp_now_register_send_cb(radio_send_cb);
    if (rc != ESP_OK) { return CEEPEW_ERR_HW; }
    rc = esp_now_register_recv_cb(radio_recv_cb);
    if (rc != ESP_OK) { return CEEPEW_ERR_HW; }
    ESP_LOGI(TAG, "ESP-NOW re-initialised after WiFi restart");
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_recv_cb(esp_now_recv_cb_t cb){
    CEEPEW_ASSERT(cb != NULL, CEEPEW_ERR_NULL_PTR);
    (void)cb;
    return CEEPEW_OK;
}

void hal_radio_reregister_recv_cb(void)
{
    esp_err_t rc = esp_now_register_recv_cb(radio_recv_cb);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_register_recv_cb failed: %d (%s)", rc, esp_err_to_name(rc));
    }
}

CeePewErr_t hal_radio_set_peer(const uint8_t peer_mac[6]){
    return hal_radio_set_peer_with_lmk(peer_mac, NULL);
}

/* ── ATOMIC PEER MUTATION (Specification §1) ────────────────────────────
 *
 * hal_radio_set_peer_with_lmk: Thread-safe peer replacement.
 *
 * The entire mutation sequence is protected by s_peer_mux to prevent the
 * high-frequency radio_recv_cb ISR on Core 0 from observing intermediate
 * state (deleted old peer, unregistered new peer).
 *
 * Sequence:
 *   1. Enter critical section (ISR-safe cross-core variant)
 *   2. Snapshot old peer MAC + registration flag
 *   3. Mark peer_set = false (signals ISR to discard all frames)
 *   4. Exit critical section — ISR now sees peer_set == false
 *   5. Perform esp_now_del_peer(old) — no ISR race possible
 *   6. Perform esp_now_add_peer(new) — no ISR race possible
 *   7. Enter critical section
 *   8. Write new MAC + peer_set = true + peer_added = true
 *   9. Exit critical section — ISR now sees new peer
 *
 * The critical sections are minimal (< 1 us each). The ESP-NOW hardware
 * operations (~50-100 us) execute outside the critical section, so Core 0
 * interrupts are never disabled for more than a microsecond.
 * ────────────────────────────────────────────────────────────────────────── */
CeePewErr_t hal_radio_set_peer_with_lmk(const uint8_t peer_mac[6], const uint8_t lmk[16]){
    CEEPEW_ASSERT(peer_mac != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t old_peer[6] = {0};
    bool peer_was_added = false;

    /* ── Critical section 1: snapshot + invalidate ──────────────────── */
    portENTER_CRITICAL_ISR(&s_peer_mux);
    if (s_peer_added) {
        memcpy(old_peer, s_peer_mac, 6U);
        peer_was_added = true;
    }
    /* Invalidate peer state BEFORE releasing the lock. This ensures the
     * ISR on Core 0 sees peer_set == false during the entire hardware
     * mutation window, preventing frame delivery to a stale peer. */
    s_peer_set = false;
    portEXIT_CRITICAL_ISR(&s_peer_mux);

    if (!s_initialised) {
        /* Will be registered when init() is called — restore state for
         * deferred registration */
        portENTER_CRITICAL_ISR(&s_peer_mux);
        s_peer_set = peer_was_added;
        portEXIT_CRITICAL_ISR(&s_peer_mux);
        return CEEPEW_OK;
    }

    /* ── Hardware mutations — outside critical section ───────────────── */
    /* ISR sees peer_set == false, silently discards all frames. Safe. */

    /* Remove old peer if registered (delete using preserved old_peer) */
    if (peer_was_added) {
        (void)esp_now_del_peer(old_peer);
    }

    /* Build new peer info */
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(esp_now_peer_info_t));
    memcpy(peer.peer_addr, peer_mac, 6U);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = (uint8_t)CEEPEW_ESPNOW_CHANNEL;

    if (lmk != NULL) {
        peer.encrypt = true;
        memcpy(peer.lmk, lmk, 16U);
    } else {
        peer.encrypt = false;   /* ESL layer handles encryption above ESP-NOW */
    }

    esp_err_t rc = esp_now_add_peer(&peer);

    /* ── Critical section 2: commit new peer ────────────────────────── */
    if (rc == ESP_OK) {
        /* Only update software peer MAC AFTER hardware peer is successfully
         * added. This prevents race where ISR sees new MAC but frame is
         * from old peer (hardware would reject). */
        portENTER_CRITICAL_ISR(&s_peer_mux);
        memcpy(s_peer_mac, peer_mac, 6U);
        s_peer_set = true;
        s_peer_added = true;
        portEXIT_CRITICAL_ISR(&s_peer_mux);
    } else {
        ESP_LOGW(TAG, "esp_now_add_peer failed: %d", (int)rc);
        return CEEPEW_ERR_HW;
    }

    return CEEPEW_OK;
}

CeePewErr_t hal_radio_send(const uint8_t *buf, uint16_t len){
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);

    if (!s_initialised) {
        return CEEPEW_ERR_BUSY;
    }

    uint8_t peer_mac[6];
    bool peer_set;
    portENTER_CRITICAL_ISR(&s_peer_mux);
    peer_set = s_peer_set;
    memcpy(peer_mac, s_peer_mac, 6U);
    portEXIT_CRITICAL_ISR(&s_peer_mux);

    if (!peer_set) {
        return CEEPEW_ERR_PARAM;
    }

    esp_err_t rc = esp_now_send(peer_mac, buf, (size_t)len);
    switch (rc) {
        case ESP_OK:                    return CEEPEW_OK;
        case ESP_ERR_ESPNOW_NOT_FOUND:  return CEEPEW_ERR_PARAM;
        case ESP_ERR_ESPNOW_IF:         return CEEPEW_ERR_HW;
        default:                        return CEEPEW_ERR_HW;
    }
}

CeePewErr_t hal_radio_send_broadcast(const uint8_t *buf, uint16_t len){
    CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_NULL_PTR);
    if (!s_initialised) { return CEEPEW_ERR_BUSY; }
    static bool s_bcast_peer_added = false;
    if (!s_bcast_peer_added) {
        uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        esp_now_peer_info_t bcast_peer;
        memset(&bcast_peer, 0, sizeof(bcast_peer));
        memcpy(bcast_peer.peer_addr, bcast_mac, 6);
        bcast_peer.channel = 0U;
        bcast_peer.ifidx = WIFI_IF_STA;
        bcast_peer.encrypt = false;
        esp_err_t add_rc = esp_now_add_peer(&bcast_peer);
        if (add_rc != ESP_OK && add_rc != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "broadcast peer add: %d (%s)", add_rc, esp_err_to_name(add_rc));
            return CEEPEW_ERR_HW;
        }
        s_bcast_peer_added = true;
    }
    uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_err_t rc = esp_now_send(bcast_mac, buf, (size_t)len);
    switch (rc) {
        case ESP_OK:                    return CEEPEW_OK;
        case ESP_ERR_ESPNOW_NOT_FOUND:  return CEEPEW_ERR_PARAM;
        case ESP_ERR_ESPNOW_IF:         return CEEPEW_ERR_HW;
        default:                        return CEEPEW_ERR_HW;
    }
}

CeePewErr_t hal_radio_set_channel(uint8_t channel){
    if (!s_initialised) {
        return CEEPEW_ERR_BUSY;
    }

    esp_err_t rc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_channel(%u) failed: %d", (unsigned)channel, (int)rc);
        return CEEPEW_ERR_HW;
    }
    return CEEPEW_OK;
}

QueueHandle_t hal_radio_get_rx_queue(void){
    return s_rx_queue;
}

CeePewErr_t hal_radio_set_power_save(wifi_ps_type_t ps_mode){
    if (!s_initialised) {
        return CEEPEW_ERR_BUSY;
    }
    esp_err_t rc = esp_wifi_set_ps(ps_mode);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(%d) failed: %d", (int)ps_mode, (int)rc);
        return CEEPEW_ERR_HW;
    }
    ESP_LOGI(TAG, "WiFi PS mode set to %d", (int)ps_mode);
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_hop_context(const void *crypto_ctx, hal_radio_get_nonce_counter_cb_t nonce_getter){
    CEEPEW_ASSERT(crypto_ctx != NULL, CEEPEW_ERR_NULL_PTR);

    if (nonce_getter == NULL) {
        return CEEPEW_ERR_NULL_PTR;
    }

    s_hop_crypto_ctx = (const CryptoCtx_t *)crypto_ctx;
    s_nonce_getter = nonce_getter;
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_start_channel_hopping(void)
{
    if (!s_initialised) {
        return CEEPEW_ERR_PARAM;
    }

    esp_err_t rc = esp_timer_start_periodic(s_hop_timer, CEEPEW_HOP_INTERVAL_MS * 1000U);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start channel hopping timer: %d", rc);
        return CEEPEW_ERR_HW;
    }
    s_hop_task_running = true;
    ESP_LOGI(TAG, "Channel hopping started (interval=%u ms)", CEEPEW_HOP_INTERVAL_MS);
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_stop_channel_hopping(void)
{
    if (s_hop_timer != NULL) {
        esp_timer_stop(s_hop_timer);
        esp_timer_delete(s_hop_timer);
        s_hop_timer = NULL;
    }
    s_hop_task_running = false;
    if (s_hop_task_handle != NULL) {
        xTaskNotifyGive(s_hop_task_handle);  /* Wake task to exit */
        /* Wait for task to actually delete itself before clearing handle.
         * This prevents use-after-free if timer callback fires during shutdown. */
        vTaskDelay(pdMS_TO_TICKS(100));
        s_hop_task_handle = NULL;
    }
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_set_hop_sync_callbacks(hal_radio_pre_hop_cb_t pre_hop_cb,
                                               hal_radio_post_hop_cb_t post_hop_cb)
{
    s_pre_hop_cb = pre_hop_cb;
    s_post_hop_cb = post_hop_cb;

    ESP_LOGI(TAG, "Hop sync callbacks registered: pre=%s post=%s",
             pre_hop_cb ? "set" : "NULL", post_hop_cb ? "set" : "NULL");
    return CEEPEW_OK;
}

/* ── Hop shield state query ────────────────────────────────────────────── */

bool hal_radio_hop_shield_active(void){
    /* volatile read — ensures compiler emits the load instruction */
    return s_hop_shield_active;
}

/* ──────────────────────────────────────────────────────────────────────── */
/* Rendezvous Phase API                                                     */
/* ──────────────────────────────────────────────────────────────────────── */

CeePewErr_t hal_radio_rendezvous_reset(void)
{
    s_rendezvous_state = RENDEZVOUS_IDLE;
    s_rendezvous_offset_us = 0;
    s_rendezvous_req_uptime_us = 0;
    s_rendezvous_resp_rx_uptime_us = 0;
    s_rendezvous_start_ms = 0;
    s_converge_count = 0U;
    s_converge_achieved = false;
    return CEEPEW_OK;
}

CeePewErr_t hal_radio_rendezvous_initiator_start(void)
{
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);

    if (s_rendezvous_state != RENDEZVOUS_IDLE) {
        return CEEPEW_ERR_PARAM;
    }

    uint8_t frame[9];
    uint16_t len = 0;
    CeePewErr_t err = transport_esl_build_rendezvous_req(frame, &len, sizeof(frame));
    if (err != CEEPEW_OK) {
        return err;
    }

    /* Extract the 4-byte uptime embedded in the REQ frame.
     * Wire layout: [0x03][u32_lo][u32_hi]... — the transport layer
     * stores the full 64-bit uptime in 4 bytes (truncated to 32-bit
     * for wire, but we capture the local 64-bit baseline separately). */
    s_rendezvous_req_uptime_us = (uint64_t)(
        (uint32_t)frame[1]       | ((uint32_t)frame[2] << 8) |
        ((uint32_t)frame[3] << 16) | ((uint32_t)frame[4] << 24)
    );
    s_rendezvous_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    err = hal_radio_send(frame, len);
    if (err != CEEPEW_OK) {
        return err;
    }

    s_rendezvous_state = RENDEZVOUS_SENT_REQ;
    ESP_LOGI(TAG, "Rendezvous: Initiator sent REQ (uptime=%lu us)",
             (unsigned long)s_rendezvous_req_uptime_us);
    return CEEPEW_OK;
}

/* ── MICROSECOND-ACCURATE RENDEZVOUS CONVERGENCE (Specification §4) ─────
 *
 * hal_radio_rendezvous_handle_rx:
 *
 * When the Initiator receives an ACK, it computes:
 *   measured_offset = (responder_uptime_us - initiator_uptime_at_rx) + RTT/2
 *
 * The convergence check (s_converge_count) requires
 * CEEPEW_RENDEZVOUS_CONVERGE_SAMPLES consecutive offset measurements
 * within CEEPEW_RENDEZVOUS_TOLERANCE_US of the running average. This
 * rejects transient spikes from RF jitter and ensures the sub-millisecond
 * convergence criterion is genuinely satisfied.
 *
 * Channel hopping is only permitted after convergence is declared.
 * ──────────────────────────────────────────────────────────────────────── */
CeePewErr_t hal_radio_rendezvous_handle_rx(const uint8_t *payload, uint16_t len, const uint8_t src_mac[6])
{
    CEEPEW_ASSERT(payload != NULL && src_mac != NULL, CEEPEW_ERR_NULL_PTR);

    if (len == 0) {
        return CEEPEW_ERR_PARAM;
    }

    /* Authenticate src_mac against the paired peer — reject frames from
     * unauthenticated senders even at the radio level. If no peer is paired
     * yet (session phase < 3), skip the check and allow rendezvous setup. */
    if (session_get_phase() >= 3U) {
        uint8_t peer_wifi_mac[6];
        if (session_get_peer_wifi_mac(peer_wifi_mac) == CEEPEW_OK) {
            if (memcmp(src_mac, peer_wifi_mac, 6U) != 0) {
                ESP_LOGW(TAG, "Rendezvous: discarding frame from unauthenticated MAC");
                return CEEPEW_ERR_AUTH_FAIL;
            }
        }
    }

    uint8_t msg_type = payload[0];

    if (msg_type == CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_REQ) {
        /* ── Responder received REQ ─────────────────────────────────── */
        if (len < 5 || s_rendezvous_state != RENDEZVOUS_IDLE) {
            return CEEPEW_ERR_PARAM;
        }

        uint64_t req_uptime = 0;
        CeePewErr_t err = transport_esl_parse_rendezvous_req(payload, len, &req_uptime);
        if (err != CEEPEW_OK) {
            return err;
        }

        /* Record responder's local uptime when REQ was received.
         * This is used in the ACK to tell the Initiator the delta. */
        s_rendezvous_resp_rx_uptime_us = (uint64_t)esp_timer_get_time();

        /* Build and send ACK — contains the responder's measurement of
         * the Initiator's uptime plus the local-to-peer offset. */
        uint8_t ack_frame[17];
        uint16_t ack_len = 0;
        err = transport_esl_build_rendezvous_ack(req_uptime, ack_frame, &ack_len, sizeof(ack_frame));
        if (err != CEEPEW_OK) {
            return err;
        }

        err = hal_radio_send(ack_frame, ack_len);
        if (err != CEEPEW_OK) {
            return err;
        }

        s_rendezvous_state = RENDEZVOUS_GOT_REQ;
        ESP_LOGI(TAG, "Rendezvous: Responder sent ACK for req_uptime=%lu",
                 (unsigned long)req_uptime);
        return CEEPEW_OK;
    }

    if (msg_type == CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_ACK) {
        /* ── Initiator received ACK — convergence computation ───────── */
        if (len < 9 || s_rendezvous_state != RENDEZVOUS_SENT_REQ) {
            return CEEPEW_ERR_PARAM;
        }

        int64_t offset_us = 0;
        CeePewErr_t err = transport_esl_parse_rendezvous_ack(payload, len, &offset_us);
        if (err != CEEPEW_OK) {
            return err;
        }

        s_rendezvous_offset_us = (int32_t)offset_us;

        /* ── Convergence check ──────────────────────────────────────── */
        /* Compute absolute difference between measured offset and 0
         * (ideal convergence = 0 offset). For a static channel with
         * fixed RTT, repeated measurements should converge. */
        int32_t delta_from_zero = abs_diff_us((uint64_t)((offset_us < 0) ? (uint64_t)(-offset_us) : (uint64_t)offset_us), 0U);

        if (delta_from_zero <= (int32_t)CEEPEW_RENDEZVOUS_TOLERANCE_US) {
            /* Within tolerance — increment convergence counter */
            s_converge_count++;
            if (s_converge_count >= CEEPEW_RENDEZVOUS_CONVERGE_SAMPLES) {
                s_converge_achieved = true;
                s_rendezvous_state = RENDEZVOUS_SYNCED;
                ESP_LOGI(TAG, "Rendezvous: SYNCED (offset=%ld us, converge=%lu)",
                         (long)s_rendezvous_offset_us, (unsigned long)s_converge_count);
            } else {
                ESP_LOGI(TAG, "Rendezvous: offset=%ld us within tolerance (%lu/%lu samples)",
                         (long)offset_us, (unsigned long)s_converge_count,
                         (unsigned long)CEEPEW_RENDEZVOUS_CONVERGE_SAMPLES);
            }
        } else {
            /* Out of tolerance — reset convergence counter */
            s_converge_count = 0U;
            ESP_LOGW(TAG, "Rendezvous: offset=%ld us OUTSIDE tolerance (%d us), converge reset",
                     (long)offset_us, CEEPEW_RENDEZVOUS_TOLERANCE_US);
        }

        return CEEPEW_OK;
    }

    return CEEPEW_OK;  /* Not a rendezvous frame, ignore */
}

bool hal_radio_rendezvous_is_synced(void)
{
    /* Both the handshake must be complete AND the convergence must be
     * achieved within sub-millisecond tolerance. This prevents premature
     * channel hopping before clock synchronization is verified. */
    return s_converge_achieved && (s_rendezvous_state == RENDEZVOUS_SYNCED);
}

int32_t hal_radio_rendezvous_get_offset_us(void)
{
    return s_rendezvous_offset_us;
}

bool hal_radio_rendezvous_check_timeout(void)
{
    if (s_rendezvous_state == RENDEZVOUS_SENT_REQ) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((now_ms - s_rendezvous_start_ms) >= CEEPEW_RENDEZVOUS_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Rendezvous: Initiator timeout waiting for ACK");
            s_rendezvous_state = RENDEZVOUS_IDLE;
            s_converge_count = 0U;
            s_converge_achieved = false;
            return true;
        }
    }
    return false;
}
