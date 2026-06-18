/* components/transport/transport_ble.h
 *
 * CEE-PEW BLE Transport (Phases 1-2: Discovery & Pairing)
 *
 * PHASE 1 (DISCOVERY):
 *   - Local device advertises its MAC, device name, and capability flags
 *   - Both peers scan for each other's advertisements
 *   - Once found, exchange MAC addresses and move to Phase 2
 *
 * PHASE 2 (PAIRING) — HYBRID GATED-GATT:
 *   1. Both peers broadcast a 16-byte truncated SHA-256 commitment in
 *      SCAN_RSP (manufacturer AD 0xCEEE subtype 0x50), preceded by a
 *      2-byte monotonic nonce (Bug 2 fix). The high bit (bit 15) of the
 *      nonce is the "GATT-ready" flag (set when local commitment_verified
 *      transitions 0→1; see Item 3 of Phase 7 plan).
 *   2. Once both beacons are received and the truncated commitments match,
 *      the lower-MAC peer (initiator) opens a brief GATTC connection,
 *      requests MTU 247 upfront, and writes an Ascon-128-AEAD-encrypted
 *      48B payload (32B ct + 16B tag) to the responder's 0xFFF3
 *      characteristic. Encryption key is HKDF-SHA256(session_code, ...)
 *      so only peers that already share the session code can decrypt.
 *   3. The initiator's gate requires ALL of: phase == GATT_IDENTITY,
 *      commitment_verified, peer_gatt_ready, is_initiator, attempts<5.
 *      The 2s watchdog fires if GATTC_OPEN_EVT never arrives after the
 *      gate opens — that catches the case where the responder's GATT
 *      server is wedged or the peer signal was lost mid-connection.
 *   4. If the brief GATT exchange fails after CEEPEW_MAX_RECONNECT_ATTEMPTS,
     *      pairing proceeds without sign_pk; the first chat frame's
     *      signature will be unverifiable.
 *   5. Once handoff_ready is set, ESP-NOW takes over for Phase 3 (active
 *      session) and the BLE link is closed.
 *
 * NOTE: BLE is ONLY used for pairing discovery and the optional sign_pk
 * handoff.  All bulk data flows on ESP-NOW.
 */

#ifndef TRANSPORT_BLE_H
#define TRANSPORT_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
#define CEEPEW_PAIRING_PEER_KEEP_MS      180000U /* 3 min during pairing flow — code
                                                  * entry can take 30-60s; do not
                                                  * yank the peer mid-typing. */

/* Peer discovery record (found during scan) */
typedef struct {
    uint8_t  peer_mac[6];
    int8_t   rssi;
    uint32_t seen_at;
    uint8_t  name[16];
    uint8_t  name_len;
} BlePeerRecord_t;

/* BLE pairing session context */
typedef struct {
    BleState_t     state;
    uint8_t        local_mac[6];
    uint8_t        peer_mac[6];
    esp_ble_addr_type_t peer_addr_type;
    uint8_t        commitment_digest[CEEPEW_COMMITMENT_BYTES];
    uint8_t        local_commitment_len; /* 0 until set, CEEPEW_COMMITMENT_BYTES after */
    /* ── Beacon commitment exchange (SCAN_RSP AD 0xCEEE/0x50) ──────── */
    bool           commitment_beacon_active;   /* scan response carries our commitment */
    uint8_t        adv_commitment[CEEPEW_COMMITMENT_ADV_BYTES]; /* cached local copy */
    bool           peer_commitment_via_adv;    /* peer commit arrived via scan beacon */
    bool           sign_pk_received;           /* peer sign_pk delivered over 0xFFF3 */
    bool           box_pubkey_received;        /* peer box_pubkey delivered over 0xFFF3 */
    bool           is_initiator_role;          /* true = we opened the GATTC connection */
    uint8_t        pending_peer_commitment[CEEPEW_COMMITMENT_ADV_BYTES];
    uint8_t        pending_peer_commitment_len;
    bool           peer_commitment_pending;
    uint8_t        reconnect_attempts;         /* sign_pk GATT retry counter (≤ CEEPEW_MAX_RECONNECT_ATTEMPTS) */
    uint32_t       discovery_start_ts;
    uint32_t       pairing_start_ts;
    bool           commitment_verified;
    bool           handoff_ready;              /* Beacon match — ready to derive key */
    bool           discovered;                 /* Peer discovered during scan */
    uint8_t        peer_name[16];              /* NUL not guaranteed; use peer_name_len */
    uint8_t        peer_name_len;
    int8_t         peer_rssi;                  /* raw, last received */
    int16_t        peer_rssi_smooth_x8;        /* EMA ×8 precision */
    uint32_t       last_seen_ms;               /* ms since boot of last scan hit */
    uint32_t       gatt_connected_since_ms;    /* ms when GATT connection established (for age tracking) */
    uint32_t       accumulated_conn_ms;        /* accumulated connected ms for current cached peer */
    BlePeerRecord_t peer_record;
    uint32_t       scan_seen_count;            /* total advertisements observed */
    uint16_t       scan_hit_count;             /* total hits recorded for this peer */
    uint32_t       adv_packet_count;           /* number of advertisement packets sent (for UI feedback) */
    bool           is_advertising;
    bool           is_scanning;
    uint8_t        gattc_if;                   /* client interface (CEEPEW_GATT_IF_NONE if unset) */
    uint8_t        gatts_if;                   /* server interface (CEEPEW_GATT_IF_NONE if unset) */
    uint16_t       conn_id;                    /* active connection id */
    uint16_t       service_start_handle;
    uint16_t       service_end_handle;
    uint16_t       gattc_sign_pk_char_handle;  /* peer's 0xFFF3 characteristic handle */
    uint16_t       gatts_sign_pk_char_handle;  /* local 0xFFF3 characteristic handle */
    bool           gattc_registered;
    bool           gatts_registered;
    bool           gattc_connected;
    bool           gatts_connected;
    bool           connecting;
    bool           ready_for_chat;              /* Local readiness flag after key derivation */
    bool           peer_ready_for_chat;         /* Peer's readiness flag (set by verify_pending_commitment_unlocked) */
    uint32_t       peer_ready_timestamp_ms;     /* When peer_ready_for_chat was set */
    /* ── Beacon replay defense (Bug 2 fix) ── */
    uint16_t       beacon_nonce_local;          /* Last counter we put in our beacon (bits 0-14 only) */
    uint16_t       beacon_nonce_peer_counter_max; /* Highest 15-bit counter accepted from peer */
    /* ── Hybrid-GATT gating (Phase 7) ── */
    bool           peer_gatt_ready;             /* Peer's beacon bit 15 set: GATT open safe */
    uint16_t       gattc_mtu;                   /* Negotiated GATTC MTU (default 23) */
    bool           gattc_sign_pk_mtu_negotiated;/* ESP_GATTC_CFG_MTU_EVT received */
    bool           gattc_sign_pk_write_pending; /* Search-cmpl wrote, awaiting ESP_GATTC_WRITE_CHAR_EVT */
    bool           pending_sign_pk_write;        /* Deferred write awaiting MTU negotiation */
    uint8_t        pending_sign_pk_encrypted[80];/* Buffered 80B payload for deferred write */
    bool           reverse_gattc_pending;       /* Responder: reverse GATTC for sign_pk exchange pending */
    bool           initiator_sign_pk_sent;      /* true after our sign_pk has been written to peer */
    /* GATTS MTU tracking (for responder sign_pk receive) */
    uint16_t       gatts_mtu;                   /* Negotiated GATTS MTU (default 23) */
    bool           gatts_sign_pk_mtu_negotiated;/* ESP_GATTS_MTU_EVT received */
} BleContext_t;

extern BleContext_t g_ble_ctx;

/* Initialize BLE subsystem (call once after WiFi/hardware init) */
CeePewErr_t transport_ble_init(void);

/* Set the local MAC used for the GATT sign_pk encryption key derivation.
 * Must be called before transport_ble_init() OR before any GATT write.
 * The MAC must be the same value passed to session_phase1_init(). If
 * unset, transport_ble_init() falls back to esp_efuse_mac_get_default().
 */
void transport_ble_set_local_mac(const uint8_t mac[6]);

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

/* Clear only the pending commitment buffer (used when starting a new
 * pairing attempt to prevent stale beacons from a prior session from
 * poisoning the new attempt). */
void transport_ble_clear_pending_commitment(void);

/*
 * Encode `len` bytes of `commitment` into the BLE scan-response payload as a
 * manufacturer-specific AD record (company 0xCEEE, subtype 0x50, plus the
 * complete local name).  Does NOT restart advertising — Bluedroid serves
 * the updated payload on the next SCAN_REQ automatically.
 * `len` must be in [1, CEEPEW_COMMITMENT_ADV_BYTES].
 * Must be called only after advertising is active.
 */
CeePewErr_t transport_ble_set_commitment_beacon(const uint8_t *commitment, uint8_t len);

/* Initiate BLE connection to discovered peer (Phase 1 → Phase 2). */
CeePewErr_t transport_ble_connect_to_peer(const uint8_t peer_mac[6]);

/* Retry a previously failed scan start (called periodically from task_session) */
CeePewErr_t transport_ble_retry_scan_if_needed(void);

/* Verify peer's commitment matches ours (call after exchange completes) */
CeePewErr_t transport_ble_verify_commitment(const uint8_t *peer_digest, uint8_t len);

/* Verify any commitment buffered by the responder before its local commitment was ready. */
CeePewErr_t transport_ble_verify_pending_commitment(void);

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

/* Check if BLE subsystem is initialised */
bool transport_ble_is_initialised(void);

/* Adjust BLE scan duty cycle (interval/window).
 * Low duty cycle during discovery saves ~40% BLE power.
 * High duty cycle during pairing for fast connection. */
CeePewErr_t transport_ble_set_scan_duty_cycle(uint16_t interval_ms, uint16_t window_ms);

/* ========================================================================== */
/* Event-Driven Pairing Architecture (Deterministic, Non-Blocking)             */
/* ========================================================================== */

/* Pairing phase identifiers used by the supervisor watchdog. */
typedef enum {
    PAIRING_PHASE_IDLE = 0U,
    PAIRING_PHASE_DISCOVERY,
    PAIRING_PHASE_BEACON_PAIRING,        /* connectionless commitment exchange via SCAN_RSP */
    PAIRING_PHASE_GATT_IDENTITY,         /* Hybrid-GATT: beacons matched, awaiting GATTC connect */
    PAIRING_PHASE_SIGN_PK_EXCHANGE,      /* brief GATTS connection for 48B Ascon-encrypted sign_pk */
    PAIRING_PHASE_FAILED
} PairingPhase_t;

/* Pairing events pushed to the FreeRTOS event queue.
 * The BLE event handlers MUST NOT block — they only push events.
 * The PairingSupervisor task processes them on Core 1. */
typedef enum {
    PAIRING_EVENT_NONE = 0U,
    PAIRING_EVENT_SIGN_PK_RECEIVED,     /* Peer's sign_pk delivered over 0xFFF3 */
    PAIRING_EVENT_PHASE_TIMEOUT,        /* Supervisor detected a stall */
    PAIRING_EVENT_RADIO_RESTART         /* Supervisor forcing radio restart */
} PairingEventType_t;

typedef struct {
    PairingEventType_t type;
    uint32_t          timestamp_ms;
    uint8_t           payload[16];  /* event-specific data (e.g., peer_mac[6]) */
    uint8_t           payload_len;
} PairingEvent_t;

/* PairingContext — the single source of truth for the pairing state machine.
 * All fields are protected by ble_ctx_lock; reads/writes must hold the lock. */
typedef struct {
    PairingPhase_t  phase;
    uint32_t        phase_entered_ms;    /* timestamp when current phase started */
    uint32_t        last_event_ms;       /* timestamp of last event applied */
} PairingContext_t;

extern PairingContext_t s_pairing_ctx;

/* Pairing event queue — declared extern so other tasks (e.g., task_session)
 * can push events when needed. Created during transport_ble_init(). */
extern QueueHandle_t g_pairing_event_queue;

/* Push a pairing event from any context. Non-blocking.
 * If the queue is full, the event is dropped and a warning is logged. */
void transport_ble_post_event(PairingEventType_t type,
                              const uint8_t *payload,
                              uint8_t payload_len);

/* Get the current pairing phase (read-only snapshot, thread-safe).
 * Used by the UI task to drive RGB feedback and on-screen status. */
PairingPhase_t transport_ble_get_phase(void);

/* True while the PairingSupervisor is performing a forced recovery
 * (PHASE_TIMEOUT or RADIO_RESTART). Cleared once a stable phase
 * (DISCOVERY / FAILED) is re-entered. */
bool transport_ble_is_recovering(void);

/* Debug hook: post a synthetic PHASE_TIMEOUT event so the recovery path
 * can be exercised from the UI without needing a real radio stall. */
void transport_ble_debug_trigger_timeout(void);

/* Notify the supervisor that a phase transition occurred.
 * Updates s_pairing_ctx.phase and resets the phase_entered_ms timer. */
void transport_ble_enter_phase(PairingPhase_t phase);

/* Unified connection-lost handler. Called from BOTH gattc_event_handler
 * and gatts_event_handler for ESP_GATTC_DISCONNECT_EVT /
 * ESP_GATTS_DISCONNECT_EVT. Non-blocking — only pushes a phase-timeout
 * event so the supervisor can re-evaluate the current phase. */
void transport_ble_handle_connection_lost(void);

/* Start the PairingSupervisor task. Call once from transport_ble_init(). */
CeePewErr_t transport_ble_supervisor_start(void);

/* Stop the PairingSupervisor task. Call from transport_ble_deinit(). */
void transport_ble_supervisor_stop(void);

/* Phase → RGB pattern mapping (per spec table in
 * Technical Specification §4 — RGB LED Patterns). */
#include "hal_rgb.h"
RgbPattern_t transport_ble_phase_to_rgb(PairingPhase_t phase);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_BLE_H */
