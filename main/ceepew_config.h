/* main/ceepew_config.h */
#ifndef CEEPEW_CONFIG_H
#define CEEPEW_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Message Limits                                                             */
/* -------------------------------------------------------------------------- */
#define CEEPEW_MAX_MSG_BYTES             160U
#define CEEPEW_MAX_MSG_CHARS             160U
#define CEEPEW_MAX_FRAGMENTS             4U
#define CEEPEW_STREAM_MAX_FRAGMENTS      512U
#define CEEPEW_STREAM_PAYLOAD_MAX        210U   /* bytes per fragment payload */
#define CEEPEW_HUFF_BUF_MAX              200U   /* worst-case compressed      */
#define CEEPEW_FEC_BUF_MAX               300U   /* worst-case FEC-encoded     */
#define CEEPEW_PACKET_MAX_BYTES          512U   /* max total frame size       */

/* -------------------------------------------------------------------------- */
/* Crypto Parameters                                                           */
/* -------------------------------------------------------------------------- */
#define CEEPEW_SESSION_KEY_BYTES         16U    /* Ascon-128 key = 128 bits   */
#define CEEPEW_ASCON_TAG_BYTES           16U    /* Ascon auth tag             */
#define CEEPEW_SHA256_BYTES              32U
#define CEEPEW_ED25519_PUBKEY_BYTES      32U
#define CEEPEW_ED25519_PRIVKEY_BYTES     64U    /* seed (32) + pubkey (32)    */
#define CEEPEW_CURVE25519_PK_BYTES       32U
#define CEEPEW_CURVE25519_SK_BYTES       32U
#define CEEPEW_DEVICE_ID_BYTES           6U     /* MAC address                */
#define CEEPEW_DEVICE_SIG_BYTES          32U    /* HMAC-SHA256 device sig     */
#define CEEPEW_SESSION_CODE_MIN_LEN      8U

/* Commitment sizes */
#define CEEPEW_COMMITMENT_BYTES          32U
/* Truncated commitment for advertisement-based exchange.
 * Fits in 31-byte SCAN_RSP alongside the 8-byte complete name and the
 * 4-byte manufacturer-data AD header (company 0xCEEE + subtype 0x50):
 *   8 (name) + 1+1+2+1+2+16 (mfr data) = 31 bytes (after Phase 7).
 * 128 bits of SHA-256 is sufficient because the 4-digit session code
 * itself only has 10,000 possible values (~13 bits of entropy). */
#define CEEPEW_COMMITMENT_ADV_BYTES      16U

/* Beacon replay defense (Bug 2 fix). Each commitment beacon carries a
 * 2-byte monotonic nonce. The receiver rejects any beacon whose nonce
 * is not strictly greater than the highest nonce it has previously
 * accepted from this peer — closing the "first-scan-wins" replay window
 * where a captured beacon could be re-broadcast by a hostile device.
 *
 * Wire layout in the mfr AD payload after the company/subtype header:
 *   [2B nonce][16B commitment]   (18 bytes payload, 22 with header)
 * SCAN_RSP budget: 8 (name) + 1+1+2+1+2+16 = 31 bytes — exactly at limit. */
#define CEEPEW_BEACON_NONCE_BYTES       2U
#define CEEPEW_BEACON_PAYLOAD_BYTES     (CEEPEW_COMMITMENT_ADV_BYTES + CEEPEW_BEACON_NONCE_BYTES)
#define CEEPEW_BEACON_MFR_AD_BYTES      (5U + CEEPEW_BEACON_PAYLOAD_BYTES)  /* incl. length+type+company+subtype */

/* -------------------------------------------------------------------------- */
/* Nonce Safety                                                                */
/*                                                                             */
/* SECURITY CRITICAL: CEEPEW_NONCE_HARD_LIMIT must NOT equal UINT64_MAX.      */
/* If nonce_counter reaches UINT64_MAX it wraps to 0 on next increment,       */
/* causing nonce reuse — the most catastrophic crypto failure.                 */
/*                                                                             */
/* The guard at HARD_LIMIT gives a warning window before the counter          */
/* approaches the danger zone. Session must terminate at HARD_LIMIT.          */
/*                                                                             */
/* Nonce parity invariant: initiator uses even nonces (0,2,4,...),            */
/* responder uses odd nonces (1,3,5,...). session_enforce_nonce_limit()       */
/* increments by 2 to preserve this. The two nonce sequences are              */
/* guaranteed disjoint — no collision possible even if both sides encrypt     */
/* simultaneously.                                                            */
/*                                                                             */
/* Phase 4: NONCE_HARD_LIMIT = 2^56 = 72,057,594,037,927,936 IVs             */
/* At 1 msg/sec, device lifetime = 2,282,404 years (far > device lifespan)   */
/* At 90% of limit, return CEEPEW_ERR_NONCE_NEARLY_EXHAUSTED (warning)        */
/* At 100% of limit, return CEEPEW_ERR_NONCE_EXHAUSTED (hard stop)            */
/* Exhaustion triggers immediate session_wipe() and UI reset to "nonce_expired"*/
/* -------------------------------------------------------------------------- */
#define CEEPEW_NONCE_WARN_THRESHOLD      1000ULL
#define CEEPEW_NONCE_HARD_LIMIT          (1ULL << 56)
#define CEEPEW_NONCE_WARNING_LIMIT       (CEEPEW_NONCE_HARD_LIMIT * 90U / 100U)
#define CEEPEW_NONCE_MAX_GAP             64U

/* -------------------------------------------------------------------------- */
/* ARQ (Stop-and-Wait)                                                         */
/* -------------------------------------------------------------------------- */
#define CEEPEW_ARQ_MAX_RETRIES           3U
#define CEEPEW_ARQ_TIMEOUT_MS            500U
#define CEEPEW_SEQ_WINDOW_SIZE           32U

/* Post-derive sync barrier — 1-byte magic plaintexts exchanged inside the
 * encrypted ESP-NOW tunnel to verify that crypto_box works in BOTH
 * directions before the UI advances to PAIRING_SUCCESS. Without this
 * round-trip, an initiator whose local key derivation succeeds would
 * transition the UI while the responder was still in PAIRING (Bug 3).
 *
 * The magic values are chosen > 0x7F so they can never collide with
 * ASCII chat text typed by the user. */
#define CEEPEW_KEY_SYNC_HELLO_BYTE       0xA5U   /* initiator → responder */
#define CEEPEW_KEY_SYNC_ACK_BYTE         0x5AU   /* responder → initiator */
#define CEEPEW_KEY_SYNC_TIMEOUT_MS       10000U  /* Give up → PAIRING_FAILED */
#define CEEPEW_KEY_SYNC_RETRY_MS         250U    /* Initiator retransmit cadence */

/* -------------------------------------------------------------------------- */
/* Session                                                                     */
/* -------------------------------------------------------------------------- */
#define CEEPEW_SESSION_GRACE_S           30U
#define CEEPEW_SESSION_TTL_S             3600U
#define CEEPEW_MSG_TTL_S                 600U   /* 10-minute auto-wipe        */
#define CEEPEW_MAX_MESSAGES              20U
#define CEEPEW_PAIRING_TIMEOUT_S         30U
#define CEEPEW_T_ROUND_S                 8U     /* Phase 2 commitment window  */
#define CEEPEW_MESSAGE_TTL_S             3600U  /* Phase 4: Message auto-wipe, 1hr default */
#define CEEPEW_MESSAGE_TTL_DIAG_S        300U   /* Phase 4: 5 min in DIAG mode */

/* -------------------------------------------------------------------------- */
/* Input / UI                                                                  */
/* -------------------------------------------------------------------------- */
#define CEEPEW_UI_LOOP_DELAY_MS          15U   /* ~60 Hz render cap; worst-case
                                                   * full-frame I2C push ~6 ms,
                                                   * leaves ~9 ms of slack per
                                                   * tick for BLE/session on C1. */
#define CEEPEW_BUTTON_DEBOUNCE_MS        25U
#define CEEPEW_DIAG_HOLD_MS              2000U
#define CEEPEW_DIAG_PAGES                6U
#define CEEPEW_DIAG_TIMEOUT_S            30U
#define CEEPEW_DIAG_SAMPLE_MS            1000U
#define CEEPEW_ADC_MAX_RAW               4095U
#define CEEPEW_ADC_SAMPLES               8U
#define CEEPEW_POT_EMA_ALPHA_NUM         25U    /* EMA alpha = 25/100 = 0.25 (snappier) */
#define CEEPEW_POT_EMA_ALPHA_DEN         100U
#define CEEPEW_POT_DEADZONE              60U    /* ADC units at rails (reduced deadzone) */
#define CEEPEW_POT_EDGE_HYSTERESIS       32U
#define CEEPEW_CLICK_COOLDOWN_MS         200U
#define CEEPEW_EDGE_STABLE_MS            300U
#define CEEPEW_AUTOSCROLL_INTERVAL_MS    5000U
#define CEEPEW_OLED_WIDTH_PX             128U
#define CEEPEW_OLED_HEIGHT_PX            64U
/* Phase 4: Fingerprint & session lifecycle */
#define CEEPEW_FINGERPRINT_BYTES         16U    /* SHA256[0:15] of peer key   */
#define CEEPEW_RGB_IDLE_PULSE_MS         500U   /* Discovery mode pulse       */
#define CEEPEW_RGB_CODE_ENTRY_BLINK_MS   200U   /* Code entry blink interval  */
#define CEEPEW_RGB_ERROR_BLINK_MS        300U   /* Error/nonce_exhausted blink*/
#define CEEPEW_RGB_REJECT_SEQUENCE_CT    3U     /* Red blinks on fingerprint reject */

/* Pairing Supervisor Watchdog (Event-Driven Architecture)
 *
 * Per phase, the supervisor records T_enter and if the phase does not advance
 * within CEEPEW_PHASE_TIMEOUT_MS, it forces a radio restart. */
#define CEEPEW_PHASE_TIMEOUT_MS          10000U /* Default per-phase watchdog */
#define CEEPEW_PHASE_TIMEOUT_CONNECT_MS  8000U  /* CONNECTING phase           */
#define CEEPEW_PHASE_TIMEOUT_MTU_MS      5000U  /* MTU_NEGOTIATING phase      */
#define CEEPEW_PHASE_TIMEOUT_DISC_MS     6000U  /* CHAR_DISCOVERING phase     */
#define CEEPEW_PHASE_TIMEOUT_VERIFY_MS   15000U /* VERIFICATION phase         */
#define CEEPEW_PHASE_TIMEOUT_GATT_MS     45000U /* Hybrid-GATT: allow human typing delay + GATTC_OPEN → write */
#define CEEPEW_PHASE_TIMEOUT_OVERALL_MS  30000U /* Whole-pairing ceiling      */

/* Pairing event queue depth — small because handlers drain quickly. */
#define CEEPEW_PAIRING_EVENT_QUEUE_DEPTH 12U

/* Pairing supervisor task parameters */
#define CEEPEW_SUPERVISOR_PERIOD_MS      500U   /* 2 Hz watchdog tick         */
#define CEEPEW_SUPERVISOR_PRIORITY       2U     /* Below UI/Session tasks     */
#define CEEPEW_SUPERVISOR_STACK_BYTES    3072U  /* Local to transport_ble     */
#define CEEPEW_RECONNECT_JITTER_MIN_MS   200U   /* Reconnect backoff floor    */
#define CEEPEW_RECONNECT_JITTER_MAX_MS   800U   /* Reconnect backoff ceiling  */
#define CEEPEW_MAX_RECONNECT_ATTEMPTS    5U     /* From spec: 5 attempts max  */

/* -------------------------------------------------------------------------- */
/* FEC (Hamming)                                                               */
/* -------------------------------------------------------------------------- */
#define CEEPEW_FEC_BLOCK_SIZE            15U    /* codeword bits              */
#define CEEPEW_FEC_DATA_SIZE             11U    /* data bits per codeword     */
#define CEEPEW_FEC_PARITY_SIZE           4U     /* parity bits per codeword   */
#define CEEPEW_HAMMING_DATA_BITS         11U
#define CEEPEW_HAMMING_CODE_BITS         15U

/* -------------------------------------------------------------------------- */
/* FreeRTOS                                                                    */
/* -------------------------------------------------------------------------- */
#define CEEPEW_CORE0_STACK_BYTES         4096U
#define CEEPEW_CORE1_STACK_BYTES         16384U
#define CEEPEW_QUEUE_DEPTH               8U
#define CEEPEW_TASK_UI_PRIORITY          3U
#define CEEPEW_TASK_SESSION_PRIORITY     3U

/* -------------------------------------------------------------------------- */
/* ESL / Transport / Replay                                                    */
/* -------------------------------------------------------------------------- */
#define CEEPEW_REPLAY_WINDOW_SIZE        64U
#define CEEPEW_TIMESTAMP_SLACK_S         45U   /* widened from 15 to tolerate raw-uptime skew
                                                  * between two devices; WireGuard 64-bit replay
                                                  * bitmap still catches exact replays within the
                                                  * wider window. A future BLE time-sync
                                                  * characteristic will replace this. */
#define CEEPEW_ESPNOW_CHANNEL            1U
#define CEEPEW_HOP_CHANNELS              9U
#define CEEPEW_HOP_SHIFT                 6U
#define CEEPEW_HOP_INTERVAL_MS           5000U  /* 5 seconds per spec */
#define CEEPEW_DOS_QUEUE_THRESHOLD       6U
#define CEEPEW_COOKIE_ROTATE_S           120U
#define CEEPEW_COOKIE_BYTES              16U
#define CEEPEW_RESPONDER_ACK_TIMEOUT_MS  5000U

/* -------------------------------------------------------------------------- */
/* Region Allocator                                                            */
/* -------------------------------------------------------------------------- */
#define CEEPEW_REGION_POOL_BYTES         (48U * 1024U)
#define CEEPEW_REGION_ALIGN              8U
#define CEEPEW_REGION_MAX_ALLOCS         256U

/* -------------------------------------------------------------------------- */
/* DIAG / Metrics                                                              */
/* -------------------------------------------------------------------------- */
#define CEEPEW_METRIC_BAR_H              6U
#define CEEPEW_DOT_SPACING               8U
#define CEEPEW_BAR_WIDTH                 8U
#define CEEPEW_TEMP_WARN_C               70
#define CEEPEW_STACK_WARN_PCT            90U
#define CEEPEW_VBAT_LOW_MV               3400U
#define CEEPEW_VBAT_CRITICAL_MV          3200U

/* -------------------------------------------------------------------------- */
/* Padding                                                                     */
/* -------------------------------------------------------------------------- */
#define CEEPEW_PAD_BLOCK_SIZE            64U    /* XSalsa20 block size        */

/* -------------------------------------------------------------------------- */
/* Pipeline                                                                    */
/* -------------------------------------------------------------------------- */
#define CEEPEW_PIPELINE_MAX_STAGES       16U

/* -------------------------------------------------------------------------- */
/* Power Management                                                            */
/* -------------------------------------------------------------------------- */
/* WiFi modem power save (Tier 1): WIFI_PS_MIN_MODEM saves ~15 mA average
 * while keeping ESP-NOW RX latency < 50 ms. Disabled during active chat
 * (Phase 3) for minimum latency; enabled during discovery (Phase 1) and
 * idle. */
#define CEEPEW_WIFI_PS_DISCOVERY          WIFI_PS_MIN_MODEM
#define CEEPEW_WIFI_PS_ACTIVE_CHAT        WIFI_PS_NONE

/* BLE scan duty cycling: window/interval ratio controls average power.
 * 30 ms scan / 300 ms interval = 10% duty cycle (saves ~40% BLE power).
 * During active pairing (Phase 2), scan is 100% (WIFI_PS_NONE equivalent). */
#define CEEPEW_BLE_SCAN_INTERVAL_DISC_MS  300U
#define CEEPEW_BLE_SCAN_WINDOW_DISC_MS    30U
#define CEEPEW_BLE_SCAN_INTERVAL_PAIR_MS  60U   /* Active pairing: fast scan */
#define CEEPEW_BLE_SCAN_WINDOW_PAIR_MS    60U

/* -------------------------------------------------------------------------- */
/* Debug                                                                       */
/*                                                                             */
/* SECURITY: Even when CEEPEW_DEBUG_SERIAL is defined, the following are      */
/* NEVER printed: key material, plaintext content, peer MAC during pairing.   */
/* -------------------------------------------------------------------------- */
#define CEEPEW_DEBUG_SERIAL   /* Uncomment for development builds ONLY  */

#ifdef CEEPEW_DEBUG_SERIAL
    #define CEEPEW_LOG(tag, fmt, ...) \
        do { ESP_LOGI(tag, fmt, ##__VA_ARGS__); } while(0)
#else
    #define CEEPEW_LOG(tag, fmt, ...) do {} while(0)
#endif

/* Phase 4: Session lifecycle state names for logging */
#define CEEPEW_SESSION_STATE_NAMES ((const char *[]){ \
    "idle", "discovery", "code_entry", "keygen", \
    "fingerprint", "chat_active", "chat_idle", "nonce_exhausted", \
    "error", "wipe_pending" \
})
#define CEEPEW_SESSION_STATE_COUNT 10U

/* ── Firmware Version ───────────────────────────────────────────────────────
 * Semantic version (MAJOR.MINOR.PATCH). CEEPEW_GIT_HASH is injected by
 * CMakeLists.txt at build time via `git describe --always --dirty`.         */
#define CEEPEW_FIRMWARE_VERSION_MAJOR    1U
#define CEEPEW_FIRMWARE_VERSION_MINOR    0U
#define CEEPEW_FIRMWARE_VERSION_PATCH    0U

#ifndef CEEPEW_GIT_HASH
#define CEEPEW_GIT_HASH                  "unknown"
#endif

/* BLE advertisement version field: 7 bytes total
 *   [0] = major, [1] = minor, [2] = patch,
 *   [3..6] = first 4 bytes of git hash (truncated) */
#define CEEPEW_VERSION_ADV_BYTES         7U

#endif /* CEEPEW_CONFIG_H */
