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

CeePewErr_t crypto_ctx_init(void);
CeePewErr_t crypto_ctx_destroy(void);
CeePewErr_t crypto_mutex_init(void);
CeePewErr_t crypto_mutex_lock(void);
CeePewErr_t crypto_mutex_unlock(void);

#endif /* CRYPTO_CTX_H */
