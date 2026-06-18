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
    uint8_t  box_privkey[32U];         /* Ephemeral X25519 private key (per-session) */
    uint8_t  box_pubkey[32U];          /* Ephemeral X25519 public key (exchanged via BLE) */
    uint8_t  peer_box_pubkey[32U];     /* Peer's X25519 public key (received via BLE) */
    bool     peer_box_pubkey_valid;    /* true once peer_box_pubkey has been received */
    uint8_t  session_id[8U];
    uint8_t  reserved[8U];
} CryptoCtx_t;

extern CryptoCtx_t g_crypto_ctx;
extern SemaphoreHandle_t g_crypto_mutex;

/* Initialize the global crypto context and PSA crypto subsystem.
 * Must be called once at boot before any crypto operations. */
CeePewErr_t crypto_ctx_init(void);

/* Securely zero the global crypto context and destroy the crypto mutex.
 * Call on session end or device wipe. */
CeePewErr_t crypto_ctx_destroy(void);

/* Initialize the global crypto mutex (lazy, idempotent). */
CeePewErr_t crypto_mutex_init(void);

/* Acquire the global crypto mutex (blocking). */
CeePewErr_t crypto_mutex_lock(void);

/* Release the global crypto mutex. */
CeePewErr_t crypto_mutex_unlock(void);

#endif /* CRYPTO_CTX_H */
