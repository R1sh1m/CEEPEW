/* main/task_session.c
 *
 * Session Task for Core 1 — Cryptographic transport and RX processing.
 * Drains incoming radio frames from RX queue, posts UI events on message
 * receipt. Phase 4: Adds TTL expiry checking, nonce exhaustion detection, and
 * session lifecycle logging.
 *
 * Design note: The session task provides the main crypto/transport processing
 * loop on Core 1. It receives encrypted frames via g_session_rx_queue
 * (populated by radio callbacks on Core 0), decrypts/verifies them, and
 * notifies the UI by posting events to g_ui_event_queue. This separation allows
 * Core 0 to focus on UI rendering (non-blocking, deterministic 30ms ticks)
 * while Core 1 handles variable-latency cryptographic operations. Phase 4 adds
 * periodic session maintenance checks (TTL, nonce exhaustion) that trigger
 * secure session wipe.
 */

#include "task_session.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "compress_huffman.h"
#include "crypto_box_wrap.h"
#include "crypto_ctx.h"
#include "ecc_crc32.h"
#include "ecc_hamming.h"
#include "esp_coexist.h"
#include "esp_random.h"
#include "esp_crc.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal_radio.h"
#include "hal_rgb.h"
#include "session_fsm.h"
#include "session_memory.h"
#include "session_msgstore.h"
#include "task_ui.h"
#include "transport_ble.h"
#include "transport_esl.h"
#include "ui_manager.h"
#include <stdio.h>
#include <string.h>

/* Forward declaration for ARQ hop sync initialization */
CeePewErr_t ecc_arq_init_hop_sync(void);

static const char *TAG = "task_session";

/* ── Coexistence diagnostic logging helpers ────────────────────────────── */
static inline uint32_t coex_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#define COEX_LOG(fmt, ...)  ESP_LOGI("coex_dbg", "[%lu] " fmt, coex_now_ms(), ##__VA_ARGS__)

CeePewErr_t crypto_ascon_aead_decrypt(const uint8_t key[16],
                                      const uint8_t nonce[16],
                                      const uint8_t *ad, uint16_t ad_len,
                                      const uint8_t *ct, uint16_t ct_len,
                                      uint8_t *pt, uint16_t *pt_len);
CeePewErr_t crypto_eddsa_verify(const uint8_t pub[32], const uint8_t *msg,
                                uint16_t msg_len, const uint8_t sig[64]);

/* -------------------------------------------------------------------------- */
/* Queue Instances (global, created by task_session_init)                    */
/* -------------------------------------------------------------------------- */

/* Session RX queue: radio callbacks post frames here */
QueueHandle_t g_session_rx_queue = NULL;

/* UI event queue: session task posts UI events here */
QueueHandle_t g_ui_event_queue = NULL;

/* UI context mutex (Phase 2.C1) — guards g_ui_ctx access between Core 0
 * (UI task writes) and Core 1 (session task reads). Recursive so the
 * session task can take it multiple times in nested call paths. */
SemaphoreHandle_t g_ui_ctx_lock = NULL;

void session_ui_ctx_lock(void) {
  if (g_ui_ctx_lock != NULL) {
    (void)xSemaphoreTakeRecursive(g_ui_ctx_lock, portMAX_DELAY);
  }
}

void session_ui_ctx_unlock(void) {
  if (g_ui_ctx_lock != NULL) {
    (void)xSemaphoreGiveRecursive(g_ui_ctx_lock);
  }
}

void session_ui_get_state_snapshot(UIState_t *out_state) {
  CEEPEW_ASSERT_VOID(out_state != NULL);
  session_ui_ctx_lock();
  *out_state = g_ui_ctx.current_state;
  session_ui_ctx_unlock();
}

/* ── Cross-core session wipe request (Phase 2.C2) ──────────────────
 * Protected by a spinlock for proper cross-core atomic access on
 * dual-core ESP32 (Xtensa). The compiler barrier alone is insufficient
 * for cache coherence between cores. */
static portMUX_TYPE s_wipe_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_session_wipe_requested = false;

void session_request_wipe(void) {
  portENTER_CRITICAL(&s_wipe_mux);
  g_session_wipe_requested = true;
  portEXIT_CRITICAL(&s_wipe_mux);
}

bool session_check_wipe_requested(void) {
  bool requested;
  portENTER_CRITICAL(&s_wipe_mux);
  requested = g_session_wipe_requested;
  g_session_wipe_requested = false;
  portEXIT_CRITICAL(&s_wipe_mux);
  return requested;
}

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
static uint64_t s_last_pfs_retry_ms = 0ULL;
static uint64_t s_ble_scan_start_ms =
    0ULL; /* ms when discovery pattern started */
static bool s_ble_peer_latched = false;
/* Jitter state for phase 2 derive gating to tolerate asymmetric timing */
static uint32_t s_phase2_jitter_target_ms = 0U;
static uint32_t s_phase2_jitter_start_ms = 0U;
/* Time-based escape hatch for STEP 5 sign_pk gate. If sign_pk exchange
 * hasn't completed within this timeout, force-proceed to STEP 6 in
 * degraded mode (missing peer sign_pk). This prevents permanent deadlock
 * when the GATT connection is lost before sign_pk exchange completes. */
static uint32_t s_sign_pk_gate_start_ms = 0U;
#define CEEPEW_SIGN_PK_GATE_TIMEOUT_MS 6000U  /* 6 s escape hatch */

/* M2 gate timeout escape: if peer_gatt_ready never arrives (ESP-IDF
 * caches scan response data and never re-polls), open GATTC anyway
 * after this timeout.  Safe because commitment_verified already
 * authenticates the peer. */
static uint32_t s_m2_gate_wait_start_ms = 0U;
#define CEEPEW_M2_GATE_TIMEOUT_MS 2000U

/* Initiator jitter state to avoid cross-connection race */
static bool s_initiator_jittered = false;
static uint32_t s_rev_gattc_backoff_stage = 0U;
static const uint32_t s_rev_gattc_backoff_ms[] = {
    CEEPEW_RESPONDER_REV_GATTC_BACKOFF_BASE_MS,
    CEEPEW_RESPONDER_REV_GATTC_BACKOFF_BASE_MS * 2U,
    CEEPEW_RESPONDER_REV_GATTC_BACKOFF_BASE_MS * 4U
};
#define CEEPEW_REV_GATTC_BACKOFF_STAGES \
    (sizeof(s_rev_gattc_backoff_ms) / sizeof(s_rev_gattc_backoff_ms[0]))

/* ── Pairing Flow Ownership (Option C) ──────────────────────────────────────
 * The session task now owns the pairing state machine. The BLE supervisor
 * only handles low-level radio recovery (disconnect/reconnect), NOT pairing
 * phase transitions. This prevents the race where the supervisor restarts
 * discovery while the session task is still in pairing. */
typedef enum {
  PAIRING_FLOW_IDLE = 0,       /* Not in pairing */
  PAIRING_FLOW_DISCOVERY = 1,  /* Waiting for peer discovery */
  PAIRING_FLOW_COMMITMENT = 2, /* Broadcasting/receiving commitment beacons */
  PAIRING_FLOW_GATT_IDENTITY =
      3,                       /* Brief GATT connection for sign_pk exchange */
  PAIRING_FLOW_KEY_DERIVE = 4, /* Deriving session keys */
  PAIRING_FLOW_POST_DERIVE_SYNC = 5, /* Encrypted ACK round-trip */
  PAIRING_FLOW_FAILED = 6,           /* Pairing failed, cleanup in progress */
} PairingFlowState_t;

static PairingFlowState_t s_pairing_flow_state = PAIRING_FLOW_IDLE;
static uint32_t s_pairing_flow_start_ms = 0U;
static uint32_t s_pairing_phase_entered_ms = 0U;
static uint8_t s_pairing_retry_count = 0U;

static void task_session_reset_pairing_static_state(void);
static void task_session_pairing_flow_reset(void);
static void task_session_pairing_flow_advance(PairingFlowState_t new_state);
static CeePewErr_t task_session_pairing_flow_drive(void);

static const char *task_session_ble_state_name(BleState_t state) {
  switch (state) {
  case BLE_IDLE:
    return "idle";
  case BLE_ADVERTISING:
    return "advertising";
  case BLE_SCANNING:
    return "scanning";
  case BLE_ADVERTISING_AND_SCANNING:
    return "adv+scan";
  case BLE_CONNECTED:
    return "connected";
  case BLE_PAIRING:
    return "pairing";
  case BLE_DONE:
    return "done";
  default:
    return "unknown";
  }
}

static void task_session_format_mac(const uint8_t mac[6], char out[18]) {
  (void)snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
}

static CeePewErr_t task_session_build_session_code(uint8_t session_code[32U]) {
  CEEPEW_ASSERT(session_code != NULL, CEEPEW_ERR_NULL_PTR);
  UIState_t ui_state;
  session_ui_get_state_snapshot(&ui_state);
  CEEPEW_ASSERT(ui_state >= UI_STATE_CODE_ENTRY, CEEPEW_ERR_PARAM);

  /* Snapshot code_digits under the same lock as the state — they must
   * be read consistently together (the user can be editing one of them
   * on Core 0 while we sample on Core 1). */
  session_ui_ctx_lock();
  uint8_t digits[4];
  memcpy(digits, g_ui_ctx.code_digits, 4U);
  session_ui_ctx_unlock();

  /* Expand the 4-digit UI entry into the 32-byte session code expected by
   * the session FSM by repeating the user-selected digits across the buffer. */
  for (uint8_t i = 0U; i < 32U; i++) {
    /* Expand the 4-character UI entry (ASCII bytes) into 32-byte session code
     */
    uint8_t ch = digits[i % 4U];
    /* If stored value is not printable, fallback to '0'+(val%10) */
    if (ch < 32U) {
      ch = (uint8_t)('0' + (ch % 10U));
    }
    session_code[i] = ch;
  }

  CEEPEW_LOG(TAG, "build_session_code: digits=[%u,%u,%u,%u] ascii=[%c,%c,%c,%c]",
           digits[0], digits[1], digits[2], digits[3],
           (digits[0] >= 32U && digits[0] < 127U) ? (char)digits[0] : '?',
           (digits[1] >= 32U && digits[1] < 127U) ? (char)digits[1] : '?',
           (digits[2] >= 32U && digits[2] < 127U) ? (char)digits[2] : '?',
           (digits[3] >= 32U && digits[3] < 127U) ? (char)digits[3] : '?');

  return CEEPEW_OK;
}

/* main/task_session.c */

static CeePewErr_t task_session_drive_ble_pairing(void) {
  CEEPEW_ASSERT(session_get_phase() <= 3U, CEEPEW_ERR_PARAM);
  CEEPEW_ASSERT(g_ble_ctx.state <= BLE_DONE, CEEPEW_ERR_PARAM);
  uint8_t phase = session_get_phase();
  BleState_t ble_state = transport_ble_get_state();
  const BlePeerRecord_t *peer = transport_ble_get_peer_cached();
  /* Phase 2.C1: snapshot current_state under the UI mutex once at the
   * top of this function. The original 7 read sites below all hit the
   * same logical value within a single pairing-flow iteration. */
  UIState_t ui_state;
  session_ui_get_state_snapshot(&ui_state);
  if (ui_state == UI_STATE_PAIRING_FAILED) {
    task_session_pairing_flow_reset();
    return CEEPEW_OK;
  }

  /* RENDEZVOUS PHASE: After key derivation and BLE teardown, before channel hopping,
   * both devices must synchronize their hop timers via a plaintext handshake on
   * the static CEEPEW_ESPNOW_CHANNEL. This prevents out-of-phase hopping that
   * causes session_drive_post_derive_sync() to fail. */
  if (phase == 3U && session_is_active() && session_sync_barrier_cleared() &&
      session_peer_box_pubkey_valid() && session_peer_sign_pk_valid()) {
    /* Rendezvous handshake must complete before channel hopping starts */
    if (!session_rendezvous_synced()) {
      CeePewErr_t rv_err = session_drive_rendezvous();
      if (rv_err == CEEPEW_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "Rendezvous timeout — pairing failed");
        session_ui_ctx_lock();
        g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
        g_ui_ctx.transition_ready = true;
        session_ui_ctx_unlock();
        (void)session_reset_to_discovery();
        task_session_reset_pairing_static_state();
        task_session_pairing_flow_reset();
        (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
        return CEEPEW_OK;
      }
      if (rv_err != CEEPEW_OK) {
        return rv_err;  /* Transport error or still in progress */
      }
      /* Rendezvous just completed (CEEPEW_OK and now synced) — fall through to start hopping */
    }

    /* Start channel hopping timer once rendezvous is complete.
     * This enables ESP-NOW channel hopping for Phase 3 communication. */
    static bool s_hopping_started = false;
    if (!s_hopping_started) {
      CeePewErr_t hop_err = hal_radio_set_hop_context(&g_crypto_ctx, session_get_nonce_counter);
      if (hop_err == CEEPEW_OK) {
        /* Register ARQ hop sync callbacks before starting hopping */
        (void)ecc_arq_init_hop_sync();
        hop_err = hal_radio_start_channel_hopping();
        if (hop_err == CEEPEW_OK) {
          ESP_LOGI(TAG, "Channel hopping started (interval=%u ms) after rendezvous sync", CEEPEW_HOP_INTERVAL_MS);
          s_hopping_started = true;
        }
      }
    }
    task_session_pairing_flow_reset();
    return CEEPEW_OK;
  }

  /* Drive the pairing flow state machine (Option C: session task owns pairing)
   */
  (void)task_session_pairing_flow_drive();

  /* Re-read peer after flow drive — pairing_flow_drive may have cleared
   * the BLE peer record (stale-pointer fix: the peer captured at line 225
   * can dangle after transport_ble_clear_discovery_peer_state). */
  peer = transport_ble_get_peer_cached();

  /* If pairing flow has failed, don't proceed with pairing steps */
  if (s_pairing_flow_state == PAIRING_FLOW_FAILED) {
    return CEEPEW_OK;
  }

  /* BLE link-drop detection during the pairing flow (GATTS sign_pk
   * phase). If the user is in code entry / countdown / confirm and the
   * BLE link has dropped to IDLE (no GATTC, no GATTS, not connecting),
   * force a UI revert to PAIRING_FAILED.
   *
   * Re-read ble_state here — the snapshot taken at function entry
   * (line 271) is stale after the 80+ lines of pairing flow logic
   * that may have advanced BLE state (e.g. from IDLE to SCANNING). */
  ble_state = transport_ble_get_state();
  bool in_pairing_screen =
      (ui_state == UI_STATE_CODE_ENTRY || ui_state == UI_STATE_COUNTDOWN ||
       ui_state == UI_STATE_PAIRING || ui_state == UI_STATE_CONFIRM);
  if (in_pairing_screen && ble_state == BLE_IDLE &&
      !g_ble_ctx.gattc_connected && !g_ble_ctx.gatts_connected &&
      !g_ble_ctx.connecting && peer == NULL) {
    ESP_LOGW(TAG, "BLE link dropped during pairing flow — reverting UI to "
                  "PAIRING_FAILED");
    ESP_LOGW(TAG,
             "  state=%s adv=%u scan=%u gattc=%u gatts=%u connect=%u "
             "committed=%u verified=%u",
             task_session_ble_state_name(ble_state), g_ble_ctx.is_advertising,
             g_ble_ctx.is_scanning, g_ble_ctx.gattc_connected,
             g_ble_ctx.gatts_connected, g_ble_ctx.connecting,
             g_ble_ctx.commitment_verified ? 1U : 0U,
             g_ble_ctx.handoff_ready ? 1U : 0U);
    session_ui_ctx_lock();
    g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
    g_ui_ctx.transition_ready = true;
    session_ui_ctx_unlock();
    (void)session_reset_to_discovery();
    task_session_reset_pairing_static_state();
    task_session_pairing_flow_reset();
    (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
    return CEEPEW_OK;
  }

  /* Reset phase-latched flags whenever we drop below their stage. */
  if (phase < 1U) {
    s_ble_peer_latched = false;
    task_session_pairing_flow_reset();
  }
  if (phase < 2U) {
    s_ble_commitment_exchanged = false;
  }

  /* Initialize pairing flow on first entry to phase 1 */
  if (phase == 1U && peer != NULL &&
      s_pairing_flow_state == PAIRING_FLOW_IDLE) {
    task_session_pairing_flow_advance(PAIRING_FLOW_DISCOVERY);
  }

  /* ── STEP 1: Accept peer MAC into session FSM once discovered ────────── */
  uint8_t current_fsm_peer[6] = {0U};
  bool peer_mismatch = false;
  if (session_get_peer_device_id(current_fsm_peer) == CEEPEW_OK) {
    peer_mismatch =
        (peer != NULL && memcmp(current_fsm_peer, peer->peer_mac, 6U) != 0);
  }

  if (phase == 1U && peer != NULL && (!s_ble_peer_latched || peer_mismatch)) {
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
  if (phase >= 1U && peer != NULL) {
    uint8_t self_mac[CEEPEW_DEVICE_ID_BYTES] = {0U};
    if (session_get_device_id(self_mac) == CEEPEW_OK) {
      is_initiator = (ceepew_ct_less(self_mac, peer->peer_mac,
                                     CEEPEW_DEVICE_ID_BYTES) != 0U);
    }
    ceepew_secure_zero(self_mac, sizeof(self_mac));
  }

  /* Persist role early so the COUNTDOWN OLED and any downstream
   * logic see the correct INITIATOR / RESPONDER value. */
  (void)session_set_role(is_initiator);

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
      (ui_state == UI_STATE_COUNTDOWN || ui_state == UI_STATE_PAIRING ||
       ui_state == UI_STATE_CONFIRM)) {

    /* Clear any stale commitment beacon from a prior attempt so
     * a leftover wrong-code beacon doesn't poison this attempt. */
    transport_ble_clear_pending_commitment();

    uint8_t session_code[32U];
    CeePewErr_t err = task_session_build_session_code(session_code);
    if (err != CEEPEW_OK) {
      return err;
    }

    err = session_phase2_initiate(session_code);
    ceepew_secure_zero(session_code, (uint32_t)sizeof(session_code));
    if (err != CEEPEW_OK) {
      return err;
    }

    /* Pre-populate commitment_digest so transport layer has a local
     * value to compare any received beacon against. */
    uint8_t commitment[CEEPEW_COMMITMENT_BYTES];
    err = session_get_commitment(commitment);
    if (err == CEEPEW_OK) {
      memcpy(g_ble_ctx.commitment_digest, commitment, CEEPEW_COMMITMENT_BYTES);
      g_ble_ctx.local_commitment_len = CEEPEW_COMMITMENT_BYTES;
    }
    ceepew_secure_zero(commitment, sizeof(commitment));
    if (err != CEEPEW_OK) {
      return err;
    }

    /*
     * Both devices broadcast their truncated commitment in the
     * SCAN_RSP manufacturer-specific AD. The peer will read it on
     * the next scan result via transport_ble_verify_pending_commitment.
     */
    uint8_t adv_commit[CEEPEW_COMMITMENT_ADV_BYTES];
    memcpy(adv_commit, g_ble_ctx.commitment_digest,
           CEEPEW_COMMITMENT_ADV_BYTES);
    CeePewErr_t beacon_err = transport_ble_set_commitment_beacon(
        adv_commit, CEEPEW_COMMITMENT_ADV_BYTES);
    ceepew_secure_zero(adv_commit, sizeof(adv_commit));
    if (beacon_err != CEEPEW_OK) {
      /* Non-fatal: the retry logic in step 4b will re-attempt */
      ESP_LOGW(TAG, "Commitment beacon set failed (%d) — will retry",
               (int)beacon_err);
    }

    s_ble_commitment_exchanged = true;

    /* Handle any peer commitment that arrived before ours was ready */
    (void)transport_ble_verify_pending_commitment();

    phase = session_get_phase(); /* now == 2 */
    ble_state = transport_ble_get_state();
    ESP_LOGI(TAG, "Phase 2 ready — commitment beacon broadcast (role=%s)",
             is_initiator ? "initiator" : "responder");
    return CEEPEW_OK;
  }

  /* ── STEP 4b: Retry commitment beacon if still unconfirmed ──────── */
  phase = session_get_phase();
  ble_state = transport_ble_get_state();

  if (phase == 2U && !g_ble_ctx.commitment_beacon_active &&
      g_ble_ctx.local_commitment_len > 0U) {
    uint8_t adv_commit[CEEPEW_COMMITMENT_ADV_BYTES];
    memcpy(adv_commit, g_ble_ctx.commitment_digest,
           CEEPEW_COMMITMENT_ADV_BYTES);
    CeePewErr_t beacon_err = transport_ble_set_commitment_beacon(
        adv_commit, CEEPEW_COMMITMENT_ADV_BYTES);
    ceepew_secure_zero(adv_commit, sizeof(adv_commit));
    if (beacon_err != CEEPEW_OK) {
      ESP_LOGW(TAG, "Commitment beacon retry failed (%d)", (int)beacon_err);
    }
  }

  /* ── STEP 5: After beacon match — open brief GATT for sign_pk ────── */
  phase = session_get_phase();
  ble_state = transport_ble_get_state();

  /*
   * Beacon match means handoff_ready == true (set by
   * transport_ble_verify_commitment_unlocked). If sign_pk has not
   * been received yet, the initiator opens a brief GATT connection
   * to write its sign_pk; the responder waits for the GATTS write.
   *
   * After CEEPEW_MAX_RECONNECT_ATTEMPTS sign_pk GATT failures we
   * give up and proceed to key derivation without sign_pk. The
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
      g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS) {
    /* Time-based escape hatch: record when we first entered this gate.
     * If sign_pk exchange hasn't completed within the timeout, fall
     * through to STEP 6 in degraded mode instead of spinning forever. */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (s_sign_pk_gate_start_ms == 0U) {
      s_sign_pk_gate_start_ms = now_ms;
    }
    if ((now_ms - s_sign_pk_gate_start_ms) >= CEEPEW_SIGN_PK_GATE_TIMEOUT_MS) {
      ESP_LOGW(TAG,
               "STEP 5: sign_pk gate timeout (%u ms) — "
               "GATT exchange failed, advancing to FAILED",
               (unsigned)CEEPEW_SIGN_PK_GATE_TIMEOUT_MS);
      ESP_LOGW(TAG,
               "sign_pk gate timeout: init=%d "
               "sign_pk_rcvd=%d rev_pend=%d init_sent=%d "
               "attempts=%u elapsed=%u ms",
               is_initiator, g_ble_ctx.sign_pk_received,
               g_ble_ctx.reverse_gattc_pending,
               g_ble_ctx.initiator_sign_pk_sent,
               (unsigned)g_ble_ctx.reconnect_attempts,
               (unsigned)(now_ms - s_sign_pk_gate_start_ms));
      s_sign_pk_gate_start_ms = 0U;
      session_ui_ctx_lock();
      g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
      g_ui_ctx.transition_ready = true;
      session_ui_ctx_unlock();
      (void)session_reset_to_discovery();
      task_session_reset_pairing_static_state();
      task_session_pairing_flow_reset();
      (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
      return CEEPEW_OK;
    } else {
      ESP_LOGI(TAG,
               "M2 initiator gate: phase=%u handoff=%d "
               "sign_pk_rcvd=%d retries=%u init=%d conn=%d "
               "gattc_conn=%d ble_phase=%d commit=%d peer_ready=%d",
               phase, transport_ble_handoff_ready(), g_ble_ctx.sign_pk_received,
               g_ble_ctx.reconnect_attempts, is_initiator, g_ble_ctx.connecting,
               g_ble_ctx.gattc_connected, transport_ble_get_phase(),
               g_ble_ctx.commitment_verified, g_ble_ctx.peer_gatt_ready);
/* M2 gate timeout escape: if peer_gatt_ready never arrives
                     * (ESP-IDF caches scan response and never re-polls),
                     * open GATTC after CEEPEW_M2_GATE_TIMEOUT_MS. */
                    bool m2_peer_ready = g_ble_ctx.peer_gatt_ready;
                    if (!m2_peer_ready) {
                        if (s_m2_gate_wait_start_ms == 0U) {
                            s_m2_gate_wait_start_ms = now_ms;
                        }
                        if ((now_ms - s_m2_gate_wait_start_ms) >= CEEPEW_M2_GATE_TIMEOUT_MS) {
                            ESP_LOGW(TAG,
                                     "M2 gate peer_gatt_ready timeout %u ms — opening GATTC",
                                     (unsigned)CEEPEW_M2_GATE_TIMEOUT_MS);
                            m2_peer_ready = true;
                        }
                    } else {
                        s_m2_gate_wait_start_ms = 0U;
                    }
                    if (is_initiator && !g_ble_ctx.connecting && !g_ble_ctx.gattc_connected &&
                            !g_ble_ctx.gatts_connected &&
                            peer != NULL &&
                            transport_ble_get_phase() == PAIRING_PHASE_GATT_IDENTITY &&
                            g_ble_ctx.commitment_verified && m2_peer_ready) {
        /* Initiator jitter: random 100-300ms delay before opening GATTC
         * to avoid both sides connecting simultaneously when the responder
         * also attempts reverse GATTC (which has its own 50-150ms jitter). */
        if (!s_initiator_jittered) {
          uint32_t jitter_ms = 100U + ((uint32_t)esp_random() % (CEEPEW_INITIATOR_JITTER_MAX_MS + 1U));
          ESP_LOGI(TAG, "Initiator: applying %u ms jitter before GATTC connect", (unsigned)jitter_ms);
          vTaskDelay(pdMS_TO_TICKS(jitter_ms));
          s_initiator_jittered = true;
        }
        ESP_LOGI(TAG, "Beacon match — opening brief GATT for sign_pk exchange");
        CeePewErr_t conn_err = transport_ble_connect_to_peer(peer->peer_mac);
        if (conn_err == CEEPEW_OK) {
          /* Allow initiator's GATTC write (sign_pk+box_pk) to complete before
           * responder attempts reverse GATTC. Increased from 100ms to
           * CEEPEW_INITIATOR_RESPONDER_DELAY_MS (300ms) for more reliable
           * sequencing. */
          vTaskDelay(pdMS_TO_TICKS(CEEPEW_INITIATOR_RESPONDER_DELAY_MS));
        } else if (conn_err != CEEPEW_ERR_BUSY) {
          ESP_LOGW(TAG, "sign_pk GATT connect failed: %d (will retry)",
                   (int)conn_err);
          g_ble_ctx.reconnect_attempts++;
          s_initiator_jittered = false;  /* retry jitter on next attempt */
        }
      }

      return CEEPEW_OK;
    }
  }

  /* M3: Responder reverse GATTC — placed OUTSIDE the !sign_pk_received
   * gate so it can fire even after the responder already received the
   * initiator's sign_pk.  Without this, the responder skips STEP 5
   * (because sign_pk_received=true) and the reverse GATTC never opens,
   * leaving the initiator without the responder's sign_pk forever. */
  ESP_LOGI(TAG,
           "M3 responder gate: init=%d phase=%u handoff=%d "
           "rev_pend=%d conn=%d gattc_conn=%d "
           "init_sent=%d retries=%u",
           is_initiator, phase, transport_ble_handoff_ready(),
           g_ble_ctx.reverse_gattc_pending, g_ble_ctx.connecting,
           g_ble_ctx.gattc_connected, g_ble_ctx.initiator_sign_pk_sent,
           g_ble_ctx.reconnect_attempts);
  if (!is_initiator && phase == 2U && transport_ble_handoff_ready() &&
      g_ble_ctx.reverse_gattc_pending && !g_ble_ctx.connecting &&
      !g_ble_ctx.gattc_connected && !g_ble_ctx.gatts_connected &&
      !g_ble_ctx.initiator_sign_pk_sent &&
      g_ble_ctx.reconnect_attempts < CEEPEW_MAX_RECONNECT_ATTEMPTS) {
    /* Exponential backoff with jitter to avoid hammering the BLE stack
     * and to give the initiator time to complete its sign_pk write.
     * Stages: 1000ms, 2000ms, 4000ms (configurable via CEEPEW_RESPONDER_REV_GATTC_BACKOFF_BASE_MS) */
    static uint32_t s_rev_gattc_last_attempt_ms = 0U;
    static bool s_rev_gattc_jittered = false;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

    /* First-attempt jitter: both devices may try to open GATTC connections
     * simultaneously (~50 ms apart), causing "gatt_connect wrong state 2".
     * A random 50-150 ms delay on the responder's first attempt pushes one
     * side outside the race window. */
    if (!s_rev_gattc_jittered && s_rev_gattc_last_attempt_ms == 0U) {
      uint32_t jitter_ms = 50U + ((uint32_t)esp_random() % 101U);
      ESP_LOGI(TAG, "reverse GATTC: applying %u ms jitter to avoid cross-connection",
               (unsigned)jitter_ms);
      s_rev_gattc_last_attempt_ms = now_ms;
      s_rev_gattc_jittered = true;
      return CEEPEW_OK;
    }
    s_rev_gattc_jittered = false;

    /* Exponential backoff based on reconnect attempt stage */
    uint32_t stage = (s_rev_gattc_backoff_stage < CEEPEW_REV_GATTC_BACKOFF_STAGES)
                     ? s_rev_gattc_backoff_stage : (CEEPEW_REV_GATTC_BACKOFF_STAGES - 1U);
    uint32_t backoff_ms = s_rev_gattc_backoff_ms[stage];
    if (s_rev_gattc_last_attempt_ms != 0U &&
        (now_ms - s_rev_gattc_last_attempt_ms) < backoff_ms) {
      return CEEPEW_OK;  /* too soon — retry on next loop iteration */
    }
    if (g_ble_ctx.peer_mac[0] != 0U) {
      ESP_LOGI(TAG, "Responder: opening reverse GATTC for sign_pk exchange");
      s_rev_gattc_last_attempt_ms = now_ms;
      CeePewErr_t conn_err = transport_ble_connect_to_peer(g_ble_ctx.peer_mac);
      ESP_LOGI(TAG,
               "Reverse GATTC connect result: err=%d "
               "rev_pend->%d attempts=%u",
               (int)conn_err, g_ble_ctx.reverse_gattc_pending,
               g_ble_ctx.reconnect_attempts);
      if (conn_err == CEEPEW_ERR_BUSY) {
        /* Connection already in progress — wait for completion */
        ESP_LOGI(TAG, "reverse GATTC: connection in progress, waiting");
      } else if (conn_err != CEEPEW_OK) {
        g_ble_ctx.reconnect_attempts++;
        /* Increment backoff stage on failure (capped at max stages) */
        if (s_rev_gattc_backoff_stage < CEEPEW_REV_GATTC_BACKOFF_STAGES - 1U) {
          s_rev_gattc_backoff_stage++;
        }
        if (g_ble_ctx.reconnect_attempts >= CEEPEW_MAX_RECONNECT_ATTEMPTS) {
          ESP_LOGW(TAG, "reverse GATTC: max retries (%u) exhausted — clearing rev_pend",
                   (unsigned)CEEPEW_MAX_RECONNECT_ATTEMPTS);
          g_ble_ctx.reverse_gattc_pending = false;
          s_rev_gattc_last_attempt_ms = 0U;
          s_rev_gattc_backoff_stage = 0U;
        } else {
          ESP_LOGW(TAG, "reverse GATT connect failed: %d — will retry (attempt %u, backoff stage %u, %u ms)",
                   (int)conn_err, (unsigned)g_ble_ctx.reconnect_attempts,
                   (unsigned)s_rev_gattc_backoff_stage,
                   (unsigned)s_rev_gattc_backoff_ms[s_rev_gattc_backoff_stage]);
        }
      } else {
        /* Success — connection initiated */
        s_rev_gattc_last_attempt_ms = 0U;
        s_rev_gattc_backoff_stage = 0U;
      }
    } else {
      ESP_LOGW(TAG, "Responder: no peer addr for reverse GATTC");
      g_ble_ctx.reverse_gattc_pending = false;
    }
  }

  /* ── STEP 6: Key derivation gate (commitment matched + sign_pk ok) ── */
  phase = session_get_phase();
  ble_state = transport_ble_get_state();

  /* Ensure sign_pk exchange has actually completed before deriving.
   * Both sides must have received the peer's keys (sign_pk_received).
   * The responder must additionally have SENT its keys back to the
   * initiator (initiator_sign_pk_sent). If the responder sent but the
   * initiator never received, both must wait for retries to exhaust.
   * All g_ble_ctx fields are read under the lock for multi-field
   * consistency — the supervisor on Core 0 can clear the context
   * between successive field reads on Core 1. */
  bool sign_pk_exchange_complete;
  transport_ble_ctx_lock();
  bool sr = g_ble_ctx.sign_pk_received;
  bool iss = g_ble_ctx.initiator_sign_pk_sent;
  uint8_t ra = g_ble_ctx.reconnect_attempts;
  transport_ble_ctx_unlock();
  sign_pk_exchange_complete = sr
      && (!is_initiator ? iss : true)
      && ra < CEEPEW_MAX_RECONNECT_ATTEMPTS;

  if (phase == 2U && transport_ble_handoff_ready() && sign_pk_exchange_complete) {

    /* Jittered backoff to reduce simultaneous-derive timing collision. */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (s_phase2_jitter_target_ms == 0U) {
      uint8_t local_dev[CEEPEW_DEVICE_ID_BYTES];
      (void)session_get_device_id(local_dev);
      uint32_t seed = now_ms ^ ((uint32_t)local_dev[5] << 8);
      if (peer != NULL) {
        seed ^=
            ((uint32_t)peer->peer_mac[5] << 16) | (uint32_t)peer->peer_mac[4];
      }
      /* jitter in [50, 500) ms */
      s_phase2_jitter_target_ms = 50U + (uint32_t)(seed % 450U);
      s_phase2_jitter_start_ms = now_ms;
      ceepew_secure_zero(local_dev, sizeof(local_dev));
      ESP_LOGI(TAG, "phase2: jitter target %u ms",
               (unsigned)s_phase2_jitter_target_ms);
    }
    if ((now_ms - s_phase2_jitter_start_ms) < s_phase2_jitter_target_ms) {
      return CEEPEW_OK;
    }
    s_phase2_jitter_target_ms = 0U;

    /* If sign_pk is still missing after retries, log a warning and
     * proceed. The first chat frame's signature will be unverifiable
     * until the peer's sign_pk is delivered via a later frame. */
    if (!g_ble_ctx.sign_pk_received) {
      ESP_LOGW(TAG,
               "sign_pk exchange gave up (attempts=%u, timeout=%u ms, "
               "init_sent=%d rev_pend=%d) — proceeding without it",
               (unsigned)g_ble_ctx.reconnect_attempts,
               (unsigned)CEEPEW_SIGN_PK_GATE_TIMEOUT_MS,
               g_ble_ctx.initiator_sign_pk_sent,
               g_ble_ctx.reverse_gattc_pending);
    }

    /* session_set_role() was already called at STEP 2. Re-set here
     * to ensure nonce parity is locked in right before derivation. */
    (void)session_set_role(is_initiator);

    /* ── STRICT HARDWARE-GATED IDENTITY HANDOFF ──────────────────────
     * The peer's WiFi STA MAC must have been received and validated
     * over the secure GATT channel (0xFFF3 characteristic) BEFORE
     * key derivation proceeds. If the WiFi MAC is missing or not yet
     * delivered, the GATT identity exchange is incomplete and key
     * derivation MUST be blocked — proceeding would bind session keys
     * to an unauthenticated transport identity. */
    if (!session_peer_wifi_mac_valid()) {
      ESP_LOGW(TAG, "STEP 6: peer WiFi MAC not yet received over GATT — "
                    "deferring key derivation");
      return CEEPEW_OK;
    }

    CeePewErr_t err = session_phase2_derive_key();
    if (err != CEEPEW_OK) {
      return err;
    }

    g_ble_ctx.accumulated_conn_ms = 0U;
    s_ble_commitment_exchanged = false;

    /* HANDOFF SYNC: Both peers must send HANDOFF_READY beacon and receive
     * the peer's beacon before tearing down BLE. This prevents race where
     * one side tears down BLE before the other has confirmed the PIN. */
    if (g_ble_ctx.ready_for_chat && !g_ble_ctx.peer_ready_for_chat) {
        /* Send HANDOFF_READY beacon if we're ready but haven't received peer's yet */
        (void)transport_ble_send_handoff_ready_beacon();
        ESP_LOGI(TAG, "Sent HANDOFF_READY beacon, waiting for peer...");
    }

    /* Wait for peer's HANDOFF_READY beacon with timeout */
    const uint32_t handoff_sync_timeout_ms = 5000U;
    uint32_t handoff_start_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    while (!g_ble_ctx.peer_ready_for_chat) {
        now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        if ((now_ms - handoff_start_ms) > handoff_sync_timeout_ms) {
            ESP_LOGW(TAG, "HANDOFF_READY sync timeout — peer not ready");
            break;  /* Proceed anyway, peer may have already moved on */
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Both sides ready (or timeout) — send final beacon and proceed */
    if (g_ble_ctx.peer_ready_for_chat) {
        ESP_LOGI(TAG, "HANDOFF_READY sync complete — both peers ready");
    }
    (void)transport_ble_send_handoff_ready_beacon();  /* Final beacon */

    /* Diagnostic: log peer WiFi MAC before BLE teardown */
    #ifdef CEEPEW_DEBUG_SERIAL
    uint8_t debug_wifi_mac[6] = {0};
    if (session_peer_wifi_mac_valid()) {
        (void)session_get_peer_wifi_mac(debug_wifi_mac);
        ESP_LOGI(TAG, "BLE teardown starting, peer_wifi_mac=%02X:%02X:%02X:%02X:%02X:%02X",
            debug_wifi_mac[0], debug_wifi_mac[1], debug_wifi_mac[2],
            debug_wifi_mac[3], debug_wifi_mac[4], debug_wifi_mac[5]);
    }
    #endif

    /* Strict BLE teardown for Phase 2->3 transition:
     * 1. Disconnect any active GATT links cleanly via esp_ble_gatts_close() and esp_ble_gattc_close()
     * 2. De-register and disable Bluedroid stack fully using esp_bluedroid_disable() and esp_bluedroid_deinit()
     * 3. Disable and deinitialize the BT controller with esp_bt_controller_disable() and esp_bt_controller_deinit()
     * 4. Release all Bluetooth hardware baseband memory back to the system heap using esp_bt_controller_mem_release(ESP_BT_MODE_BLE)
     * 5. Inject mandatory vTaskDelay(pdMS_TO_TICKS(150)) block for radio PHY settle before hal_radio_init() */
    (void)transport_ble_teardown();

    /* Reclaim full RF for WiFi/ESP-NOW after BLE teardown.
     * BT controller and Bluedroid are fully deinitialized, so coexistence arbiter
     * no longer allocates RF time slices to BLE, giving WiFi 100% of the RF. */
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BLE, 0x3FU);

#if CONFIG_CEEPEW_WIFI_FULL_RESTART
    /* Full WiFi stop+start — forces coexistence hardware re-query.
     * The BT controller remains alive (to avoid IWDT crash), so the
     * coex arbiter still reserves RF for BLE despite the status bit clear.
     * A full WiFi stop+start forces the driver to re-query coex state
     * with BLE bits now zeroed, giving WiFi 100% of the RF. */
    COEX_LOG("esp_wifi_stop()");
    (void)esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    COEX_LOG("esp_wifi_start()");
    (void)esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(100));
    /* ESP-NOW state is destroyed by esp_wifi_stop(). Re-init it. */
    COEX_LOG("hal_radio_espnow_reinit()");
    (void)hal_radio_espnow_reinit();
#else
    /* Lightweight: restart coex scheduler without full WiFi restart.
     * Uses internal coex_schm_process_restart() to reset scheduler state.
     * Experimental — may not fully reset hardware coex state. */
    COEX_LOG("coex_schm_process_restart()");
    extern int coex_schm_process_restart(void);
    (void)coex_schm_process_restart();
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    /* Ensure WiFi is fully powered for ESP-NOW post-derive sync.
     * hal_radio_init() is idempotent — safe to call when already up.
     * Re-assert the WiFi channel in case BLE coexistence drifted it. */
    #ifdef CEEPEW_DEBUG_SERIAL
    ESP_LOGI(TAG, "Calling hal_radio_init()");
    #endif
    COEX_LOG("hal_radio_init()");
    (void)hal_radio_init();
    /* Re-register the ESP-NOW receive callback. coex_schm_process_restart()
     * does NOT fully reset ESP-NOW, but BLE's Wi-Fi coexistence hooks may
     * transiently suppress the recv callback. This is an idempotent no-op
     * if the callback is already live. */
    hal_radio_reregister_recv_cb();
    (void)hal_radio_set_power_save(WIFI_PS_NONE);
    (void)hal_radio_set_channel(CEEPEW_ESPNOW_CHANNEL);
    #ifdef CEEPEW_DEBUG_SERIAL
    ESP_LOGI(TAG, "ESP-NOW initialized on channel %d", CEEPEW_ESPNOW_CHANNEL);
    #endif
    vTaskDelay(pdMS_TO_TICKS(100));  /* 100 ms for WiFi/coex stack to settle (was 200) */

    /* Quick broadcast probe to verify ESP-NOW RX is live after BLE teardown */
    {
        uint8_t probe_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (hal_radio_is_peer_registered(probe_mac)) {
            esp_now_del_peer(probe_mac);
        }
        esp_now_peer_info_t probe_peer;
        memset(&probe_peer, 0, sizeof(probe_peer));
        memcpy(probe_peer.peer_addr, probe_mac, 6);
        probe_peer.channel = (uint8_t)CEEPEW_ESPNOW_CHANNEL;
        probe_peer.ifidx = WIFI_IF_STA;
        probe_peer.encrypt = false;
        esp_now_add_peer(&probe_peer);
        uint8_t probe_data[] = {0xBB, 0xBB};
        esp_now_send(probe_mac, probe_data, sizeof(probe_data));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_now_del_peer(probe_mac);
    }

    /* Initialize rendezvous state before peer registration.
     * The rendezvous handshake runs on the static CEEPEW_ESPNOW_CHANNEL
     * and must complete before channel hopping starts. */
    CeePewErr_t rv_init_err = session_rendezvous_init();
    if (rv_init_err != CEEPEW_OK) {
        ESP_LOGE(TAG, "Rendezvous init failed: %d", (int)rv_init_err);
        return rv_init_err;
    }

    /* Register the ESP-NOW peer ONCE after key derivation.
     * Use the peer's WiFi STA MAC (received via GATT sign_pk exchange) for ESP-NOW.
     * Fallback to BLE MAC if WiFi MAC not yet available (should not happen after fix). */
    COEX_LOG("Starting peer registration");
    uint8_t peer_mac[6];
    CeePewErr_t peer_err;
    if (session_peer_wifi_mac_valid()) {
        peer_err = session_get_peer_wifi_mac(peer_mac);
        if (peer_err == CEEPEW_OK) {
            ESP_LOGI(TAG, "Using peer WiFi MAC for ESP-NOW: %02X:%02X:%02X:%02X:%02X:%02X",
                     peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
        }
    } else {
        ESP_LOGW(TAG, "Peer WiFi MAC not available — falling back to BLE MAC");
        peer_err = session_get_peer_device_id(peer_mac);
    }
    if (peer_err != CEEPEW_OK) {
        return peer_err;
    }

    /* Derive ESP-NOW Local Master Key (LMK) for this peer.
     * LMK is derived from session key + peer WiFi MAC. */
    uint8_t lmk[16];
    peer_err = crypto_espnow_derive_lmk(peer_mac, lmk);
    if (peer_err != CEEPEW_OK) {
        ESP_LOGE(TAG, "Failed to derive ESP-NOW LMK: %d", (int)peer_err);
        return peer_err;
    }

    /* Retry peer registration with verification — ESP-NOW peer add can
     * fail transiently due to WiFi/BLE coexistence or timing.
     * Retry up to 3 times with increasing delays. */
    const uint32_t peer_retry_delays_ms[] = {50U, 150U, 300U};
    for (uint8_t attempt = 0; attempt < 3U; attempt++) {
        COEX_LOG("hal_radio_set_peer_with_lmk() attempt %u", (unsigned)attempt + 1U);
        peer_err = hal_radio_set_peer_with_lmk(peer_mac, lmk);
        if (peer_err == CEEPEW_OK) {
            /* Verify the peer was actually added to the ESP-NOW peer list */
            vTaskDelay(pdMS_TO_TICKS(peer_retry_delays_ms[attempt]));
            if (hal_radio_is_peer_registered(peer_mac)) {
                COEX_LOG("Peer registered and verified (attempt %u)", (unsigned)attempt + 1U);
                #ifdef CEEPEW_DEBUG_SERIAL
                ESP_LOGI(TAG, "ESP-NOW peer registered: %02X:%02X:%02X:%02X:%02X:%02X",
                    peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
                #endif
                break;
            }
            ESP_LOGW(TAG, "ESP-NOW peer registration reported OK but not verified (attempt %u)", (unsigned)attempt + 1U);
        } else {
            ESP_LOGW(TAG, "ESP-NOW peer registration failed (attempt %u): %d", (unsigned)attempt + 1U, (int)peer_err);
        }
        if (attempt < 2U) {
            vTaskDelay(pdMS_TO_TICKS(peer_retry_delays_ms[attempt]));
        }
    }
    ceepew_secure_zero(lmk, sizeof(lmk));
    if (peer_err != CEEPEW_OK) {
        ESP_LOGE(TAG, "ESP-NOW peer registration failed after 3 attempts");
        return peer_err;
    }
    /* Final verification and extended settle time for WiFi stack */
    if (!hal_radio_is_peer_registered(peer_mac)) {
        ESP_LOGE(TAG, "ESP-NOW peer not registered after retries — aborting");
        return CEEPEW_ERR_HW;
    }
    vTaskDelay(pdMS_TO_TICKS(200));  /* Additional 200ms for WiFi stack to fully settle */
    COEX_LOG("ESP-NOW peer ready for post-derive sync");

    /* ─── PFS (Perfect Forward Secrecy) Handshake ───────────────────────
     * Now that ESP-NOW link is up with LMK, perform ephemeral ECDH over ESP-NOW.
     * The BLE-derived session key is ONLY used as the ESP-NOW LMK.
     * The PFS handshake derives a fresh Ascon-128 key for the chat session. */
    if (is_initiator) {
        /* Initiator: generate PFS keypair and send PFS_INIT to peer */
        CeePewErr_t pfs_err = session_pfs_initiate();
        if (pfs_err == CEEPEW_OK) {
            uint8_t local_pfs_pubkey[32U];
            if (session_get_local_pfs_pubkey(local_pfs_pubkey) == CEEPEW_OK) {
                uint8_t pfs_frame[CEEPEW_PACKET_MAX_BYTES];
                uint16_t pfs_len = 0U;
                if (transport_esl_build_pfs_handshake(pfs_frame, &pfs_len, sizeof(pfs_frame),
                                                      local_pfs_pubkey, true) == CEEPEW_OK) {
                    (void)hal_radio_send_broadcast(pfs_frame, pfs_len);
                    ESP_LOGI(TAG, "Sent PFS_INIT to peer, waiting for PFS_RESP");
                }
            }
        } else {
            ESP_LOGW(TAG, "PFS initiate failed: %d", (int)pfs_err);
        }
    } else {
        /* Responder: generate PFS keypair, wait for PFS_INIT from initiator */
        CeePewErr_t pfs_err = session_pfs_initiate();
        if (pfs_err != CEEPEW_OK) {
            ESP_LOGW(TAG, "PFS initiate (responder) failed: %d", (int)pfs_err);
        }
    }

    /* Post-derive sync barrier: do NOT advance the UI to CRYPTOGRAM until
     * an encrypted MSG_TYPE_KEY_ACK round-trip has been verified. This
     * prevents state desync where one device shows "✓ SECURE" while the
     * other is still in PAIRING because key derivation completed locally
     * but the peer never confirmed. The actual ACK exchange is driven by
     * task_session_drive_post_derive_sync() on the session task's main loop. */
    ESP_LOGI(TAG, "Session key derived — entering post-derive sync (role=%s)",
             is_initiator ? "initiator" : "responder");

    session_ui_ctx_lock();
    (void)ui_manager_transition_to(UI_STATE_KEYDER);
    g_ui_ctx.transition_ready = true;
    session_ui_ctx_unlock();

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

static const char *task_session_phase_name(uint8_t phase) {
  switch (phase) {
  case 0U:
    return "idle";
  case 1U:
    return "discovery";
  case 2U:
    return "pairing";
  case 3U:
    return "active";
  default:
    return "unknown";
  }
}

static const char *task_session_ui_name(UIState_t state) {
  switch (state) {
  case UI_STATE_BOOT:
    return "boot";
  case UI_STATE_DISCOVERY:
    return "discovery";
  case UI_STATE_CODE_ENTRY:
    return "code_entry";
  case UI_STATE_COUNTDOWN:
    return "countdown";
  case UI_STATE_CONFIRM:
    return "confirm";
  case UI_STATE_KEYDER:
    return "keyder";
  case UI_STATE_CHAT:
    return "chat";
  case UI_STATE_CHAT_MENU:
    return "chat_menu";
  case UI_STATE_CHAT_COMPOSE:
    return "compose";
  case UI_STATE_CHAT_SEND_CONFIRM:
    return "send_confirm";
  case UI_STATE_CRYPTOGRAM:
    return "cryptogram";
  case UI_STATE_NONCE_EXHAUSTED:
    return "nonce_exhausted";
  case UI_STATE_INFO:
    return "info";
  case UI_STATE_ERROR:
    return "error";
  case UI_STATE_PAIRING:
    return "pairing";
  case UI_STATE_PAIRING_FAILED:
    return "pair_fail";
  default:
    return "unknown";
  }
}

static const char *task_session_rgb_name(RgbPattern_t pattern) {
  switch (pattern) {
  case RGB_OFF:
    return "off";
  case RGB_RED:
    return "red";
  case RGB_GREEN:
    return "green";
  case RGB_BLUE:
    return "blue";
  case RGB_YELLOW:
    return "yellow";
  case RGB_CYAN:
    return "cyan";
  case RGB_MAGENTA:
    return "magenta";
  case RGB_WHITE:
    return "white";
  case RGB_RED_BLINK:
    return "red_blink";
  case RGB_GREEN_BLINK:
    return "green_blink";
  case RGB_BLUE_BLINK:
    return "blue_blink";
  case RGB_WHITE_PULSE:
    return "white_pulse";
  case RGB_BLUE_PULSE:
    return "blue_pulse";
  case RGB_GREEN_PULSE:
    return "green_pulse";
  case RGB_AMBER_PULSE:
    return "amber_pulse";
  case RGB_CYAN_PULSE:
    return "cyan_pulse";
  case RGB_YELLOW_RED_BLINK:
    return "yellow_red_blink";
  case RGB_RAINBOW_CYCLE:
    return "rainbow";
  case RGB_HEARTBEAT:
    return "heartbeat";
  default:
    return "unknown";
  }
}

static RgbPattern_t task_session_map_pattern(UIState_t state,
                                             bool session_active) {
  switch (state) {
  case UI_STATE_BOOT:
    return RGB_WHITE_PULSE; /* One-shot white breathe during boot */
  case UI_STATE_DISCOVERY:
    return RGB_BLUE_PULSE; /* Smooth blue pulse during scanning */
  case UI_STATE_CODE_ENTRY:
  case UI_STATE_COUNTDOWN:
  case UI_STATE_CONFIRM:
    return RGB_AMBER_PULSE; /* Smooth amber pulse for user input */
  case UI_STATE_KEYDER:
    return RGB_YELLOW; /* Solid yellow during key derivation */
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
    return RGB_RED_BLINK; /* Red blink for errors */
  default:
    return RGB_OFF;
  }
}

/* Reset task-local static state after pairing failure.
 * Ensures consistency with FSM phase after session_reset_to_discovery(). */
static void task_session_reset_pairing_static_state(void) {
  s_ble_peer_latched = false;
  s_ble_commitment_exchanged = false;
  s_phase2_jitter_target_ms = 0U;
  s_phase2_jitter_start_ms = 0U;
  s_initiator_jittered = false;
  s_rev_gattc_backoff_stage = 0U;
  s_sign_pk_gate_start_ms = 0U;
  s_m2_gate_wait_start_ms = 0U;
}

/* Pairing flow ownership — session task drives the pairing state machine */
static void task_session_pairing_flow_reset(void) {
  s_pairing_flow_state = PAIRING_FLOW_IDLE;
  s_pairing_flow_start_ms = 0U;
  s_pairing_phase_entered_ms = 0U;
  s_pairing_retry_count = 0U;
  s_ble_peer_latched = false; /* Force re-latch on next pairing attempt */
  s_initiator_jittered = false;
  s_rev_gattc_backoff_stage = 0U;
  s_sign_pk_gate_start_ms = 0U;
  s_m2_gate_wait_start_ms = 0U;
}

static void task_session_pairing_flow_advance(PairingFlowState_t new_state) {
  if (s_pairing_flow_state != new_state) {
    ESP_LOGI(TAG, "pairing flow: %d -> %d", s_pairing_flow_state, new_state);
    s_pairing_flow_state = new_state;
    s_pairing_phase_entered_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
  }
}

static CeePewErr_t task_session_pairing_flow_drive(void) {
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
  uint8_t phase = session_get_phase();
  const BlePeerRecord_t *peer = transport_ble_get_peer_cached();
  UIState_t ui_state;
  session_ui_get_state_snapshot(&ui_state);

  /* Timeout budgets per pairing flow state (ms) */
  static const uint32_t pairing_flow_timeout_ms[] = {
      0,     /* IDLE */
      30000, /* DISCOVERY - 30s */
      15000, /* COMMITMENT - 15s */
      45000, /* GATT_IDENTITY - 45s (matches supervisor CEEPEW_PHASE_TIMEOUT_GATT_MS) */
       35000, /* KEY_DERIVE - 35s (reverse GATTC + derivation + sync; must exceed UI KEYDER safety) */
      10000, /* POST_DERIVE_SYNC - 10s */
      5000,  /* FAILED - 5s cleanup */
  };

  switch (s_pairing_flow_state) {
  case PAIRING_FLOW_IDLE:
    if (phase == 1U) {
      if (!transport_ble_is_initialised()) {
        (void)transport_ble_restart_discovery_session();
      }
      if (peer != NULL) {
        task_session_pairing_flow_advance(PAIRING_FLOW_DISCOVERY);
        s_pairing_flow_start_ms = now_ms;
      }
    }
    break;

  case PAIRING_FLOW_DISCOVERY:
    if (phase >= 2U) {
      task_session_pairing_flow_advance(PAIRING_FLOW_COMMITMENT);
    } else if (peer == NULL) {
      task_session_pairing_flow_advance(PAIRING_FLOW_IDLE);
    } else if ((now_ms - s_pairing_phase_entered_ms) >
               pairing_flow_timeout_ms[PAIRING_FLOW_DISCOVERY]) {
      ESP_LOGW(TAG, "pairing flow: DISCOVERY timeout");
      task_session_pairing_flow_advance(PAIRING_FLOW_FAILED);
    }
    break;

  case PAIRING_FLOW_COMMITMENT:
    if (transport_ble_handoff_ready()) {
      task_session_pairing_flow_advance(PAIRING_FLOW_GATT_IDENTITY);
    } else if (phase < 2U) {
      task_session_pairing_flow_advance(PAIRING_FLOW_DISCOVERY);
    } else if ((now_ms - s_pairing_phase_entered_ms) >
               pairing_flow_timeout_ms[PAIRING_FLOW_COMMITMENT]) {
      ESP_LOGW(TAG, "pairing flow: COMMITMENT timeout");
      task_session_pairing_flow_advance(PAIRING_FLOW_FAILED);
    }
    break;

  case PAIRING_FLOW_GATT_IDENTITY:
    if (g_ble_ctx.sign_pk_received) {
      ESP_LOGI(TAG,
               "pairing flow: GATT_IDENTITY -> KEY_DERIVE (sign_pk received)");
      task_session_pairing_flow_advance(PAIRING_FLOW_KEY_DERIVE);
    } else if (g_ble_ctx.reconnect_attempts >= CEEPEW_MAX_RECONNECT_ATTEMPTS) {
      ESP_LOGW(TAG,
               "pairing flow: GATT_IDENTITY -> KEY_DERIVE (retries exhausted, "
               "sign_pk MISSING, rev_pend=%d init_sent=%d)",
               g_ble_ctx.reverse_gattc_pending,
               g_ble_ctx.initiator_sign_pk_sent);
      task_session_pairing_flow_advance(PAIRING_FLOW_KEY_DERIVE);
    } else if (phase < 2U) {
      task_session_pairing_flow_advance(PAIRING_FLOW_DISCOVERY);
    } else if ((now_ms - s_pairing_phase_entered_ms) >
               pairing_flow_timeout_ms[PAIRING_FLOW_GATT_IDENTITY]) {
      ESP_LOGW(TAG, "pairing flow: GATT_IDENTITY timeout");
      task_session_pairing_flow_advance(PAIRING_FLOW_FAILED);
    }
    break;

  case PAIRING_FLOW_KEY_DERIVE:
    if (session_is_active() && session_sync_barrier_cleared()) {
      task_session_pairing_flow_advance(PAIRING_FLOW_POST_DERIVE_SYNC);
    } else if ((now_ms - s_pairing_phase_entered_ms) >
               pairing_flow_timeout_ms[PAIRING_FLOW_KEY_DERIVE]) {
      ESP_LOGW(TAG, "pairing flow: KEY_DERIVE timeout");
      task_session_pairing_flow_advance(PAIRING_FLOW_FAILED);
    }
    /* NOTE: Do NOT check phase < 3U here.  STEP 6 (key derivation) runs
     * AFTER this function returns, in task_session_drive_ble_pairing().
     * Checking phase < 3U on the next tick would prematurely kill the
     * pairing flow before STEP 6 gets a chance to derive the key. */
    break;

  case PAIRING_FLOW_POST_DERIVE_SYNC:
    if (session_sync_barrier_cleared()) {
      task_session_pairing_flow_reset();
    } else if ((now_ms - s_pairing_phase_entered_ms) >
               pairing_flow_timeout_ms[PAIRING_FLOW_POST_DERIVE_SYNC]) {
      ESP_LOGW(TAG, "pairing flow: POST_DERIVE_SYNC timeout");
      task_session_pairing_flow_advance(PAIRING_FLOW_FAILED);
    }
    break;

  case PAIRING_FLOW_FAILED:
    if ((now_ms - s_pairing_phase_entered_ms) >
        pairing_flow_timeout_ms[PAIRING_FLOW_FAILED]) {
      ESP_LOGI(TAG, "pairing flow: cleanup complete, returning to discovery");
      (void)transport_ble_restart_discovery_session();
      task_session_reset_pairing_static_state();
      task_session_pairing_flow_reset();
    }
    break;

  default:
    break;
  }

  return CEEPEW_OK;
}

CeePewErr_t task_session_sync_visual_state(void) {
  uint8_t phase = session_get_phase();
  bool active = session_is_active();
  BleState_t ble_state = transport_ble_get_state();
  uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
  const BlePeerRecord_t *peer = transport_ble_get_peer();
  bool discovered = (peer != NULL);
  uint8_t ble_hits = g_ble_ctx.scan_hit_count;
  int8_t ble_rssi = g_ble_ctx.peer_rssi;
  bool scan_active =
      (ble_state == BLE_SCANNING || ble_state == BLE_ADVERTISING_AND_SCANNING);

  /* Phase 2.C1: snapshot current_state once for the two read sites below. */
  UIState_t ui_state;
  session_ui_get_state_snapshot(&ui_state);

  /* Phase 3 (ACTIVE) — once the session is live, nudge any pre-chat UI
   * into the secure-chat derivation path without disturbing later states. */
  if (phase == 3U && active) {
    UIState_t current = ui_state;
    bool pre_chat =
        (current == UI_STATE_DISCOVERY || current == UI_STATE_COUNTDOWN ||
         current == UI_STATE_PAIRING || current == UI_STATE_CODE_ENTRY ||
         current == UI_STATE_CONFIRM || current == UI_STATE_BOOT);

    if (pre_chat) {
      ESP_LOGI(TAG, "sync: session active but UI on %u — forcing KEYDER",
               (unsigned)current);
      session_ui_ctx_lock();
      (void)ui_manager_transition_to(UI_STATE_KEYDER);
      g_ui_ctx.transition_ready = true;
      session_ui_ctx_unlock();
    }
  }

  /* (ui_state already snapshotted above for the pre-chat check.) */
  RgbPattern_t pattern = task_session_map_pattern(ui_state, active);

  /* Track discovery scan start time so we can enforce a safe timeout */
  if (scan_active) {
    if (s_ble_scan_start_ms == 0ULL) {
      s_ble_scan_start_ms = now_ms;
    }
  } else {
    s_ble_scan_start_ms = 0ULL;
  }

  bool changed =
      (!s_visual_state_ready || phase != s_last_session_phase ||
       active != s_last_session_active || ui_state != s_last_ui_state ||
       pattern != s_last_rgb_pattern || ble_state != s_last_ble_state ||
       discovered != s_last_ble_discovered || ble_hits != s_last_ble_hits ||
       ble_rssi != s_last_ble_rssi);

  if (changed) {
    char peer_mac[18] = "<none>";
    char peer_name[17] = "<none>";
    if (peer != NULL) {
      task_session_format_mac(peer->peer_mac, peer_mac);
      if (g_ble_ctx.peer_record.name_len > 0U) {
        uint8_t name_len = g_ble_ctx.peer_record.name_len;
        if (name_len > 16U) {
          name_len = 16U;
        }
        memcpy(peer_name, g_ble_ctx.peer_record.name, name_len);
        peer_name[name_len] = '\0';
      }
    }

    ESP_LOGI(
        TAG,
        "[%llu ms] phase=%s ui=%s active=%u rgb=%s ble=%s adv=%u scan=%u "
        "discovered=%u hits=%u peer=%s rssi=%d name=%s conn=%u/%u",
        (unsigned long long)now_ms, task_session_phase_name(phase),
        task_session_ui_name(ui_state), active ? 1U : 0U,
        task_session_rgb_name(pattern), task_session_ble_state_name(ble_state),
        g_ble_ctx.is_advertising ? 1U : 0U, g_ble_ctx.is_scanning ? 1U : 0U,
        discovered ? 1U : 0U, (unsigned)ble_hits, peer_mac, (int)ble_rssi,
        peer_name, g_ble_ctx.gattc_connected ? 1U : 0U,
        g_ble_ctx.gatts_connected ? 1U : 0U);
  }

  if (scan_active && (s_last_ble_scan_heartbeat_ms == 0ULL ||
                      (now_ms - s_last_ble_scan_heartbeat_ms) >= 2000ULL)) {
    char peer_mac[18] = "<none>";
    if (peer != NULL) {
      task_session_format_mac(peer->peer_mac, peer_mac);
    }

    ESP_LOGI(TAG,
             "scan heartbeat: state=%s seen=%lu peer_hits=%u peer=%s rssi=%d "
             "adv=%u scan=%u",
             task_session_ble_state_name(ble_state),
             (unsigned long)g_ble_ctx.scan_seen_count, (unsigned)ble_hits,
             peer_mac, (int)ble_rssi, g_ble_ctx.is_advertising ? 1U : 0U,
             g_ble_ctx.is_scanning ? 1U : 0U);
    /* Detect stuck scanning: if we have been scanning but have not
     * seen a discovered peer for >60s, restart discovery. Use the
     * scan start time for comparison to avoid false positives on
     * initial heartbeats. */
    s_last_ble_scan_heartbeat_ms = now_ms;
    if (s_ble_scan_start_ms != 0ULL && g_ble_ctx.is_scanning && !g_ble_ctx.discovered &&
        (now_ms - s_ble_scan_start_ms) > 60000U) {
      ESP_LOGW(
          TAG,
          "Scan appears stuck (>60s without discovery) — restarting discovery");
      (void)transport_ble_restart_discovery_session();
      /* Refresh scan start time after restart */
      s_ble_scan_start_ms = now_ms;
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
static CeePewErr_t process_rx_frame(const RadioFrame_t *frame) {
  CEEPEW_ASSERT(frame != NULL, CEEPEW_ERR_NULL_PTR);
  CEEPEW_ASSERT(frame->payload_len > 0U &&
                    frame->payload_len <= CEEPEW_PACKET_MAX_BYTES,
                CEEPEW_ERR_BOUNDS);

  #ifdef CEEPEW_DEBUG_SERIAL
  ESP_LOGI("SESSION-RX", "process_rx_frame: src=%02X:%02X:%02X:%02X:%02X:%02X len=%u active=%d",
           frame->src_mac[0], frame->src_mac[1], frame->src_mac[2],
           frame->src_mac[3], frame->src_mac[4], frame->src_mac[5],
           (unsigned)frame->payload_len, session_is_active());
  #endif

  /* Handle rendezvous frames BEFORE session_is_active() check.
   * Rendezvous runs on static channel after key derivation but before
   * channel hopping starts. These are plaintext frames with msg type
   * CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_REQ (0x03) or RENDEZVOUS_ACK (0x04). */
  if (frame->payload_len >= 1U) {
    uint8_t msg_type = frame->payload[0];
    if (msg_type == CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_REQ ||
        msg_type == CEEPEW_ESL_MSG_TYPE_RENDEZVOUS_ACK) {
      CeePewErr_t rv_err = hal_radio_rendezvous_handle_rx(frame->payload, frame->payload_len, frame->src_mac);
      if (rv_err == CEEPEW_OK) {
        /* Rendezvous frame handled successfully */
        return CEEPEW_OK;
      }
      /* If not a rendezvous frame or error, fall through */
    }
  }

  if (!session_is_active()) {
    return CEEPEW_OK;
  }

  uint8_t local_work_frame[CEEPEW_PACKET_MAX_BYTES];
  uint8_t local_fec_out[CEEPEW_FEC_BUF_MAX];
  uint8_t local_box_ct[CEEPEW_MAX_MSG_BYTES + 64U];
  uint8_t local_ascon_ct[CEEPEW_MAX_MSG_BYTES + CEEPEW_ASCON_TAG_BYTES];
  uint8_t local_plain[CEEPEW_MAX_MSG_BYTES];
  uint8_t local_nonce[24U];
  uint8_t local_sign_pk[32U];
  uint8_t local_box_pk[32U];
  uint8_t local_ascon_key[16U];
  uint8_t local_sig[64U];

  CeePewErr_t err = CEEPEW_OK;
  uint16_t work_len = frame->payload_len;
  memcpy(local_work_frame, frame->payload, frame->payload_len);

  /* Peek frame type before ESL processing — ESL strips the header on success,
   * making type inspection impossible afterward. PFS handshake frames bypass
   * the normal crypto pipeline entirely. */
  uint8_t msg_type = 0xFFU;
  CeePewErr_t peek_err = transport_esl_peek_msg_type(local_work_frame, frame->payload_len, &msg_type);
  bool is_pfs = (peek_err == CEEPEW_OK &&
                 (msg_type == CEEPEW_ESL_MSG_TYPE_PFS_INIT ||
                  msg_type == CEEPEW_ESL_MSG_TYPE_PFS_RESP));

  if (is_pfs) {
    uint8_t peer_pfs_pubkey[32U];
    CeePewErr_t pfs_err = transport_esl_process_pfs_handshake(local_work_frame, frame->payload_len,
                                                                frame->src_mac, peer_pfs_pubkey);
    if (pfs_err == CEEPEW_OK) {
      CeePewErr_t derive_err = session_pfs_process_peer_key(peer_pfs_pubkey);
      if (derive_err == CEEPEW_OK) {
        if (msg_type == CEEPEW_ESL_MSG_TYPE_PFS_INIT) {
          /* Responder: send PFS_RESP back to initiator with our PFS public key */
          uint8_t local_pfs_pubkey[32U];
          if (session_get_local_pfs_pubkey(local_pfs_pubkey) == CEEPEW_OK) {
            uint8_t pfs_frame[CEEPEW_PACKET_MAX_BYTES];
            uint16_t pfs_len = 0U;
            if (transport_esl_build_pfs_handshake(pfs_frame, &pfs_len, sizeof(pfs_frame),
                                                  local_pfs_pubkey, false) == CEEPEW_OK) {
              (void)hal_radio_send_broadcast(pfs_frame, pfs_len);
              ESP_LOGI("SESSION", "Sent PFS_RESP to peer");
            }
          }
        }
      }
      ceepew_secure_zero(peer_pfs_pubkey, sizeof(peer_pfs_pubkey));
    }
    goto rx_cleanup;
  }

  uint32_t rx_queue_depth = 0U;
  if (g_session_rx_queue != NULL) {
    rx_queue_depth = (uint32_t)uxQueueMessagesWaiting(g_session_rx_queue);
  }
  err = transport_esl_process_incoming(local_work_frame, &work_len,
                                       frame->src_mac, rx_queue_depth);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: transport pipeline failed (err=%d len=%u)",
             (int)err, (unsigned)frame->payload_len);
    goto rx_cleanup;
  }

  /* Verify Inner CRC appended after the FEC-encoded data */
  if (work_len < 4U) {
    ESP_LOGW("SESSION", "RX discard: FEC output too small for inner CRC (%u bytes)",
             (unsigned)work_len);
    goto rx_cleanup;
  }
  uint32_t rx_inner_crc = 0U;
  memcpy(&rx_inner_crc, local_work_frame + work_len - 4U, 4U);
  uint32_t calc_inner_crc = esp_crc32_le(0U, local_work_frame, work_len - 4U);
  if (rx_inner_crc != calc_inner_crc) {
    ESP_LOGE("SESSION", "Inner CRC mismatch: rx=%08lx, calc=%08lx",
             rx_inner_crc, calc_inner_crc);
    goto rx_cleanup; /* Silent discard */
  }
  work_len -= 4U; /* Strip Inner CRC */

  bool corrected = false;
  uint16_t fec_out_len = (uint16_t)sizeof(local_fec_out);
  err = ecc_hamming_decode(local_work_frame, work_len, local_fec_out,
                           &fec_out_len, &corrected);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: Hamming decode failed (err=%d in=%u)",
             (int)err, (unsigned)work_len);
    goto rx_cleanup;
  }

  /* The decoded FEC payload layout is: [2-byte LE box_ct_len][box_ct][64-byte Ed25519 sig].
   * The 2-byte prefix was prepended by session_send.c before FEC encode; strip it here. */
  if (fec_out_len < 66U) {
    ESP_LOGW("SESSION", "RX discard: decoded payload too small for prefix+box+sig (%u bytes)",
             (unsigned)fec_out_len);
    goto rx_cleanup;
  }
  uint16_t box_ct_len = (uint16_t)local_fec_out[0]
                      | ((uint16_t)local_fec_out[1] << 8U);
  uint16_t expected_min = (uint16_t)(2U + box_ct_len + 64U);
  if (expected_min > fec_out_len) {
    ESP_LOGW("SESSION", "RX discard: box_ct_len from prefix (%u) exceeds fec_out_len (%u)",
             (unsigned)box_ct_len, (unsigned)fec_out_len);
    goto rx_cleanup;
  }
  uint16_t sig_off = 2U + box_ct_len;
  memcpy(local_box_ct, local_fec_out + 2U, box_ct_len);
  memcpy(local_sig, local_fec_out + sig_off, 64U);

  /* Preserve box_ct_len before crypto_box_decrypt overwrites it with the
   * decrypted length. The Ed25519 signature covers the original ciphertext. */
  uint16_t box_ct_orig_len = box_ct_len;

  uint64_t rx_nonce_counter = 0ULL;
  err = transport_esl_get_last_nonce_counter(&rx_nonce_counter);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: nonce counter retrieval failed (err=%d)",
              (int)err);
    goto rx_cleanup;
  }
  err = session_get_nonce(local_nonce);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: session nonce retrieval failed (err=%d)",
              (int)err);
    goto rx_cleanup;
  }
  for (uint8_t i = 0U; i < 8U; i++) {
    local_nonce[8U + i] = (uint8_t)((rx_nonce_counter >> (i * 8U)) & 0xFFU);
  }

  /* Use the peer's X25519 public key for crypto_box ECDH decryption */
  if (!session_peer_box_pubkey_valid()) {
    ESP_LOGW("SESSION", "RX discard: peer box pubkey not valid");
    goto rx_cleanup;
  }
  err = session_get_local_box_pubkey(local_box_pk);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: local box pubkey retrieval failed (err=%d)",
              (int)err);
    goto rx_cleanup;
  }
  /* The peer's X25519 pubkey is in g_crypto_ctx.peer_box_pubkey;
   * pass it directly to crypto_box_decrypt via the context. */
  #ifdef CEEPEW_DEBUG_SERIAL
  ESP_LOGI("SESSION", "RX-BOX: src=%02X:%02X:%02X:%02X:%02X:%02X len=%u",
           frame->src_mac[0], frame->src_mac[1], frame->src_mac[2],
           frame->src_mac[3], frame->src_mac[4], frame->src_mac[5],
           (unsigned)box_ct_len);
  ESP_LOGI("SESSION", "RX-BOX: our_box_pk[0:4]=%02x%02x%02x%02x",
           local_box_pk[0], local_box_pk[1], local_box_pk[2], local_box_pk[3]);
  ESP_LOGI("SESSION", "RX-BOX: peer_box_pk[0:4]=%02x%02x%02x%02x",
           g_crypto_ctx.peer_box_pubkey[0], g_crypto_ctx.peer_box_pubkey[1],
           g_crypto_ctx.peer_box_pubkey[2], g_crypto_ctx.peer_box_pubkey[3]);
  #endif
  err = crypto_box_decrypt(&g_crypto_ctx, local_nonce,
                           g_crypto_ctx.peer_box_pubkey, local_box_ct,
                           box_ct_len, local_ascon_ct, &box_ct_len);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: crypto_box decrypt failed (err=%d len=%u)",
              (int)err, (unsigned)box_ct_len);
    goto rx_cleanup;
  }

  uint8_t ascon_nonce[16U];
  memcpy(ascon_nonce, local_nonce, sizeof(ascon_nonce));
  err = session_get_session_key(local_ascon_key);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: session key retrieval failed (err=%d)",
              (int)err);
    goto rx_cleanup;
  }

  uint16_t plain_len = (uint16_t)sizeof(local_plain);
  err = crypto_ascon_aead_decrypt(local_ascon_key, ascon_nonce, NULL, 0U,
                                  local_ascon_ct, box_ct_len, local_plain,
                                  &plain_len);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: Ascon AEAD decrypt failed (err=%d len=%u)",
              (int)err, (unsigned)box_ct_len);
    goto rx_cleanup;
  }

  /* Use the peer's Ed25519 sign_pk for signature verification */
  err = session_get_peer_public_key(local_sign_pk);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: peer sign_pk retrieval failed (err=%d)",
              (int)err);
    goto rx_cleanup;
  }
  err = crypto_eddsa_verify(local_sign_pk, local_box_ct, box_ct_orig_len, local_sig);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: Ed25519 signature verify failed (err=%d)",
             (int)err);
    goto rx_cleanup;
  }

  /* Store peer MAC for UI display */
  {
    UIState_t ui_state;
    session_ui_get_state_snapshot(&ui_state);
    session_ui_ctx_lock();
    memcpy(g_ui_ctx.peer_mac, frame->src_mac, CEEPEW_DEVICE_ID_BYTES);
    session_ui_ctx_unlock();
  }

  uint16_t decoded_len = (uint16_t)sizeof(local_work_frame);
  err = compress_huffman_decompress(local_plain, plain_len, local_work_frame,
                                    &decoded_len,
                                    (uint16_t)sizeof(local_work_frame));
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: Huffman decompress failed (err=%d in=%u)",
             (int)err, (unsigned)plain_len);
    goto rx_cleanup;
  }

  /* ── DOUBLE-ENDED POST-DERIVE SYNC ROUTING ──────────────────────────
   * A successful round-trip decryption of a 1-byte sync payload is the
   * proof that crypto_box works in both directions with the converged
   * keys. Before routing to the sync handler, verify that the frame's
   * source MAC matches the GATT-verified peer WiFi MAC — this closes
   * the window where an attacker could relay encrypted sync frames
   * from a spoofed MAC address. */
  if (decoded_len == 1U && (local_work_frame[0] == CEEPEW_KEY_SYNC_HELLO_BYTE ||
                            local_work_frame[0] == CEEPEW_KEY_SYNC_ACK_BYTE)) {
    /* HARDWARE-GATED IDENTITY CHECK: verify frame src_mac matches the
     * WiFi MAC that was authenticated over the secure GATT channel. */
    CeePewErr_t mac_check = session_verify_wifi_mac_matches_frame(frame->src_mac);
    if (mac_check != CEEPEW_OK) {
      ESP_LOGW("SESSION", "Sync frame WiFi MAC mismatch — discarding (possible relay)");
      goto rx_cleanup;
    }

    CeePewErr_t sync_err = session_handle_key_sync_byte(local_work_frame[0]);
    if (sync_err == CEEPEW_ERR_NEED_TX) {
      /* Responder received HELLO — send the ACK back using X25519 key.
       * After the ACK is enqueued, call session_confirm_ack_sent() to
       * complete the double-ended rendezvous. */
      uint8_t ack_plain[1] = {CEEPEW_KEY_SYNC_ACK_BYTE};
      uint8_t peer_mac[6];
      if (g_crypto_ctx.peer_box_pubkey_valid) {
        memcpy(peer_mac, frame->src_mac, 6U);
        CeePewErr_t ack_err = session_send_message(ack_plain, 1U, peer_mac,
                                   g_crypto_ctx.peer_box_pubkey);
        if (ack_err == CEEPEW_OK) {
          /* ACK sent successfully — confirm to the FSM so the
           * double-ended rendezvous can complete and the sync
           * barrier is cleared. */
          (void)session_confirm_ack_sent();
        } else {
          ESP_LOGW("SESSION", "Sync ACK send failed: %d", (int)ack_err);
        }
      }
    }
    (void)session_update_last_message_time();
    goto rx_cleanup;
  }

  err = msg_store_add(local_box_ct, box_ct_len, decoded_len, 0U);
  if (err != CEEPEW_OK) {
    ESP_LOGW("SESSION", "RX discard: msg_store_add failed (err=%d decoded=%u)",
             (int)err, (unsigned)decoded_len);
    goto rx_cleanup;
  }

  {
    UIEvent_t ui_event;
    memset(&ui_event, 0U, sizeof(ui_event));
    ui_event.type = UI_EVENT_MESSAGE_RECEIVED;
    ui_event.param = (uint32_t)msg_store_count();
    memcpy(ui_event.payload.message_rx.device_id, frame->src_mac,
           CEEPEW_DEVICE_ID_BYTES);
    ui_event.payload.message_rx.msg_id = (uint16_t)(msg_store_count() - 1U);

    BaseType_t result = xQueueSendToBack(g_ui_event_queue, &ui_event, 0U);
    if (result != pdPASS) {
      ESP_LOGW(TAG, "UI event queue full — dropping MESSAGE_RECEIVED event");
    }
  }

  (void)session_update_last_message_time();

  s_stats.rx_frames_processed++;

  err = CEEPEW_OK;

rx_cleanup:
  /* Secure zero all stack buffers that held key material or plaintext */
  ceepew_secure_zero(local_work_frame, sizeof(local_work_frame));
  ceepew_secure_zero(local_fec_out, sizeof(local_fec_out));
  ceepew_secure_zero(local_box_ct, sizeof(local_box_ct));
  ceepew_secure_zero(local_ascon_ct, sizeof(local_ascon_ct));
  ceepew_secure_zero(local_plain, sizeof(local_plain));
  ceepew_secure_zero(local_nonce, sizeof(local_nonce));
  ceepew_secure_zero(local_sign_pk, sizeof(local_sign_pk));
  ceepew_secure_zero(local_box_pk, sizeof(local_box_pk));
  ceepew_secure_zero(local_ascon_key, sizeof(local_ascon_key));
  ceepew_secure_zero(local_sig, sizeof(local_sig));
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
 * - g_ui_event_queue: depth = CEEPEW_UI_EVENT_QUEUE_DEPTH, element size =
 * UIEvent_t
 */
void task_session_init(void) {
  if (s_session_initialised && g_session_rx_queue != NULL &&
      g_ui_event_queue != NULL) {
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
    g_ui_event_queue =
        xQueueCreate(CEEPEW_UI_EVENT_QUEUE_DEPTH, sizeof(UIEvent_t));
    CEEPEW_ASSERT_VOID(g_ui_event_queue != NULL);
  }

  /* Create UI context mutex (Phase 2.C1) — guards g_ui_ctx access
   * between the UI task on Core 0 (writes) and the session task on
   * Core 1 (reads). Recursive to allow nested take/release within
   * a single function body. */
  if (g_ui_ctx_lock == NULL) {
    g_ui_ctx_lock = xSemaphoreCreateRecursiveMutex();
    CEEPEW_ASSERT_VOID(g_ui_ctx_lock != NULL);
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
void task_session_run(void *pvParameters) {
  (void)pvParameters; /* Unused parameter */

  s_session_task_handle = xTaskGetCurrentTaskHandle();
  CEEPEW_ASSERT_VOID(s_session_task_handle != NULL);

  ESP_LOGI(TAG, "Session task started on Core %d", xPortGetCoreID());

  /* Main loop: drain RX queue and process frames */
  for (;;) {
    if (g_session_rx_queue == NULL || g_ui_event_queue == NULL) {
      vTaskDelay(pdMS_TO_TICKS(100U));
      continue;
    }

    /* Phase 2.C2: Process deferred wipe requests from UI task (Core 0).
     * session_wipe() must run on Core 1 because it touches crypto_ctx,
     * region allocator, and pipeline. The UI task sets
     * g_session_wipe_requested and we execute the wipe here. */
    if (session_check_wipe_requested()) {
      ESP_LOGW(TAG, "Executing deferred session wipe requested by UI task");
      (void)session_wipe();
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

      /* Phase 2.C1: snapshot current_state once for the periodic
       * maintenance path. The reads at lines below (KEYDER check,
       * pair_err handler) all observe the same logical value within
       * this 1s tick. */
      UIState_t ui_state;
      session_ui_get_state_snapshot(&ui_state);

      /* Discovery timeout fallback: if we've been scanning for too long, clear
       * RGB to avoid stuck-on LEDs */
      {
        uint64_t now_ms_local = (uint64_t)(esp_timer_get_time() / 1000LL);
        const uint64_t DISCOVERY_TIMEOUT_MS =
            60000ULL; /* 60s conservative fallback */
        if (s_ble_scan_start_ms != 0ULL &&
            (now_ms_local - s_ble_scan_start_ms) >= DISCOVERY_TIMEOUT_MS) {
          ESP_LOGI(TAG, "Discovery timeout: clearing RGB after %llu ms",
                   (unsigned long long)(now_ms_local - s_ble_scan_start_ms));
          (void)rgb_set_pattern(RGB_OFF);
          s_ble_scan_start_ms = 0ULL;
        }
      }

      /* Phase 4: Periodic session maintenance (check TTL, nonce exhaustion,
       * etc) */
      if (session_is_active()) {
        /* Check if nonce counter is near exhaustion */
        uint64_t nonce_counter = session_get_nonce_counter();
        if (nonce_counter >= CEEPEW_NONCE_HARD_LIMIT) {
          CEEPEW_LOG(TAG,
                     "Nonce exhaustion detected (counter=%llu >= limit=%llu)",
                     (unsigned long long)nonce_counter,
                     (unsigned long long)CEEPEW_NONCE_HARD_LIMIT);
          session_ui_ctx_lock();
          g_ui_ctx.error_start_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
          g_ui_ctx.transition_ready = true;
          session_ui_ctx_unlock();
          (void)ui_manager_transition_to(UI_STATE_NONCE_EXHAUSTED);
          continue;
        }

        /* Check if message TTL has expired */
        uint32_t idle_seconds = 0U;
        CeePewErr_t ttl_err = session_get_idle_seconds(&idle_seconds);
        if (ttl_err == CEEPEW_OK) {
          uint32_t ttl_limit = CEEPEW_MESSAGE_TTL_S; /* 1 hour by default */
          if (idle_seconds >= ttl_limit) {
            CEEPEW_LOG(TAG, "Message TTL expired (idle=%lu >= limit=%lu)",
                       (unsigned long)idle_seconds, (unsigned long)ttl_limit);
            session_wipe();
            continue;
          }
        }

        /* ── PFS HANDHSAKE RETRANSMISSION ──────────────────────────────
         * PFS_INIT is sent once during key derivation, but may be lost
         * if the peer's ESP-NOW is still initializing (BLE teardown +
         * WiFi restart window). The initiator retransmits every
         * CEEPEW_PFS_RETRY_INTERVAL_MS until PFS completes. The responder
         * never sends PFS_INIT — it waits for the initiator's frame. */
        if (session_is_active() && session_get_phase() >= 3U &&
            !session_pfs_active() && session_get_role()) {
          uint64_t now_pfs = (uint64_t)(esp_timer_get_time() / 1000LL);
          if (now_pfs - s_last_pfs_retry_ms >= CEEPEW_PFS_RETRY_INTERVAL_MS) {
            s_last_pfs_retry_ms = now_pfs;
            CeePewErr_t pfs_err = session_pfs_initiate();
            if (pfs_err == CEEPEW_OK) {
              uint8_t local_pfs_pubkey[32U];
              if (session_get_local_pfs_pubkey(local_pfs_pubkey) == CEEPEW_OK) {
                uint8_t pfs_frame[CEEPEW_PACKET_MAX_BYTES];
                uint16_t pfs_len = 0U;
                if (transport_esl_build_pfs_handshake(pfs_frame, &pfs_len,
                        sizeof(pfs_frame), local_pfs_pubkey, true) == CEEPEW_OK) {
                  (void)hal_radio_send_broadcast(pfs_frame, pfs_len);
                  ESP_LOGI(TAG, "PFS retry: sent PFS_INIT (t=%llu ms)", now_pfs);
                }
              }
            }
          }
        }

        /* ── DOUBLE-ENDED POST-DERIVE SYNC BARRIER ──────────────────────
         * Until the encrypted HELLO/ACK round-trip completes on the static
         * baseline channel, the UI must remain on KEYDER. The drive
         * function returns ERR_TIMEOUT once the deadline elapses; we then
         * transition to FAILED. The barrier is ONLY cleared inside
         * session_clear_sync_barrier_internal() which also resets the ARQ
         * engine and flushes the RX queue — so by the time we reach the
         * else branch below, the radio and protocol state are clean. */
        if (!session_sync_barrier_cleared()) {
          uint64_t now_ms_local = (uint64_t)(esp_timer_get_time() / 1000LL);
          CeePewErr_t sync_err = session_drive_post_derive_sync(now_ms_local);
          if (sync_err == CEEPEW_ERR_TIMEOUT) {
            CEEPEW_LOG(
                TAG, "Post-derive sync timed out after %u ms — failing pairing",
                (unsigned)CEEPEW_KEY_SYNC_TIMEOUT_MS);
            session_ui_ctx_lock();
            g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_TIMED_OUT;
            g_ui_ctx.transition_ready = true;
            session_ui_ctx_unlock();
            (void)session_reset_to_discovery();
            task_session_reset_pairing_static_state();
            task_session_pairing_flow_reset();
            (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
            continue;
          }
        } else if (ui_state == UI_STATE_KEYDER) {
          /* ── CLEAN SLATE CHAT EVOLUTION ──────────────────────────────
           * The sync barrier has been verified (encrypted round-trip
           * confirmed). The ARQ engine was reset and RX queue was flushed
           * inside session_clear_sync_barrier_internal(). Now evolve the
           * UI directly to CHAT_MENU — the system is ready to process
           * incoming and outgoing user chat text without dropping
           * early packets. */
          CEEPEW_LOG(TAG,
                     "Post-derive sync cleared — evolving to CHAT_MENU");
          session_ui_ctx_lock();
          g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_SUCCESS;
          g_ui_ctx.transition_ready = true;
          session_ui_ctx_unlock();
          (void)ui_manager_transition_to(UI_STATE_CHAT_MENU);
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
        if (ui_state != UI_STATE_PAIRING_FAILED) {
          if (pair_err == CEEPEW_ERR_BUSY) {
            /* [FIX-1] A write retry is pending — not a permanent failure */
            CEEPEW_LOG(TAG, "BLE pairing driver busy (write retry pending)");
          } else {
            session_ui_ctx_lock();
            if (pair_err == CEEPEW_ERR_TIMEOUT) {
              g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_TIMED_OUT;
            } else if (pair_err == CEEPEW_ERR_AUTH_FAIL ||
                       pair_err == CEEPEW_ERR_SIG_FAIL ||
                       pair_err == CEEPEW_ERR_CRYPTO) {
              g_ui_ctx.pairing_result_reason =
                  UI_PAIRING_RESULT_COMMITMENT_FAIL;
            } else {
              g_ui_ctx.pairing_result_reason = UI_PAIRING_RESULT_LINK_FAIL;
            }
            g_ui_ctx.transition_ready = true;
            session_ui_ctx_unlock();

            task_session_reset_pairing_static_state();

            if (pair_err == CEEPEW_ERR_AUTH_FAIL ||
                pair_err == CEEPEW_ERR_SIG_FAIL ||
                pair_err == CEEPEW_ERR_CRYPTO) {
              /* Mismatch error: reset and show unified PAIRING_FAILED screen */
              (void)session_reset_to_discovery();
              (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
            } else {
              /* Link or timeout error: reset immediately */
              (void)session_reset_to_discovery();
              (void)ui_manager_transition_to(UI_STATE_PAIRING_FAILED);
            }
          }
        }
      }

      if (transport_ble_is_initialised()) {
        (void)transport_ble_retry_scan_if_needed();
      }

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
