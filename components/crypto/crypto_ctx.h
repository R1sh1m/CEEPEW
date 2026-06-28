/* components/crypto/crypto_ctx.h */

#ifndef CRYPTO_CTX_H
#define CRYPTO_CTX_H

#include "ceepew_assert.h"
#include "ceepew_config.h"

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool     session_active;
    uint8_t  ascon_key[CEEPEW_SESSION_KEY_BYTES];
    uint8_t  box_seed[32U];            /* HKDF derivation seed (retained for key expansion) */
    uint8_t  box_privkey[32U];         /* Ephemeral X25519 private key (per-session, BLE-derived) */
    uint8_t  box_pubkey[32U];          /* Ephemeral X25519 public key (exchanged via BLE) */
    uint8_t  peer_box_pubkey[32U];     /* Peer's X25519 public key (received via BLE) */
    bool     peer_box_pubkey_valid;    /* true once peer_box_pubkey has been received */
    uint8_t  session_id[8U];
    uint8_t  reserved[8U];

    /* PFS (Perfect Forward Secrecy) — ephemeral Curve25519 ECDH over ESP-NOW */
    uint8_t  pfs_privkey[32U];         /* PFS ephemeral private key */
    uint8_t  pfs_pubkey[32U];          /* PFS ephemeral public key */
    uint8_t  pfs_peer_pubkey[32U];     /* Peer's PFS ephemeral public key */
    bool     pfs_peer_pubkey_valid;    /* true once peer's PFS key received */
    uint8_t  pfs_shared_secret[32U];   /* Curve25519 shared secret */
    uint8_t  pfs_ascon_key[16U];       /* HKDF-derived Ascon key from PFS secret */
    bool     pfs_active;               /* true once PFS key exchange complete */
} CryptoCtx_t;

extern CryptoCtx_t g_crypto_ctx;
extern SemaphoreHandle_t g_crypto_mutex;

/* Init global crypto context + PSA. Call once at boot. */
CeePewErr_t crypto_ctx_init(void);

/* Secure zero context + destroy mutex. Call on session end/wipe. */
CeePewErr_t crypto_ctx_destroy(void);

/* Lazy, idempotent mutex init. */
CeePewErr_t crypto_mutex_init(void);

/* Blocking mutex acquire. */
CeePewErr_t crypto_mutex_lock(void);

/* Mutex release. */
CeePewErr_t crypto_mutex_unlock(void);

/* Derive ESP-NOW PMK from session key. Symmetric by design (both peers share key). */
CeePewErr_t crypto_espnow_derive_pmk(uint8_t pmk_out[16]);

/* Derive ESP-NOW LMK for a peer. Incorporates peer WiFi MAC for per-peer keys. */
CeePewErr_t crypto_espnow_derive_lmk(const uint8_t peer_wifi_mac[6], uint8_t lmk_out[16]);

#endif /* CRYPTO_CTX_H */
