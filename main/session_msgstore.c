/* main/session_msgstore.c
 * Implementation of session message store with TTL enforcement.
 * Automatically expires messages older than CEEPEW_MSG_TTL_S.
 * Tracks nonce counter and triggers exhaustion flag when limit is reached.
 */

#include "session_msgstore.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "esp_timer.h"
#include <string.h>

/* Design note: The message store is a simple circular buffer with expiration.
   Instead of maintaining a background task, expiration is checked opportunistically
   on every add and via explicit msg_store_expire_old() calls. The nonce_exhausted
   flag is set when the counter reaches CEEPEW_NONCE_HARD_LIMIT; once set, it
   persists until msg_store_wipe_all() clears it (which happens on session end). */
MsgStore_t g_msg_store = {0};
CeePewErr_t msg_store_init(void){
    /* Ensure store starts empty and configured limits are sane */
    CEEPEW_ASSERT(g_msg_store.count == 0U, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_MAX_MESSAGES > 0U, CEEPEW_ERR_PARAM);
    memset(&g_msg_store, 0U, sizeof(MsgStore_t));
    g_msg_store.head = 0U;
    g_msg_store.tail = 0U;
    g_msg_store.count = 0U;
    g_msg_store.last_wipe_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);
    g_msg_store.nonce_exhausted = false;
    /* Sanity assert after init */
    CEEPEW_ASSERT(g_msg_store.count == 0U, CEEPEW_ERR_INTERNAL);
    return CEEPEW_OK;
}

CeePewErr_t msg_store_add(const uint8_t *encrypted_data, uint16_t encrypted_len,
                          uint16_t plaintext_len, uint8_t direction)
{
    CEEPEW_ASSERT(encrypted_data != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(encrypted_len > 0U && encrypted_len <= sizeof(g_msg_store.messages[0].encrypted),
                  CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(plaintext_len > 0U && plaintext_len <= CEEPEW_MAX_MSG_BYTES,
                  CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(direction <= 1U, CEEPEW_ERR_PARAM);

    /* Expire old messages to make room if needed */
    if (g_msg_store.count >= CEEPEW_MAX_MESSAGES) {
        CeePewErr_t err = msg_store_expire_old();
        CEEPEW_ASSERT(err == CEEPEW_OK, err);

        /* If still full after expiration, securely zero oldest message then drop it */
        if (g_msg_store.count >= CEEPEW_MAX_MESSAGES) {
            volatile uint8_t *pold = (volatile uint8_t *)&g_msg_store.messages[g_msg_store.head];
            for (uint32_t z = 0U; z < sizeof(StoredMsg_t); z++) { pold[z] = 0U; }
            __asm__ __volatile__("" ::: "memory");

            g_msg_store.head = (g_msg_store.head + 1U) % CEEPEW_MAX_MESSAGES;
            g_msg_store.count--;
        }
    }

    /* Write message at tail */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    g_msg_store.messages[g_msg_store.tail].meta.created_at = now_s;
    g_msg_store.messages[g_msg_store.tail].meta.payload_len = plaintext_len;
    g_msg_store.messages[g_msg_store.tail].meta.dir = direction;
    g_msg_store.messages[g_msg_store.tail].meta.reserved = 0U;

    memcpy(g_msg_store.messages[g_msg_store.tail].encrypted, encrypted_data, encrypted_len);
    /* Zero-pad the rest of the ciphertext buffer */
    if (encrypted_len < sizeof(g_msg_store.messages[g_msg_store.tail].encrypted)) {
        memset(g_msg_store.messages[g_msg_store.tail].encrypted + encrypted_len,
               0U,
               sizeof(g_msg_store.messages[g_msg_store.tail].encrypted) - encrypted_len);
    }

    g_msg_store.tail = (g_msg_store.tail + 1U) % CEEPEW_MAX_MESSAGES;
    g_msg_store.count++;

    /* Sanity asserts */
    CEEPEW_ASSERT(g_msg_store.count <= CEEPEW_MAX_MESSAGES, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(g_msg_store.tail < CEEPEW_MAX_MESSAGES, CEEPEW_ERR_INTERNAL);

    return CEEPEW_OK;
}

const StoredMsg_t *msg_store_get(uint8_t index)
{
    /* Sanity asserts for pointer-returning function */
    CEEPEW_ASSERT_PTR(g_msg_store.count <= CEEPEW_MAX_MESSAGES, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT_PTR(index < CEEPEW_MAX_MESSAGES, CEEPEW_ERR_PARAM);

    if (index >= g_msg_store.count) { return NULL; }

    uint8_t msg_idx = (g_msg_store.head + index) % CEEPEW_MAX_MESSAGES;
    return &g_msg_store.messages[msg_idx];
}

CeePewErr_t msg_store_expire_old(void)
{
    CEEPEW_ASSERT(g_msg_store.count <= CEEPEW_MAX_MESSAGES, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(CEEPEW_MSG_TTL_S > 0U, CEEPEW_ERR_PARAM);

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);

    /* Remove messages from head while expired (oldest entries expire first) */
    /* loop bound: CEEPEW_MAX_MESSAGES (compile-time constant) */
    for (uint8_t i = 0U; i < CEEPEW_MAX_MESSAGES && g_msg_store.count > 0U; i++) {
        const StoredMsg_t *msg = msg_store_get(0U);
        if (msg == NULL) { break; }

        uint32_t age_s = (now_s > msg->meta.created_at)
                       ? (now_s - msg->meta.created_at)
                       : 0U;

        if (age_s > CEEPEW_MSG_TTL_S) {
            /* Securely zero this message before removing */
            volatile uint8_t *p = (volatile uint8_t *)&g_msg_store.messages[g_msg_store.head];
            for (uint32_t j = 0U; j < sizeof(StoredMsg_t); j++) {
                p[j] = 0U;
            }
            __asm__ __volatile__("" ::: "memory");

            g_msg_store.head = (g_msg_store.head + 1U) % CEEPEW_MAX_MESSAGES;
            g_msg_store.count--;
        } else {
            break;  /* Remaining messages are younger */
        }
    }

    g_msg_store.last_wipe_ts = now_s;
    return CEEPEW_OK;
}

CeePewErr_t msg_store_wipe_all(void)
{
    /* Sanity asserts */
    CEEPEW_ASSERT(sizeof(g_msg_store.messages) > 0U, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(g_msg_store.count <= CEEPEW_MAX_MESSAGES, CEEPEW_ERR_INTERNAL);

    /* Securely zero all message storage */
    volatile uint8_t *p = (volatile uint8_t *)g_msg_store.messages;
    /* loop bound: sizeof(g_msg_store.messages) = CEEPEW_MAX_MESSAGES * sizeof(StoredMsg_t) */
    for (uint32_t i = 0U; i < sizeof(g_msg_store.messages); i++) {
        p[i] = 0U;
    }
    __asm__ __volatile__("" ::: "memory");

    g_msg_store.head = 0U;
    g_msg_store.tail = 0U;
    g_msg_store.count = 0U;
    g_msg_store.nonce_exhausted = false;

    /* Post-conditions */
    CEEPEW_ASSERT(g_msg_store.count == 0U, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(g_msg_store.head == 0U && g_msg_store.tail == 0U, CEEPEW_ERR_INTERNAL);

    return CEEPEW_OK;
}

CeePewErr_t msg_store_check_nonce_exhaustion(uint64_t nonce_counter)
{
    CEEPEW_ASSERT(nonce_counter <= CEEPEW_NONCE_HARD_LIMIT, CEEPEW_ERR_NONCE_EXHAUSTED);
    CEEPEW_ASSERT(g_msg_store.count <= CEEPEW_MAX_MESSAGES, CEEPEW_ERR_INTERNAL);

    if (nonce_counter >= CEEPEW_NONCE_HARD_LIMIT) {
        g_msg_store.nonce_exhausted = true;
        return CEEPEW_ERR_NONCE_EXHAUSTED;
    }
    return CEEPEW_OK;
}

uint8_t msg_store_count(void)
{
    CEEPEW_ASSERT(g_msg_store.count <= CEEPEW_MAX_MESSAGES,
                  CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(g_msg_store.head < CEEPEW_MAX_MESSAGES,
                  CEEPEW_ERR_INTERNAL);
    return g_msg_store.count;
}

uint32_t msg_store_usage_bytes(void)
{
    CEEPEW_ASSERT(g_msg_store.count <= CEEPEW_MAX_MESSAGES, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(sizeof(StoredMsg_t) > 0U, CEEPEW_ERR_INTERNAL);
    /* Exact usage: count * size of a stored message */
    return (uint32_t)g_msg_store.count * (uint32_t)sizeof(StoredMsg_t);
}

uint32_t session_get_last_wipe_ms(void)
{
    /* Return last wipe timestamp in milliseconds for diagnostics/UI. Stored value is seconds. */
    return (uint32_t)(g_msg_store.last_wipe_ts * 1000U);
}
