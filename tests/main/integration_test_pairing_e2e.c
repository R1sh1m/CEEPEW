/* tests/main/integration_test_pairing_e2e.c
 *
 * Two-device automated pairing + encrypted message integration test.
 *
 * REQUIREMENTS:
 *   - CONFIG_CEEPEW_BUILD_TESTS=y
 *   - CONFIG_CEEPEW_HEADLESS_MODE=y (UI auto-advances through pairing)
 *   - Both devices must be flashed with the same firmware binary
 *   - Session code hardcoded to "ZZZZ" via headless mode
 *
 * DESIGN:
 *   A dedicated monitor task (Core 0, prio=idle+2) tracks the session FSM
 *   through every milestone and logs per-stage results. On reaching ACTIVE
 *   phase with sync+PFS complete, each device sends a test plaintext message
 *   and checks msg_store for incoming messages from the peer.  A structured
 *   "=== PAIRING E2E REPORT ===" is printed on completion, suitable for
 *   manual reading and machine grep.
 *
 *   The monitor self-deletes when the test completes or times out (120s).
 *
 * MILESTONES:
 *   INIT        test started
 *   PHASE2      session phase >= 2 (pairing initiated)
 *   PHASE3      session phase == 3 (key derivation done, session active)
 *   SYNC        post-derive sync barrier cleared (HELLO/ACK round-trip)
 *   PFS         PFS handshake complete (ephemeral ECDH over ESP-NOW)
 *   MSG_SENT    test message transmitted via session_send_message()
 *   MSG_RX      at least one message received from peer in msg_store
 *   DONE        all milestones reached
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "session_fsm.h"
#include "session_msgstore.h"
#include "crypto_ctx.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifdef CEEPEW_ENABLE_SELFTEST

static const char *TAG = "CEE-PEW-PAIRING-E2E";

#define TEST_MSG        "CEEPEW-TEST-HELLO"
#define TEST_MSG_LEN    16U

/* Timeout budgets (ms) — generous for staggered boot + retries */
#define TIMEOUT_TOTAL          180000UL
#define TIMEOUT_PHASE2          60000UL
#define TIMEOUT_PHASE3         120000UL
#define TIMEOUT_MSG_RX           45000UL

/* Milestone tracking */
typedef enum {
    MILESTONE_INIT = 0,
    MILESTONE_PHASE2_PAIRING,
    MILESTONE_PHASE3_ACTIVE,
    MILESTONE_SYNC_BARRIER,
    MILESTONE_PFS_ACTIVE,
    MILESTONE_MSG_SENT,
    MILESTONE_MSG_RECEIVED,
    MILESTONE_DONE,
    _MILESTONE_COUNT
} Milestone_t;

static const char *ms_name(Milestone_t m)
{
    static const char *names[_MILESTONE_COUNT] = {
        "INIT", "PHASE2", "PHASE3", "SYNC",
        "PFS", "MSG_SENT", "MSG_RX", "DONE"
    };
    return (m < _MILESTONE_COUNT) ? names[m] : "?";
}

typedef struct {
    Milestone_t milestone;
    bool        passed[_MILESTONE_COUNT];
    int64_t     ts[_MILESTONE_COUNT];
    uint8_t     local_mac[6];
    bool        is_initiator;
    bool        test_msg_sent;
    uint8_t     rx_count_before;
} TestCtx_t;

static TestCtx_t s_ctx;

static void mark(Milestone_t m)
{
    if (!s_ctx.passed[m]) {
        s_ctx.passed[m] = true;
        s_ctx.ts[m] = esp_timer_get_time();
        int64_t elapsed = (s_ctx.ts[m] - s_ctx.ts[MILESTONE_INIT]) / 1000LL;
        ESP_LOGI(TAG, "[MILESTONE] %s at t=%lld ms", ms_name(m), elapsed);
        if (m > s_ctx.milestone) {
            s_ctx.milestone = m;
        }
    }
}

static bool timed_out(int64_t now_us, int64_t deadline_us, Milestone_t expected)
{
    if (now_us < deadline_us) return false;
    ESP_LOGE(TAG, "[FAIL] timeout waiting for %s (t=%lld ms)",
             ms_name(expected), (now_us - s_ctx.ts[MILESTONE_INIT]) / 1000LL);
    return true;
}

static void pairing_e2e_monitor(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.ts[MILESTONE_INIT] = esp_timer_get_time();
    s_ctx.passed[MILESTONE_INIT] = true;
    esp_read_mac(s_ctx.local_mac, ESP_MAC_BT);

    ESP_LOGI(TAG, "=== PAIRING E2E TEST STARTED ===");
    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_ctx.local_mac[0], s_ctx.local_mac[1], s_ctx.local_mac[2],
             s_ctx.local_mac[3], s_ctx.local_mac[4], s_ctx.local_mac[5]);
    ESP_LOGI(TAG, "Headless mode: %s",
#if CONFIG_CEEPEW_HEADLESS_MODE
             "ENABLED"
#else
             "DISABLED (test will likely fail without UI interaction)"
#endif
    );

    int64_t start_us      = esp_timer_get_time();
    int64_t deadline_total = start_us + (int64_t)TIMEOUT_TOTAL * 1000LL;
    int64_t deadline_ph2  = start_us + (int64_t)TIMEOUT_PHASE2 * 1000LL;
    int64_t deadline_ph3  = start_us + (int64_t)TIMEOUT_PHASE3 * 1000LL;
    int64_t deadline_rx   = 0;

    bool ph2 = false, ph3 = false, sync = false, pfs = false;

    s_ctx.rx_count_before = msg_store_count();

    while (true) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= deadline_total) {
            ESP_LOGE(TAG, "[FAIL] total timeout (%u s)", TIMEOUT_TOTAL / 1000U);
            break;
        }

        uint8_t  phase        = session_get_phase();
        bool     active       = session_is_active();
        bool     sync_cleared = session_sync_barrier_cleared();
        bool     pfs_active   = session_pfs_active();
        uint8_t  rx_count     = msg_store_count();
        uint8_t  rx_new       = (rx_count > s_ctx.rx_count_before)
                                  ? (uint8_t)(rx_count - s_ctx.rx_count_before) : 0U;

        if (!ph2 && phase >= 2U) { mark(MILESTONE_PHASE2_PAIRING); ph2 = true; }

        if (!ph3 && active) {
            mark(MILESTONE_PHASE3_ACTIVE); ph3 = true;
            uint8_t peer_mac[6];
            if (session_get_peer_device_id(peer_mac) == CEEPEW_OK) {
                s_ctx.is_initiator = (memcmp(s_ctx.local_mac, peer_mac, 6) < 0);
                ESP_LOGI(TAG, "Role: %s", s_ctx.is_initiator ? "INITIATOR" : "RESPONDER");
            }
            deadline_rx = now_us + (int64_t)TIMEOUT_MSG_RX * 1000LL;
        }

        if (!sync && sync_cleared) { mark(MILESTONE_SYNC_BARRIER); sync = true; }
        if (!pfs && pfs_active)    { mark(MILESTONE_PFS_ACTIVE);   pfs = true; }

        if (!s_ctx.test_msg_sent && active && sync_cleared && pfs_active) {
            vTaskDelay(pdMS_TO_TICKS(s_ctx.is_initiator ? 2000 : 8000));

            uint8_t peer_mac[6];
            if (session_get_peer_wifi_mac(peer_mac) == CEEPEW_OK) {
                ESP_LOGI(TAG, "Sending test message (%u bytes)...", TEST_MSG_LEN);
                CeePewErr_t err = session_send_message(
                    (const uint8_t *)TEST_MSG, TEST_MSG_LEN, peer_mac, NULL);
                if (err == CEEPEW_OK) {
                    mark(MILESTONE_MSG_SENT);
                    s_ctx.test_msg_sent = true;
                    ESP_LOGI(TAG, "[PASS] test message transmitted");
                } else {
                    ESP_LOGE(TAG, "[FAIL] session_send_message = %d (%s)", (int)err,
                             err == CEEPEW_ERR_NONCE_EXHAUSTED ? "NONCE_EXHAUSTED" :
                             err == CEEPEW_ERR_PARAM ? "PARAM (not active/registered?)" :
                             err == CEEPEW_ERR_BUSY  ? "BUSY (mutex contention)" : "other");
                }
            }
            if (deadline_rx == 0) {
                deadline_rx = now_us + (int64_t)TIMEOUT_MSG_RX * 1000LL;
            }
        }

        if (s_ctx.test_msg_sent && rx_new > 0U && !s_ctx.passed[MILESTONE_MSG_RECEIVED]) {
            mark(MILESTONE_MSG_RECEIVED);
            ESP_LOGI(TAG, "[PASS] received %u message(s)", rx_new);
        }

        if (ph2 && ph3 && sync && pfs &&
            s_ctx.test_msg_sent && s_ctx.passed[MILESTONE_MSG_RECEIVED]) {
            mark(MILESTONE_DONE);
            break;
        }

        if (!ph2 && timed_out(now_us, deadline_ph2, MILESTONE_PHASE2_PAIRING)) break;
        if (ph2 && !ph3 && timed_out(now_us, deadline_ph3, MILESTONE_PHASE3_ACTIVE)) break;
        if (s_ctx.test_msg_sent && !s_ctx.passed[MILESTONE_MSG_RECEIVED] &&
            timed_out(now_us, deadline_rx, MILESTONE_MSG_RECEIVED)) break;

        vTaskDelay(pdMS_TO_TICKS(250));
    }

    /* Final report */
    int64_t now_us = esp_timer_get_time();
    int64_t total_ms = (now_us - start_us) / 1000LL;

    for (int i = 0; i < _MILESTONE_COUNT; i++) {
        if (!s_ctx.passed[i]) continue;
        int64_t t = (s_ctx.ts[i] - start_us) / 1000LL;
        ESP_LOGI(TAG, "  [%-8s] PASS at t=%lld ms", ms_name((Milestone_t)i), t);
    }

    bool all_ok = true;
    bool pairing_pass = ph2 && ph3 && sync && pfs;
    bool msg_tx_pass  = s_ctx.test_msg_sent;
    bool msg_rx_pass  = s_ctx.passed[MILESTONE_MSG_RECEIVED];

    if (pairing_pass) ESP_LOGI(TAG, "  [PAIRING  ] PASS");
    else { ESP_LOGE(TAG, "  [PAIRING  ] FAIL"); all_ok = false; }

    if (msg_tx_pass) ESP_LOGI(TAG, "  [MSG-TX   ] PASS");
    else { ESP_LOGE(TAG, "  [MSG-TX   ] FAIL"); all_ok = false; }

    if (msg_rx_pass) ESP_LOGI(TAG, "  [MSG-RX   ] PASS — %u msg(s) received",
                               msg_store_count() - s_ctx.rx_count_before);
    else { ESP_LOGE(TAG, "  [MSG-RX   ] FAIL — no incoming messages"); all_ok = false; }

    ESP_LOGI(TAG, "  Duration: %lld ms | Overall: %s", total_ms, all_ok ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "=== PAIRING E2E REPORT ===");
    ESP_LOGI(TAG, "  [%-18s] %s", "Pairing E2E", all_ok ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "=========================");

    vTaskDelete(NULL);
}

void integration_test_pairing_e2e_run(void)
{
    BaseType_t rc = xTaskCreatePinnedToCore(
        pairing_e2e_monitor, "pairing-e2e", 8192, NULL,
        tskIDLE_PRIORITY + 2, NULL, 0);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "FAILED to create pairing E2E monitor task (rc=%d)", (int)rc);
    }
}

#endif /* CEEPEW_ENABLE_SELFTEST */
