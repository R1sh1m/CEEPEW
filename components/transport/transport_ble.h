/* components/transport/transport_ble.h
 *
 * CEE-PEW BLE Transport (Phases 1-2: Discovery & Pairing)
 * Uses Bluedroid BLE stack from ESP-IDF for advertisement scanning and GATT exchange.
 *
 * PHASE 1 (DISCOVERY):
 *   - Local device advertises its MAC, device name, and capability flags
 *   - Both peers scan for each other's advertisements
 *   - Once found, exchange MAC addresses and move to Phase 2
 *
 * PHASE 2 (PAIRING):
 *   - Establish BLE connection (central → peripheral)
 *   - Exchange commitment hash (session code digest) via GATT
 *   - Verify commitment matches (user confirms 4-digit code on both devices)
 *   - Once confirmed, handoff to ESP-NOW for Phase 3 (active session)
 *
 * NOTE: BLE is ONLY used for pairing discovery; all bulk data flows on ESP-NOW.
 */

#ifndef TRANSPORT_BLE_H
#define TRANSPORT_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "ceepew_assert.h"
#include "ceepew_config.h"
#ifdef CONFIG_BT_ENABLED
#include "esp_bt_defs.h"
#else
typedef uint8_t esp_ble_addr_type_t;
#ifndef BLE_ADDR_TYPE_PUBLIC
#define BLE_ADDR_TYPE_PUBLIC 0U
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CEEPEW_GATT_IF_NONE = 0xFFU
};

/* BLE connection states */
typedef enum {
    BLE_IDLE = 0U,
    BLE_ADVERTISING,
    BLE_SCANNING,
    BLE_ADVERTISING_AND_SCANNING,
    BLE_CONNECTED,
    BLE_PAIRING,
    BLE_DONE
} BleState_t;

#define CEEPEW_DISCOVERY_PEER_VISIBLE_MS 15000U
#define CEEPEW_DISCOVERY_PEER_CLEAR_MS   20000U

/* Peer discovery record (found during scan) */
typedef struct {
    uint8_t  peer_mac[6];
    int8_t   rssi;
    uint32_t seen_at;
    uint8_t  name[16];
    uint8_t  name_len;
} BlePeerRecord_t;

/* Verification status codes for 0xFFF2 characteristic */
typedef enum {
    CEEPEW_VERIFY_PENDING = 0U,  /* Verification in progress */
    CEEPEW_VERIFY_OK = 1U,       /* Verification passed */
    CEEPEW_VERIFY_MISMATCH = 2U  /* Verification failed (code mismatch) */
} VerificationStatus_t;

/* BLE pairing session context */
typedef struct {
    BleState_t     state;
    uint8_t        local_mac[6];
    uint8_t        peer_mac[6];
    esp_ble_addr_type_t peer_addr_type;
    uint8_t        commitment_digest[CEEPEW_COMMITMENT_BYTES];      /* Truncated SHA256(session_code) */
    uint8_t        local_commitment_len; /* bytes stored for local commitment */
    uint8_t        peer_commitment_len;  /* bytes received from peer (8 or 16) */
    bool           peer_commitment_legacy; /* true if peer uses legacy 8-byte commit */
    bool           is_initiator_role;     /* true = we opened the GATTC connection */
    uint8_t        pending_peer_commitment[CEEPEW_COMMITMENT_BYTES];
    uint8_t        pending_peer_commitment_len;
    bool           peer_commitment_pending;
    uint8_t        reconnect_attempts;   /* reconnect attempts after disconnect */
    uint32_t       discovery_start_ts;
    uint32_t       pairing_start_ts;
    bool           commitment_verified;
    bool           handoff_ready;              /* Signal to move to Phase 3 */
    bool           discovered;                 /* Peer discovered during scan */
    uint8_t        peer_name[16];              /* NUL not guaranteed; use peer_name_len */
    uint8_t        peer_name_len;
    int8_t         peer_rssi;                  /* raw, last received */
    int16_t        peer_rssi_smooth_x8;        /* EMA ×8 precision */
    uint32_t       last_seen_ms;               /* ms since boot of last scan hit */
    uint32_t       gatt_connected_since_ms;    /* ms when GATT connection established (for age tracking) */
    uint32_t       accumulated_conn_ms;        /* accumulated connected ms for current cached peer */
    bool           pending_verify_result;      /* queued verification result pending GATTC availability */
    uint8_t        pending_verify_status;      /* queued verification status value (VerificationStatus_t) */
    BlePeerRecord_t peer_record;
    uint32_t       scan_seen_count;            /* total advertisements observed */
    uint8_t        scan_hit_count;             /* total hits recorded for this peer */
    uint32_t       adv_packet_count;           /* number of advertisement packets sent (for UI feedback) */
    bool           is_advertising;
    bool           is_scanning;
    uint8_t        gattc_if;                   /* client interface (CEEPEW_GATT_IF_NONE if unset) */
    uint8_t        gatts_if;                   /* server interface (CEEPEW_GATT_IF_NONE if unset) */
    uint16_t       conn_id;                    /* active connection id */
    uint16_t       service_start_handle;
    uint16_t       service_end_handle;
    uint16_t       gattc_char_handle;          /* peer characteristic handle */
    uint16_t       gatts_char_handle;          /* local characteristic handle */
    uint16_t       gattc_verify_char_handle;   /* peer verification status characteristic (0xFFF2) */
    uint16_t       gatts_verify_char_handle;   /* local verification status characteristic (0xFFF2) */
    bool           gattc_registered;
    bool           gatts_registered;
    bool           gattc_connected;
    bool           gatts_connected;
    bool           connecting;
    bool           commitment_write_pending;
    VerificationStatus_t verification_result;  /* Local verification result */
    VerificationStatus_t peer_verification_result; /* Peer's verification result */
    bool           peer_verification_pending;  /* Waiting for peer verification result */
    uint32_t       verification_timeout_ms;    /* Deadline for receiving verification result */
    uint32_t       verify_fail_count;
    bool           ready_for_chat;              /* Local readiness flag after commitment verified */
    bool           peer_ready_for_chat;         /* Peer's readiness flag received via GATT */
    uint32_t       peer_ready_timestamp_ms;     /* When peer_ready_for_chat was set */
} BleContext_t;

extern BleContext_t g_ble_ctx;

/* Initialize BLE subsystem (call once after WiFi/hardware init) */
CeePewErr_t transport_ble_init(void);

/* Start BLE advertisement (Phase 1 initiator). Advertises MAC + device name. */
CeePewErr_t transport_ble_start_advertising(void);

/* Start BLE scan (Phase 1 responder). Scans for peer advertisements. */
CeePewErr_t transport_ble_start_scan(void);

/* Restart discovery from a clean slate (disconnect, clear peer cache, re-arm adv/scan). */
CeePewErr_t transport_ble_restart_discovery_session(void);

/* Get current BLE state */
BleState_t transport_ble_get_state(void);

/* Get discovered peer for UI freshness-gated rendering only. */
const BlePeerRecord_t *transport_ble_get_peer(void);

/* Get discovered peer cache regardless of freshness (pairing control path). */
const BlePeerRecord_t *transport_ble_get_peer_cached(void);

/* True if a peer has been discovered and cached for pairing. */
bool transport_ble_has_peer_cached(void);

/* Clear cached discovery peer state (used when restarting pairing). */
void transport_ble_clear_discovery_peer_state(void);

/* Initiate BLE connection to discovered peer (Phase 1 → Phase 2) */
CeePewErr_t transport_ble_connect_to_peer(const uint8_t peer_mac[6]);

/* Retry a previously failed scan start (called periodically from task_session) */
CeePewErr_t transport_ble_retry_scan_if_needed(void);

/* Retry pending commitment writes/handshake retries (called from task_session tick). */
CeePewErr_t transport_ble_retry_commitment_if_needed(void);

/* Exchange commitment hash via GATT (Phase 2).
 * Sets g_ble_ctx.commitment_digest and waits for peer to confirm. */
CeePewErr_t transport_ble_exchange_commitment(const uint8_t *commitment_digest, uint8_t len);

/* Verify peer's commitment matches ours (call after exchange completes) */
CeePewErr_t transport_ble_verify_commitment(const uint8_t *peer_digest, uint8_t len);

/* Verify any commitment buffered by the responder before its local commitment was ready. */
CeePewErr_t transport_ble_verify_pending_commitment(void);

/* Check verification result with timeout (called from task_session tick).
 * Returns CEEPEW_OK if verification passed or is still pending.
 * Returns CEEPEW_ERR_AUTH_FAIL if verification failed (mismatch).
 * Returns CEEPEW_ERR_BUSY if verification timeout not yet reached.
 * Returns CEEPEW_ERR_HW for other failures. */
CeePewErr_t transport_ble_check_verification_result(void);

/* Get current verification status (for UI display if needed) */
VerificationStatus_t transport_ble_get_verification_status(void);

/* Set local readiness flag after commitment verified (for sync handshake) */
void transport_ble_set_ready_for_chat(void);

/* Check if peer has signaled readiness (call to verify both sides ready) */
bool transport_ble_peer_ready_for_chat(void);

/* Check if both local and peer are ready for chat transition */
bool transport_ble_both_ready_for_chat(void);

/* Check if handoff to Phase 3 is ready (commitment verified + connection stable) */
bool transport_ble_handoff_ready(void);

/* Get peer age in ms computed from accumulated_conn_ms + current connection delta
 * or last_seen when disconnected. Safe to call from UI (thread-safe). */
uint32_t transport_ble_get_peer_age_ms(void);

/* Reset accumulated connection time (safe API) */
void transport_ble_reset_accumulated_conn_ms(void);

/* Gracefully close BLE connection and move to Phase 3 (ESP-NOW handoff) */
CeePewErr_t transport_ble_disconnect(void);

/* Deinit BLE subsystem (call on session end) */
CeePewErr_t transport_ble_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_BLE_H */
