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
#define CEEPEW_COMMITMENT_BYTES          16U
#define CEEPEW_COMMITMENT_LEGACY_BYTES   8U

/* -------------------------------------------------------------------------- */
/* Nonce Safety                                                                */
/*                                                                             */
/* SECURITY CRITICAL: CEEPEW_NONCE_HARD_LIMIT must NOT equal UINT64_MAX.      */
/* If nonce_counter reaches UINT64_MAX it wraps to 0 on next increment,       */
/* causing nonce reuse — the most catastrophic crypto failure.                 */
/*                                                                             */
/* The guard at HARD_LIMIT gives a 1000-message warning window before the     */
/* counter approaches the danger zone. Session must terminate at HARD_LIMIT.  */
/*                                                                             */
/* Phase 4: NONCE_HARD_LIMIT = 2^56 = 72,057,594,037,927,936 IVs             */
/* At 1 msg/sec, device lifetime = 2,282,404 years (far > device lifespan)   */
/* Exhaustion triggers immediate session_wipe() and UI reset to "nonce_expired"*/
/* -------------------------------------------------------------------------- */
#define CEEPEW_NONCE_WARN_THRESHOLD      1000ULL
#define CEEPEW_NONCE_HARD_LIMIT          (1ULL << 56)
#define CEEPEW_NONCE_MAX_GAP             64U

/* -------------------------------------------------------------------------- */
/* ARQ (Stop-and-Wait)                                                         */
/* -------------------------------------------------------------------------- */
#define CEEPEW_ARQ_MAX_RETRIES           3U
#define CEEPEW_ARQ_TIMEOUT_MS            500U
#define CEEPEW_SEQ_WINDOW_SIZE           32U

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
#define CEEPEW_UI_LOOP_DELAY_MS          30U
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
#define CEEPEW_CORE1_STACK_BYTES         8192U
#define CEEPEW_QUEUE_DEPTH               8U
#define CEEPEW_TASK_UI_PRIORITY          3U
#define CEEPEW_TASK_SESSION_PRIORITY     3U

/* -------------------------------------------------------------------------- */
/* ESL / Transport / Replay                                                    */
/* -------------------------------------------------------------------------- */
#define CEEPEW_REPLAY_WINDOW_SIZE        64U
#define CEEPEW_TIMESTAMP_SLACK_S         15U
#define CEEPEW_ESPNOW_CHANNEL            1U
#define CEEPEW_HOP_CHANNELS              9U
#define CEEPEW_HOP_SHIFT                 6U
#define CEEPEW_HOP_INTERVAL_MS           5000U  /* 5 seconds per spec */
#define CEEPEW_DOS_QUEUE_THRESHOLD       6U
#define CEEPEW_COOKIE_ROTATE_S           120U
#define CEEPEW_COOKIE_BYTES              16U
#define CEEPEW_MAX_RECONNECT_ATTEMPTS    5U

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

#endif /* CEEPEW_CONFIG_H */
