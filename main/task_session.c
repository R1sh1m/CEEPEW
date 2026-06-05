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
    #include "transport_ble.h"
    #include "ui_manager.h"
    #include "ceepew_config.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "freertos/queue.h"
    #include "esp_log.h"
    #include "esp_timer.h"
    #include <string.h>
    #include <stdio.h>

    static const char *TAG = "task_session";

    CeePewErr_t crypto_ascon_aead_decrypt(const uint8_t key[16], const uint8_t nonce[16], const uint8_t *ad, uint16_t ad_len, const uint8_t *ct, uint16_t ct_len, uint8_t *pt, uint16_t *pt_len);
    CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32], const uint8_t *msg, uint16_t msg_len, const uint8_t sig[64]);

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
    static bool s_ble_commitment_exchanged = false;
    static BleState_t s_last_ble_state = BLE_IDLE;
    static bool s_last_ble_discovered = false;
    static uint8_t s_last_ble_hits = 0U;
    static int8_t s_last_ble_rssi = 0;
    static uint64_t s_last_ble_scan_heartbeat_ms = 0ULL;
static uint64_t s_ble_scan_start_ms = 0ULL; /* ms when discovery pattern started */
    static bool s_ble_peer_latched = false;
    /* Jitter state for phase 2 derive gating to tolerate asymmetric timing */
    static uint32_t s_phase2_jitter_target_ms = 0U;
    static uint32_t s_phase2_jitter_start_ms = 0U;

    static void task_session_reset_pairing_static_state(void);

    static const char *task_session_ble_state_name(BleState_t state){
        switch (state) {
            case BLE_IDLE: return "idle";
            case BLE_ADVERTISING: return "advertising";
            case BLE_SCANNING: return "scanning";
            case BLE_ADVERTISING_AND_SCANNING: return "adv+scan";
            case BLE_CONNECTED: return "connected";
            case BLE_PAIRING: return "pairing";
            case BLE_DONE: return "done";
            default: return "unknown";
        }
    }

    static void task_session_format_mac(const uint8_t mac[6], char out[18]){
        (void)snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    static CeePewErr_t task_session_build_session_code(uint8_t session_code[32U]){
        CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
        CEEPEW_ASSERT(g_ui_ctx.current_state >= UI_STATE_CODE_ENTRY, CEEPEW_ERR_PARAM);

        /* Expand the 4-digit UI entry into the 32-byte session code expected by
        * the session FSM by repeating the user-selected digits across the buffer. */
        for (uint8_t i = 0U; i < 32U; i++) {
            /* Expand the 4-character UI entry (ASCII bytes) into 32-byte session code */
            uint8_t ch = g_ui_ctx.code_digits[i % 4U];
            /* If stored value is not printable, fallback to '0'+(val%10) */
            if (ch < 32U) { ch = (uint8_t)('0' + (ch % 10U)); }
            session_code[i] = ch;
        }

        return CEEPEW_OK;
    }

    /* main/task_session.c */

    static CeePewErr_t task_session_drive_ble_pairing(void){
        CEEPEW_ASSERT(session_get_phase() <= 3U, CEEPEW_ERR_PARAM);
        CEEPEW_ASSERT(g_ble_ctx.state <= BLE_DONE, CEEPEW_ERR_PARAM);
        uint8_t    phase     = session_get_phase();
        BleState_t ble_state = transport_ble_get_state();
        const BlePeerRecord_t *peer = transport_ble_get_peer_cached();
        if (g_ui_ctx.current_state == UI_STATE_PAIRING_SUCCESS ||
            g_ui_ctx.current_state == UI_STATE_PAIRING_FAILED) {
            return CEEPEW_OK;
        }
        /* Nothing to do once active session is established */
        if (phase == 3U && session_is_active()) {return CEEPEW_OK;}

        /* BLE link-drop detection during the pairing flow (GATTS sign_pk
         * phase). If the user is in code entry / countdown / confirm and the
         * BLE link has dropped to IDLE (no GATTC, no GATTS, not connecting),
         * force a UI revert to PAIRING_FAILED. */
        bool in_pairing_screen = (g_ui_ctx.current_state == UI_STATE_CODE_ENTRY ||
                                  g_ui_ctx.current_state == UI_STATE_COUNTDOWN ||
                                  g_ui_ctx.current_state == UI_STATE_CONFIRM);
        if (in_pairing_screen &&
            ble_state == BLE_IDLE &&
            !g_ble_ctx.gattc_connected &&
            !g_ble_ctx.gatts_connected &&
            !g_ble_ctx.connecting &&
            peer == NULL) {
            ESP_LOGW(TAG, "BLE link dropped during pairing flow — reverting UI to PAIRING_FAILED");
            ESP_LOGW(TAG, "  state=%s adv=%u scan=%u gattc=%u gatts=%u connect=%u committed=%u verified=%u",
                     task_session_ble_state_name(ble_state),
                     g_ble_ctx.is_advertising, g_ble_ctx.is_scanning,
                     g_ble_ctx.gattc_connected, g_ble_ctx.gatts_connected,
                     g_ble_ctx.connecting,
                     g_ble_ctx.commitment_verified ? 1U : 0U,
                     g_ble_ctx.handoff_ready ? 1U : 0U);
            g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
            (void)session_reset_to_discovery();
            task_session_reset_pairing_static_state();
            (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
            g_ui_ctx.transition_ready = true;
            return CEEPEW_OK;
        }

        /* Reset phase-latched flags whenever we drop below their stage. */
        if (phase < 1U) {
            s_ble_peer_latched = false;
        }
        if (phase < 2U) {
            s_ble_commitment_exchanged = false;
        }

        /* ── STEP 1: Accept peer MAC into session FSM once discovered ────────── */
        if (phase == 1U && peer != NULL && !s_ble_peer_latched) {
            char peer_mac[18];
            task_session_format_mac(peer->peer_mac, peer_mac);
            ESP_LOGI(TAG, "Discovery peer latched: mac=%s rssi=%d hits=%u name_len=%u",
                    peer_mac, (int)peer->rssi, (unsigned)g_ble_ctx.scan_hit_count,
                    (unsigned)peer->name_len);
            (void)session_phase1_accept_peer(peer->peer_mac);
            s_ble_peer_latched = true;
        }

        /* ── STEP 2: Determine initiator / responder role ─────────────────────
        * Lower MAC = initiator (opens the brief GATT connection for sign_pk).
        * Higher MAC = responder (waits for GATTS connect).                      */
        bool is_initiator = false;
        if (peer != NULL) {
            uint8_t self_mac[CEEPEW_DEVICE_ID_BYTES] = {0U};
            if (session_get_device_id(self_mac) == CEEPEW_OK) {
                is_initiator = (ceepew_ct_less(self_mac, peer->peer_mac,
                                    CEEPEW_DEVICE_ID_BYTES) != 0U);
            }
            ceepew_secure_zero(self_mac, sizeof(self_mac));
        }

        /*
         * ── STEP 3: NO GATT CONNECTION FOR COMMITMENT ────────────────────────
         *
         * Commitments are exchanged via BLE scan-response beacons (step 4).
         * A GATT connection is NOT initiated during commitment exchange —
         * this eliminates BLE+WiFi coexistence failures, MTU timeouts,
         * reconnect races, and the "peer vanished" false-positive that
         * plagued the previous design.
         *
         * The GATT server remains registered (for future use) and the
         * 0xFFF3 sign_pk characteristic accepts writes. After beacon match,
         * the lower-MAC device opens a brief GATT connection to deliver
         * its sign_pk (step 5).
         */

        /* ── STEP 4: Phase 2 initiate + commitment beacon broadcast ──────── */
        if (phase == 1U && peer != NULL &&
            (g_ui_ctx.current_state == UI_STATE_COUNTDOWN ||
             g_ui_ctx.current_state == UI_STATE_PAIRING)) {

            uint8_t session_code[32U];
            CeePewErr_t err = task_session_build_session_code(session_code);
            if (err != CEEPEW_OK) { return err; }

            err = session_phase2_initiate(session_code);
            ceepew_secure_zero(session_code, (uint32_t)sizeof(session_code));
            if (err != CEEPEW_OK) { return err; }

            /* Pre-populate commitment_digest so transport layer has a local
             * value to compare any received beacon against. */
            uint8_t commitment[CEEPEW_COMMITMENT_BYTES];
            err = session_get_commitment(commitment);
            if (err == CEEPEW_OK) {
                memcpy(g_ble_ctx.commitment_digest, commitment, CEEPEW_COMMITMENT_BYTES);
                g_ble_ctx.local_commitment_len = CEEPEW_COMMITMENT_BYTES;
            }
            ceepew_secure_zero(commitment, sizeof(commitment));
            if (err != CEEPEW_OK) { return err; }

            /*
             * Both devices broadcast their truncated commitment in the
             * SCAN_RSP manufacturer-specific AD. The peer will read it on
             * the next scan result via transport_ble_verify_pending_commitment.
             */
            uint8_t adv_commit[CEEPEW_COMMITMENT_ADV_BYTES];
            memcpy(adv_commit, g_ble_ctx.commitment_digest, CEEPEW_COMMITMENT_ADV_BYTES);
            CeePewErr_t beacon_err = transport_ble_set_commitment_beacon(
                                        adv_commit, CEEPEW_COMMITMENT_ADV_BYTES);
            ceepew_secure_zero(adv_commit, sizeof(adv_commit));
            if (beacon_err != CEEPEW_OK) {
                /* Non-fatal: the retry logic in step 4b will re-attempt */
                ESP_LOGW(TAG, "Commitment beacon set failed (%d) — will retry", (int)beacon_err);
            }

            s_ble_commitment_exchanged = true;

            /* Handle any peer commitment that arrived before ours was ready */
            (void)transport_ble_verify_pending_commitment();

            phase     = session_get_phase();  /* now == 2 */
            ble_state = transport_ble_get_state();
            ESP_LOGI(TAG, "Phase 2 ready — commitment beacon broadcast (role=%s)",
                     is_initiator ? "initiator" : "responder");
            return CEEPEW_OK;
        }

        /* ── STEP 4b: Retry commitment beacon if still unconfirmed ──────── */
        phase     = session_get_phase();
        ble_state = transport_ble_get_state();

        if (phase == 2U && !g_ble_ctx.commitment_beacon_active &&
            g_ble_ctx.local_commitment_len > 0U) {
            uint8_t adv_commit[CEEPEW_COMMITMENT_ADV_BYTES];
            memcpy(adv_commit, g_ble_ctx.commitment_digest, CEEPEW_COMMITMENT_ADV_BYTES);
            CeePewErr_t beacon_err = transport_ble_set_commitment_beacon(
                                        adv_commit, CEEPEW_COMMITMENT_ADV_BYTES);
            ceepew_secure_zero(adv_commit, sizeof(adv_commit));
            if (beacon_err != CEEPEW_OK) {
                ESP_LOGW(TAG, "Commitment beacon retry failed (%d)", (int)beacon_err);
            }
        }

        /* ── STEP 5: After beacon match — open brief GATT for sign_pk ────── */
        phase     = session_get_phase();
        ble_state = transport_ble_get_state();

        /*
         * Beacon match means handoff_ready == true (set by
         * transport_ble_verify_commitment_unlocked). If sign_pk has not
         * been received yet, the initiator opens a brief GATT connection
         * to write its sign_pk; the responder waits for the GATTS write.
         *
         * After CEEPEW_MAX_RECONNECT_ATTEMPTS sign_pk GATT failures we
         * give up and proceed to key derivation without sign_pk. The
         * fingerprint screen will be empty but the chat flow still works.
         *
         * Hybrid-GATT hardening (Phase 7): we additionally require
         *   (a) PAIRING_PHASE_GATT_IDENTITY (explicit state, not just phase==2)
         *   (b) commitment_verified (redundant with handoff_ready but explicit)
         *   (c) peer_gatt_ready (peer's beacon has bit-15 set — see Item 3)
         * before opening the GATTC connection. (c) is set asynchronously
         * by the beacon decoder when it sees the peer's re-broadcast
         * beacon with the GATT-ready flag flipped on. Until that
         * happens, the gate stays closed and the supervisor's 2s
         * watchdog fires — designed to fail-closed.
         */
        if (phase == 2U && transport_ble_handoff_ready() &&
            !g_ble_ctx.sign_pk_received &&
            g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS)
        {
            if (is_initiator && !g_ble_ctx.connecting &&
                !g_ble_ctx.gattc_connected && peer != NULL &&
                transport_ble_get_phase() == PAIRING_PHASE_GATT_IDENTITY &&
                g_ble_ctx.commitment_verified &&
                g_ble_ctx.peer_gatt_ready)
            {
                ESP_LOGI(TAG, "Beacon match — opening brief GATT for sign_pk exchange");
                CeePewErr_t conn_err = transport_ble_connect_to_peer(peer->peer_mac);
                if (conn_err != CEEPEW_OK && conn_err != CEEPEW_ERR_BUSY) {
                    ESP_LOGW(TAG, "sign_pk GATT connect failed: %d (will retry)", (int)conn_err);
                    g_ble_ctx.reconnect_attempts++;
                }
            }
            return CEEPEW_OK;
        }

        /* ── STEP 6: Key derivation gate (commitment matched + sign_pk ok) ── */
        phase     = session_get_phase();
        ble_state = transport_ble_get_state();

        if (phase == 2U && transport_ble_handoff_ready()) {

            /* Jittered backoff to reduce simultaneous-derive timing collision. */
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
            if (s_phase2_jitter_target_ms == 0U) {
                uint8_t local_dev[CEEPEW_DEVICE_ID_BYTES];
                (void)session_get_device_id(local_dev);
                uint32_t seed = now_ms ^ ((uint32_t)local_dev[5] << 8);
                if (peer != NULL) {
                    seed ^= ((uint32_t)peer->peer_mac[5] << 16) |
                            (uint32_t)peer->peer_mac[4];
                }
                /* jitter in [50, 500) ms */
                s_phase2_jitter_target_ms = 50U + (uint32_t)(seed % 450U);
                s_phase2_jitter_start_ms  = now_ms;
                ceepew_secure_zero(local_dev, sizeof(local_dev));
                ESP_LOGI(TAG, "phase2: jitter target %u ms",
                         (unsigned)s_phase2_jitter_target_ms);
            }
            if ((now_ms - s_phase2_jitter_start_ms) < s_phase2_jitter_target_ms) {
                return CEEPEW_OK;
            }
            s_phase2_jitter_target_ms = 0U;

            /* If sign_pk is still missing after retries, log a warning and
             * proceed. The fingerprint screen will display an empty value
             * and the first chat frame's signature will be unverifiable
             * until the peer's sign_pk is delivered via a later frame. */
            if (!g_ble_ctx.sign_pk_received) {
                ESP_LOGW(TAG,
                         "sign_pk exchange gave up (attempts=%u) — proceeding without it",
                         (unsigned)g_ble_ctx.reconnect_attempts);
            }

            /* Set the role BEFORE derivation so the nonce counter starts at
             * the correct parity (initiator: 0/even, responder: 1/odd).
             * Both peers compute the same role from the same MAC ordering. */
            (void)session_set_role(is_initiator);

            CeePewErr_t err = session_phase2_derive_key();
            if (err != CEEPEW_OK) { return err; }

            g_ble_ctx.accumulated_conn_ms = 0U;
            s_ble_commitment_exchanged    = false;

            /* BLE is no longer needed for pairing — disconnect any stale link */
            (void)transport_ble_disconnect();

            /* Post-derive sync barrier: do NOT advance the UI to
             * PAIRING_SUCCESS until an encrypted MSG_TYPE_KEY_ACK
             * round-trip has been verified. This prevents state desync
             * where one device shows "✓ SECURE" while the other is
             * still in PAIRING because key derivation completed locally
             * but the peer never confirmed. The actual ACK exchange is
             * driven by task_session_drive_post_derive_sync() on the
             * session task's main loop. */
            ESP_LOGI(TAG, "Session key derived — entering post-derive sync (role=%s)",
                     is_initiator ? "initiator" : "responder");

            (void)ui_manager_transition_to(UI_STATE_KEYDER);
            g_ui_ctx.transition_ready = true;

            /* Stay in phase 3 (keys derived) but with sync_barrier_cleared
             * still false; the UI remains on KEYDER until the sync ACK is
             * verified. session_is_active() will return true (keys exist)
             * but the post-derive sync is what gates the UI. */
        }

        return CEEPEW_OK;
    }
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
            case UI_STATE_CODE_INCORRECT: return "code_incorrect";
            case UI_STATE_CODE_DIFFERENT: return "code_different";
            case UI_STATE_CONFIRM: return "confirm";
            case UI_STATE_KEYDER: return "keyder";
            case UI_STATE_FINGERPRINT: return "fingerprint";
            case UI_STATE_FINGERPRINT_CONFIRM: return "fp_confirm";
            case UI_STATE_CHAT: return "chat";
            case UI_STATE_CHAT_MENU: return "chat_menu";
            case UI_STATE_CHAT_COMPOSE: return "compose";
            case UI_STATE_CHAT_SEND_CONFIRM: return "send_confirm";
            case UI_STATE_CRYPTOGRAM: return "cryptogram";
            case UI_STATE_NONCE_EXHAUSTED: return "nonce_exhausted";
            case UI_STATE_INFO: return "info";
            case UI_STATE_ERROR: return "error";
            case UI_STATE_PAIRING: return "pairing";
            case UI_STATE_PAIRING_SUCCESS: return "pair_ok";
            case UI_STATE_PAIRING_FAILED: return "pair_fail";
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
            case RGB_YELLOW_RED_BLINK: return "yellow_red_blink";
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
            case UI_STATE_CHAT_MENU:
                return RGB_AMBER_PULSE;
            case UI_STATE_CHAT_COMPOSE:
                return RGB_GREEN_PULSE;
            case UI_STATE_CHAT_SEND_CONFIRM:
                return RGB_CYAN_PULSE;
            case UI_STATE_NONCE_EXHAUSTED:
            case UI_STATE_ERROR:
                return RGB_RED_BLINK;    /* Red blink for errors */
            default:
                return RGB_OFF;
        }
    }

    /* Reset task-local static state after pairing failure.
     * Ensures consistency with FSM phase after session_reset_to_discovery(). */
    static void task_session_reset_pairing_static_state(void)
    {
        s_ble_peer_latched = false;
        s_ble_commitment_exchanged = false;
        s_phase2_jitter_target_ms = 0U;
        s_phase2_jitter_start_ms = 0U;
    }

    CeePewErr_t task_session_sync_visual_state(void)
    {
        uint8_t phase = session_get_phase();
        bool active = session_is_active();
        BleState_t ble_state = transport_ble_get_state();
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
        const BlePeerRecord_t *peer = transport_ble_get_peer();
        bool discovered = (peer != NULL);
        uint8_t ble_hits = g_ble_ctx.scan_hit_count;
        int8_t ble_rssi = g_ble_ctx.peer_rssi;
        bool scan_active = (ble_state == BLE_SCANNING || ble_state == BLE_ADVERTISING_AND_SCANNING);

        /* Phase 3 (ACTIVE) — once the session is live, nudge any pre-chat UI
         * into the secure-chat derivation path without disturbing later states. */
        if (phase == 3U && active) {
            UIState_t current = g_ui_ctx.current_state;
            bool pre_chat = (current == UI_STATE_DISCOVERY   ||
                             current == UI_STATE_COUNTDOWN   ||
                             current == UI_STATE_CODE_ENTRY  ||
                             current == UI_STATE_CONFIRM     ||
                             current == UI_STATE_BOOT);

            if (pre_chat) {
                ESP_LOGI(TAG, "sync: session active but UI on %u — forcing KEYDER",
                         (unsigned)current);
                (void)ui_manager_transition_to(UI_STATE_KEYDER);
                g_ui_ctx.transition_ready = true;
            }
        }

        UIState_t ui_state = g_ui_ctx.current_state;
        RgbPattern_t pattern = task_session_map_pattern(ui_state, active);

        /* Track discovery scan start time so we can enforce a safe timeout */
        if (scan_active) {
            if (s_ble_scan_start_ms == 0ULL) { s_ble_scan_start_ms = now_ms; }
        }
        else { s_ble_scan_start_ms = 0ULL; }

        bool changed = (!s_visual_state_ready ||
                        phase != s_last_session_phase ||
                        active != s_last_session_active ||
                        ui_state != s_last_ui_state ||
                        pattern != s_last_rgb_pattern ||
                        ble_state != s_last_ble_state ||
                        discovered != s_last_ble_discovered ||
                        ble_hits != s_last_ble_hits ||
                        ble_rssi != s_last_ble_rssi);

        if (changed) {
            char peer_mac[18] = "<none>";
            char peer_name[17] = "<none>";
            if (peer != NULL) {
                task_session_format_mac(peer->peer_mac, peer_mac);
                if (g_ble_ctx.peer_record.name_len > 0U) {
                    uint8_t name_len = g_ble_ctx.peer_record.name_len;
                    if (name_len > 16U) { name_len = 16U; }
                    memcpy(peer_name, g_ble_ctx.peer_record.name, name_len);
                    peer_name[name_len] = '\0';
                }
            }

            ESP_LOGI(TAG,
                    "[%llu ms] phase=%s ui=%s active=%u rgb=%s ble=%s adv=%u scan=%u discovered=%u hits=%u peer=%s rssi=%d name=%s conn=%u/%u",
                    (unsigned long long)now_ms,
                    task_session_phase_name(phase),
                    task_session_ui_name(ui_state),
                    active ? 1U : 0U,
                    task_session_rgb_name(pattern),
                    task_session_ble_state_name(ble_state),
                    g_ble_ctx.is_advertising ? 1U : 0U,
                    g_ble_ctx.is_scanning ? 1U : 0U,
                    discovered ? 1U : 0U,
                    (unsigned)ble_hits,
                    peer_mac,
                    (int)ble_rssi,
                    peer_name,
                    g_ble_ctx.gattc_connected ? 1U : 0U,
                    g_ble_ctx.gatts_connected ? 1U : 0U);
        }

        if (scan_active && (s_last_ble_scan_heartbeat_ms == 0ULL ||
            (now_ms - s_last_ble_scan_heartbeat_ms) >= 2000ULL)) {
            char peer_mac[18] = "<none>";
            if (peer != NULL) {
                task_session_format_mac(peer->peer_mac, peer_mac);
            }

            ESP_LOGI(TAG, "scan heartbeat: state=%s seen=%lu peer_hits=%u peer=%s rssi=%d adv=%u scan=%u",
                    task_session_ble_state_name(ble_state),
                    (unsigned long)g_ble_ctx.scan_seen_count,
                    (unsigned)ble_hits,
                    peer_mac,
                    (int)ble_rssi,
                    g_ble_ctx.is_advertising ? 1U : 0U,
                    g_ble_ctx.is_scanning ? 1U : 0U);
            /* Detect stuck scanning: if we have been scanning but have not
             * seen a discovered peer for >30s, restart discovery. Use the
             * previous heartbeat value for comparison. */
            uint32_t prev_hb = (uint32_t)s_last_ble_scan_heartbeat_ms;
            s_last_ble_scan_heartbeat_ms = now_ms;
            if (prev_hb != 0U && g_ble_ctx.is_scanning && !g_ble_ctx.discovered &&
                (now_ms - prev_hb) > 30000U) {
                ESP_LOGW(TAG, "Scan appears stuck (>30s without discovery) — restarting discovery");
                (void)transport_ble_restart_discovery_session();
                /* Refresh heartbeat after restart */
                s_last_ble_scan_heartbeat_ms = now_ms;
            }
        }
        if (!scan_active) {
            s_last_ble_scan_heartbeat_ms = 0ULL;
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
        s_last_ble_state = ble_state;
        s_last_ble_discovered = discovered;
        s_last_ble_hits = ble_hits;
        s_last_ble_rssi = ble_rssi;
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

        /* Post-derive sync routing (Bug 3 fix). A successful round-trip
         * decryption of a 1-byte sync payload is the proof that crypto_box
         * works in both directions with the converged keys. Route to the
         * sync handler instead of the message store. */
        if (decoded_len == 1U &&
            (s_work_frame[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ||
             s_work_frame[0] == CEEPEW_KEY_SYNC_ACK_BYTE)) {
            CeePewErr_t sync_err = session_handle_key_sync_byte(s_work_frame[0]);
            if (sync_err == CEEPEW_ERR_NEED_TX) {
                /* Responder received HELLO — send the ACK back. */
                uint8_t ack_plain[1] = { CEEPEW_KEY_SYNC_ACK_BYTE };
                uint8_t peer_mac[6];
                uint8_t peer_pk[32];
                if (session_get_peer_public_key(peer_pk) == CEEPEW_OK) {
                    memcpy(peer_mac, frame->src_mac, 6U);
                    (void)session_send_message(ack_plain, 1U, peer_mac, peer_pk);
                }
            }
            ceepew_secure_zero(s_work_frame, sizeof(s_work_frame));
            (void)session_update_last_message_time();
            return CEEPEW_OK;
        }

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
                
                /* Discovery timeout fallback: if we've been scanning for too long, clear RGB to avoid stuck-on LEDs */
                {
                    uint64_t now_ms_local = (uint64_t)(esp_timer_get_time() / 1000LL);
                    const uint64_t DISCOVERY_TIMEOUT_MS = 60000ULL; /* 60s conservative fallback */
                    if (s_ble_scan_start_ms != 0ULL && (now_ms_local - s_ble_scan_start_ms) >= DISCOVERY_TIMEOUT_MS) {
                        ESP_LOGI(TAG, "Discovery timeout: clearing RGB after %llu ms", (unsigned long long)(now_ms_local - s_ble_scan_start_ms));
                        (void)rgb_set_pattern(RGB_OFF);
                        s_ble_scan_start_ms = 0ULL;
                    }
                }

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

                    /* Post-derive sync barrier (Bug 3 fix). Until the encrypted
                     * HELLO/ACK round-trip completes, the UI must remain on
                     * KEYDER. The drive function returns ERR_TIMEOUT once
                     * the deadline elapses; we then transition to FAILED. */
                    if (!session_sync_barrier_cleared()) {
                        uint64_t now_ms_local = (uint64_t)(esp_timer_get_time() / 1000LL);
                        CeePewErr_t sync_err = session_drive_post_derive_sync(now_ms_local);
                        if (sync_err == CEEPEW_ERR_TIMEOUT) {
                            CEEPEW_LOG(TAG, "Post-derive sync timed out after %u ms — failing pairing",
                                       (unsigned)CEEPEW_KEY_SYNC_TIMEOUT_MS);
                            g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_TIMED_OUT;
                            (void)session_reset_to_discovery();
                            task_session_reset_pairing_static_state();
                            (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                            g_ui_ctx.transition_ready = true;
                            continue;
                        }
                    } else if (g_ui_ctx.current_state == UI_STATE_KEYDER) {
                        /* Barrier just cleared — promote UI to PAIRING_SUCCESS */
                        CEEPEW_LOG(TAG, "Post-derive sync cleared — promoting to PAIRING_SUCCESS");
                        g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_SUCCESS;
                        (void)ui_manager_transition_to(UI_STATE_PAIRING_SUCCESS);
                        g_ui_ctx.transition_ready = true;
                    }
                }

                /* Phase 2 periodic: verify any buffered peer commitment.
                 * The beacon path populates pending_peer_commitment from
                 * SCAN_RESULT_EVT; this tick drains it once both sides have
                 * a local commitment ready. */
                if (session_get_phase() == 2U && g_ble_ctx.peer_commitment_pending) {
                    CeePewErr_t verify_err = transport_ble_verify_pending_commitment();
                    if (verify_err != CEEPEW_OK) {
                        CEEPEW_LOG(TAG, "Periodic commitment re-check failed: %d",
                                   (int)verify_err);
                    }
                }

                CeePewErr_t pair_err = task_session_drive_ble_pairing();
                if (pair_err != CEEPEW_OK) {
                    CEEPEW_LOG(TAG, "BLE pairing driver returned %d", (int)pair_err);
                    if (g_ui_ctx.current_state != UI_STATE_PAIRING_FAILED &&
                        g_ui_ctx.current_state != UI_STATE_PAIRING_SUCCESS) {
                        if (pair_err == CEEPEW_ERR_BUSY) {
                            /* [FIX-1] A write retry is pending — not a permanent failure */
                            CEEPEW_LOG(TAG, "BLE pairing driver busy (write retry pending)");
                        } else {
                            if (pair_err == CEEPEW_ERR_TIMEOUT) {
                                g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_TIMED_OUT;
                            } else if (pair_err == CEEPEW_ERR_AUTH_FAIL ||
                                       pair_err == CEEPEW_ERR_SIG_FAIL ||
                                       pair_err == CEEPEW_ERR_CRYPTO) {
                                g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_COMMITMENT_FAIL;
                            } else {
                                g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
                            }
                            (void)session_reset_to_discovery();
                            task_session_reset_pairing_static_state();
                            (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
                            g_ui_ctx.transition_ready = true;
                        }
                    }
                }

                /* ── BLE scan retry (Bug 6 fix) ──────────────────────────────
                 * If a previous scan start failed (e.g., transient coexistence
                 * resource conflict), retry it on this periodic tick.          */
                (void)transport_ble_retry_scan_if_needed();

                /* Continue to next iteration */
            } else {
                /* Unexpected return value from xQueueReceive */
                CEEPEW_LOG(TAG, "Unexpected xQueueReceive result: %d", (int)result);
            }

            /* Periodic session maintenance (placeholder for sprints) */
            /* - Check nonce exhaustion
            * - Expire old messagesI
            * - Check session TTL
            * - Advance replay windows
            */
        }
    }
