/* main/session_msgstore.c
 * Implementation of session message store with TTL enforcement.
 * Automatically expires messages older than CEEPEW_MSG_TTL_S.
 * Tracks nonce counter and triggers exhaustion flag when limit is reached.
 */

#include "session_msgstore.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h" /* IWYU pragma: keep */
#include <string.h>

/* Circular buffer with opportunistic expiration — no background maintenance task. */
MsgStore_t g_msg_store = {0};
static portMUX_TYPE s_msg_store_mux = portMUX_INITIALIZER_UNLOCKED;

CeePewErr_t msg_store_init(void){
    /* Ensure store starts empty and configured limits are sane */
    CEEPEW_ASSERT(CEEPEW_MAX_MESSAGES > 0U, CEEPEW_ERR_PARAM);

    portENTER_CRITICAL(&s_msg_store_mux);
    memset(&g_msg_store, 0U, sizeof(MsgStore_t));
    g_msg_store.head = 0U;
    g_msg_store.tail = 0U;
    g_msg_store.count = 0U;
    g_msg_store.last_wipe_ts = (uint32_t)(esp_timer_get_time() / 1000000LL);
    g_msg_store.nonce_exhausted = false;
    portEXIT_CRITICAL(&s_msg_store_mux);

    /* Sanity assert after init */
    CEEPEW_ASSERT(g_msg_store.count == 0U, CEEPEW_ERR_INTERNAL);
    return CEEPEW_OK;
}

static CeePewErr_t msg_store_expire_old_unlocked(void)
{
    uint64_t now_s = (uint64_t)(esp_timer_get_time() / 1000000LL);

    for (uint8_t i = 0U; i < CEEPEW_MAX_MESSAGES && g_msg_store.count > 0U; i++) {
        uint8_t msg_idx = (g_msg_store.head) % CEEPEW_MAX_MESSAGES;
        const StoredMsg_t *msg = &g_msg_store.messages[msg_idx];

        uint64_t age_s = (now_s > msg->meta.created_at)
                       ? (now_s - msg->meta.created_at)
                       : 0U;

        if (age_s > CEEPEW_MSG_TTL_S) {
            volatile uint8_t *p = (volatile uint8_t *)&g_msg_store.messages[g_msg_store.head];
            for (uint32_t j = 0U; j < sizeof(StoredMsg_t); j++) {
                p[j] = 0U;
            }
            __asm__ __volatile__("" ::: "memory");

            g_msg_store.head = (g_msg_store.head + 1U) % CEEPEW_MAX_MESSAGES;
            g_msg_store.count--;
        } else {
            break;
        }
    }

    g_msg_store.last_wipe_ts = (uint32_t)now_s;
    return CEEPEW_OK;
}

CeePewErr_t msg_store_add(const uint8_t *plaintext, uint16_t plaintext_len, uint8_t direction)
{
    CEEPEW_ASSERT(plaintext != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(plaintext_len > 0U && plaintext_len <= CEEPEW_MAX_MSG_BYTES,
                  CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(direction <= 1U, CEEPEW_ERR_PARAM);

    portENTER_CRITICAL(&s_msg_store_mux);

    /* Expire old messages to make room if needed */
    if (g_msg_store.count >= CEEPEW_MAX_MESSAGES) {
        (void)msg_store_expire_old_unlocked();

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
    uint64_t now_s = (uint64_t)(esp_timer_get_time() / 1000000LL);
    g_msg_store.messages[g_msg_store.tail].meta.created_at = now_s;
    g_msg_store.messages[g_msg_store.tail].meta.payload_len = plaintext_len;
    g_msg_store.messages[g_msg_store.tail].meta.dir = direction;
    g_msg_store.messages[g_msg_store.tail].meta.delivery_status = (direction == 1U) ? (uint8_t)MSG_STATUS_SENT : (uint8_t)MSG_STATUS_DELIVERED;
    g_msg_store.messages[g_msg_store.tail].meta.ttl_seconds = 0U;

    memcpy(g_msg_store.messages[g_msg_store.tail].plaintext, plaintext, plaintext_len);
    g_msg_store.messages[g_msg_store.tail].plaintext[plaintext_len] = '\0';
    /* Zero-pad the rest of the plaintext buffer */
    if (plaintext_len + 1U < sizeof(g_msg_store.messages[g_msg_store.tail].plaintext)) {
        memset(g_msg_store.messages[g_msg_store.tail].plaintext + plaintext_len + 1U,
               0U,
               sizeof(g_msg_store.messages[g_msg_store.tail].plaintext) - (plaintext_len + 1U));
    }

    /* New messages start as unread (false = unread). Only RX messages (dir==0) are considered for unread count. */
    g_msg_store.messages[g_msg_store.tail].read = false;

    g_msg_store.tail = (g_msg_store.tail + 1U) % CEEPEW_MAX_MESSAGES;
    g_msg_store.count++;

    portEXIT_CRITICAL(&s_msg_store_mux);

    return CEEPEW_OK;
}

const StoredMsg_t *msg_store_get(uint8_t index)
{
    portENTER_CRITICAL(&s_msg_store_mux);

    if (index >= g_msg_store.count) {
        portEXIT_CRITICAL(&s_msg_store_mux);
        return NULL;
    }

    uint8_t msg_idx = (g_msg_store.head + index) % CEEPEW_MAX_MESSAGES;
    const StoredMsg_t *msg_ptr = &g_msg_store.messages[msg_idx];

    portEXIT_CRITICAL(&s_msg_store_mux);
    return msg_ptr;
}

CeePewErr_t msg_store_expire_old(void)
{
    portENTER_CRITICAL(&s_msg_store_mux);
    CeePewErr_t err = msg_store_expire_old_unlocked();
    portEXIT_CRITICAL(&s_msg_store_mux);
    return err;
}

CeePewErr_t msg_store_wipe_all(void)
{
    portENTER_CRITICAL(&s_msg_store_mux);

    volatile uint8_t *p = (volatile uint8_t *)g_msg_store.messages;
    for (uint32_t i = 0U; i < sizeof(g_msg_store.messages); i++) {
        p[i] = 0U;
    }
    __asm__ __volatile__("" ::: "memory");

    g_msg_store.head = 0U;
    g_msg_store.tail = 0U;
    g_msg_store.count = 0U;
    g_msg_store.nonce_exhausted = false;

    portEXIT_CRITICAL(&s_msg_store_mux);
    return CEEPEW_OK;
}

CeePewErr_t msg_store_check_nonce_exhaustion(uint64_t nonce_counter)
{
    CEEPEW_ASSERT(nonce_counter <= CEEPEW_NONCE_HARD_LIMIT, CEEPEW_ERR_NONCE_EXHAUSTED);

    portENTER_CRITICAL(&s_msg_store_mux);
    if (nonce_counter >= CEEPEW_NONCE_HARD_LIMIT) {
        g_msg_store.nonce_exhausted = true;
        portEXIT_CRITICAL(&s_msg_store_mux);
        return CEEPEW_ERR_NONCE_EXHAUSTED;
    }
    portEXIT_CRITICAL(&s_msg_store_mux);
    return CEEPEW_OK;
}

uint8_t msg_store_count(void)
{
    portENTER_CRITICAL(&s_msg_store_mux);
    uint8_t count = g_msg_store.count;
    portEXIT_CRITICAL(&s_msg_store_mux);
    return count;
}

uint32_t session_get_last_wipe_ms(void)
{
    portENTER_CRITICAL(&s_msg_store_mux);
    uint32_t wipe_s = g_msg_store.last_wipe_ts;
    portEXIT_CRITICAL(&s_msg_store_mux);
    return wipe_s * 1000U;
}

uint8_t msg_store_unread_count(void)
{
    portENTER_CRITICAL(&s_msg_store_mux);
    uint8_t count = 0U;
    for (uint8_t i = 0U; i < g_msg_store.count; i++) {
        uint8_t msg_idx = (g_msg_store.head + i) % CEEPEW_MAX_MESSAGES;
        const StoredMsg_t *msg = &g_msg_store.messages[msg_idx];
        if (msg->meta.dir == 0U && !msg->read) {
            count++;
        }
    }
    portEXIT_CRITICAL(&s_msg_store_mux);
    return count;
}

CeePewErr_t msg_store_mark_read(uint8_t index)
{
    portENTER_CRITICAL(&s_msg_store_mux);
    CeePewErr_t err = CEEPEW_OK;
    if (index >= g_msg_store.count) {
        err = CEEPEW_ERR_BOUNDS;
    } else {
        uint8_t msg_idx = (g_msg_store.head + index) % CEEPEW_MAX_MESSAGES;
        g_msg_store.messages[msg_idx].read = true;
    }
    portEXIT_CRITICAL(&s_msg_store_mux);
    return err;
}

CeePewErr_t msg_store_update_delivery_status(uint8_t index, MsgDeliveryStatus_t status)
{
    portENTER_CRITICAL(&s_msg_store_mux);
    CeePewErr_t err = CEEPEW_OK;
    if (index >= g_msg_store.count) {
        err = CEEPEW_ERR_BOUNDS;
    } else {
        uint8_t msg_idx = (g_msg_store.head + index) % CEEPEW_MAX_MESSAGES;
        g_msg_store.messages[msg_idx].meta.delivery_status = (uint8_t)status;
    }
    portEXIT_CRITICAL(&s_msg_store_mux);
    return err;
}

CeePewErr_t msg_store_burn_all(void)
{
    return msg_store_wipe_all();
}
