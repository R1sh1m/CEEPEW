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

#ifdef __cplusplus
extern "C" {
#endif

/* BLE connection states */
typedef enum {
    BLE_IDLE = 0U,
    BLE_ADVERTISING,
    BLE_SCANNING,
    BLE_CONNECTED,
    BLE_PAIRING,
    BLE_DONE
} BleState_t;

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
    uint8_t        commitment_digest[8];      /* Truncated SHA256(session_code) */
    uint32_t       discovery_start_ts;
    uint32_t       pairing_start_ts;
    bool           commitment_verified;
    bool           handoff_ready;              /* Signal to move to Phase 3 */
    bool           discovered;                 /* Peer discovered during scan */
    uint8_t        peer_name[16];              /* NUL not guaranteed; use peer_name_len */
    uint8_t        peer_name_len;
    int8_t         peer_rssi;
} BleContext_t;

extern BleContext_t g_ble_ctx;

/* Initialize BLE subsystem (call once after WiFi/hardware init) */
CeePewErr_t transport_ble_init(void);

/* Start BLE advertisement (Phase 1 initiator). Advertises MAC + device name. */
CeePewErr_t transport_ble_start_advertising(void);

/* Start BLE scan (Phase 1 responder). Scans for peer advertisements. */
CeePewErr_t transport_ble_start_scan(void);

/* Get current BLE state */
BleState_t transport_ble_get_state(void);

/* Get discovered peer (after scan completes). Returns NULL if no peer found. */
const BlePeerRecord_t *transport_ble_get_peer(void);

/* Initiate BLE connection to discovered peer (Phase 1 → Phase 2) */
CeePewErr_t transport_ble_connect_to_peer(const uint8_t peer_mac[6]);

/* Exchange commitment hash via GATT (Phase 2). 
 * Sets g_ble_ctx.commitment_digest and waits for peer to confirm. */
CeePewErr_t transport_ble_exchange_commitment(const uint8_t commitment_digest[8]);

/* Verify peer's commitment matches ours (call after exchange completes) */
CeePewErr_t transport_ble_verify_commitment(const uint8_t peer_digest[8]);

/* Check if handoff to Phase 3 is ready (commitment verified + connection stable) */
bool transport_ble_handoff_ready(void);

/* Gracefully close BLE connection and move to Phase 3 (ESP-NOW handoff) */
CeePewErr_t transport_ble_disconnect(void);

/* Deinit BLE subsystem (call on session end) */
CeePewErr_t transport_ble_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_BLE_H */
