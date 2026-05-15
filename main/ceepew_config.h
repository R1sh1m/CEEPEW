/* main/ceepew_config.h */
#ifndef CEEPEW_CONFIG_H
#define CEEPEW_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Message Limits */
/* -------------------------------------------------------------------------- */
#define CEEPEW_MAX_MSG_BYTES             160U
#define CEEPEW_MAX_FRAGMENTS             4U
#define CEEPEW_STREAM_MAX_FRAGMENTS      256U

/* -------------------------------------------------------------------------- */
/* Crypto Parameters */
/* -------------------------------------------------------------------------- */
#define CEEPEW_SESSION_KEY_BYTES         16U
#define CEEPEW_ASCON_TAG_BYTES           16U
#define CEEPEW_SHA256_BYTES              32U
#define CEEPEW_ED25519_PUBKEY_BYTES      32U
#define CEEPEW_ED25519_PRIVKEY_BYTES     64U

/* -------------------------------------------------------------------------- */
/* Nonce Safety */
/* -------------------------------------------------------------------------- */
#define CEEPEW_NONCE_MAX_GAP             64U
#define CEEPEW_NONCE_HARD_LIMIT          0xFFFFFFFFFFFFFFFFULL

/* -------------------------------------------------------------------------- */
/* ARQ (Stop-and-Wait / Retransmit) */
/* -------------------------------------------------------------------------- */
#define CEEPEW_ARQ_MAX_RETRIES           3U
#define CEEPEW_ARQ_RETRY_TIMEOUT_MS      500U

/* -------------------------------------------------------------------------- */
/* Session */
/* -------------------------------------------------------------------------- */
#define CEEPEW_SESSION_GRACE_S           30U
#define CEEPEW_SESSION_TTL_S             3600U
#define CEEPEW_SESSION_CODE_MIN_LEN      8U

/* -------------------------------------------------------------------------- */
/* Input / UI */
/* -------------------------------------------------------------------------- */
#define CEEPEW_UI_LOOP_DELAY_MS          30U
#define CEEPEW_BUTTON_DEBOUNCE_MS        25U
#define CEEPEW_DIAG_HOLD_MS              2000U
#define CEEPEW_DIAG_PAGES                6U
#define CEEPEW_ADC_MAX_RAW               4095U

/* -------------------------------------------------------------------------- */
/* FEC (Hamming / parity) */
/* -------------------------------------------------------------------------- */
#define CEEPEW_FEC_BLOCK_SIZE            15U
#define CEEPEW_FEC_DATA_SIZE             11U
#define CEEPEW_FEC_PARITY_SIZE           (CEEPEW_FEC_BLOCK_SIZE - CEEPEW_FEC_DATA_SIZE)

/* -------------------------------------------------------------------------- */
/* FreeRTOS / Timing */
/* -------------------------------------------------------------------------- */
#define CEEPEW_TASK_STACK_UI             4096U
#define CEEPEW_TASK_STACK_SESSION        8192U
#define CEEPEW_DIAG_SAMPLE_MS            1000U

/* -------------------------------------------------------------------------- */
/* ESL / Transport / Replay */
/* -------------------------------------------------------------------------- */
#define CEEPEW_REPLAY_WINDOW_SIZE        64U
#define CEEPEW_TIMESTAMP_SLACK_S         15U
#define CEEPEW_ESPNOW_CHANNEL            1U

/* -------------------------------------------------------------------------- */
/* Region Allocator */
/* -------------------------------------------------------------------------- */
#define CEEPEW_REGION_POOL_BYTES         (200U * 1024U)
#define CEEPEW_REGION_ALIGN              8U
#define CEEPEW_REGION_MAX_ALLOCS         256U

/* -------------------------------------------------------------------------- */
/* DIAG / Metrics */
/* -------------------------------------------------------------------------- */
#define CEEPEW_METRIC_BAR_H              6U
#define CEEPEW_DOT_SPACING               8U

/* -------------------------------------------------------------------------- */
/* Misc / Limits */
/* -------------------------------------------------------------------------- */
#define CEEPEW_MAX_DEVICE_ID_BYTES       8U

#endif /* CEEPEW_CONFIG_H */
