/* main/session_msgstore.h
 *
 * Session message store with TTL enforcement and nonce exhaustion handling.
 * Maintains a circular buffer of up to CEEPEW_MAX_MESSAGES recent messages.
 * Each message has a creation timestamp; messages older than CEEPEW_MSG_TTL_S
 * are automatically expired and wiped from the buffer.
 * Nonce exhaustion (when session reaches CEEPEW_NONCE_HARD_LIMIT) triggers
 * automatic session wipe and forces a new pairing cycle.
 */

#ifndef SESSION_MSGSTORE_H
#define SESSION_MSGSTORE_H

#include "ceepew_config.h"
#include "hal_ui_types.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MSG_STATUS_PENDING = 0U,
    MSG_STATUS_SENT = 1U,
    MSG_STATUS_DELIVERED = 2U,
    MSG_STATUS_READ = 3U
} MsgDeliveryStatus_t;

/* Message metadata: timestamp, length, flags */
typedef struct {
    uint32_t created_at;          /* Unix timestamp (seconds) */
    uint16_t payload_len;         /* Plaintext size before encryption */
    uint8_t  dir;                 /* 0=RX, 1=TX */
    uint8_t  delivery_status;     /* MsgDeliveryStatus_t (0=pending, 1=sent, 2=delivered, 3=read) */
    uint8_t  ttl_seconds;         /* Self-destruct timer, 0 = default CEEPEW_MSG_TTL_S */
} MsgMeta_t;

/* In-store message: metadata + plaintext payload */
typedef struct {
    MsgMeta_t meta;
    char      plaintext[CEEPEW_MAX_MSG_BYTES + 1U];  /* Null-terminated plaintext */
    bool      read;                                  /* Message read status: true=read, false=unread */
} StoredMsg_t;

/* Message store context: circular buffer + expiration tracking */
typedef struct {
    StoredMsg_t messages[CEEPEW_MAX_MESSAGES];
    uint8_t     head;             /* Index of oldest message */
    uint8_t     tail;             /* Index of next free slot */
    uint8_t     count;            /* Number of valid messages */
    uint32_t    last_wipe_ts;     /* Timestamp of last auto-wipe */
    bool        nonce_exhausted;  /* Flag: session needs re-pairing */
} MsgStore_t;

extern MsgStore_t g_msg_store;

/* Initialize message store (call once after session_phase2_derive_key) */
CeePewErr_t msg_store_init(void);

/* Store a received or transmitted message (plaintext). Expires old entries on overflow. */
CeePewErr_t msg_store_add(const uint8_t *plaintext, uint16_t plaintext_len, uint8_t direction);

/* Get message at index (0=oldest, count-1=newest). Returns NULL if index out of bounds. */
const StoredMsg_t *msg_store_get(uint8_t index);

/* Expire messages older than CEEPEW_MSG_TTL_S. Called periodically or on storage pressure. */
CeePewErr_t msg_store_expire_old(void);

/* Securely wipe all messages (used on session end or nonce exhaustion). */
CeePewErr_t msg_store_wipe_all(void);

/* Check nonce counter and mark store as exhausted if limit reached.
 * Call this AFTER every encryption operation that increments nonce. */
CeePewErr_t msg_store_check_nonce_exhaustion(uint64_t nonce_counter);

/* Get current message count */
uint8_t msg_store_count(void);

/* Get number of unread messages (dir == 0 && read == false) */
uint8_t msg_store_unread_count(void);

/* Mark a message at index as read. Returns CEEPEW_OK or CEEPEW_ERR_BOUNDS if index invalid. */
CeePewErr_t msg_store_mark_read(uint8_t index);

/* Update delivery status (pending, sent, delivered, read) for a message. */
CeePewErr_t msg_store_update_delivery_status(uint8_t index, MsgDeliveryStatus_t status);

/* Securely burn/wipe all stored messages immediately (alias for link drop / burn feature). */
CeePewErr_t msg_store_burn_all(void);

/* Diagnostic accessor: last secure-wipe time in milliseconds since epoch */
uint32_t session_get_last_wipe_ms(void);

#endif /* SESSION_MSGSTORE_H */
