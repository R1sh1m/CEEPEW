/* components/ecc/ecc_arq.c */

#include "ecc_hamming.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>
#include <string.h>

/* Simple ARQ wrapper using existing Hamming ECC encode/decode. This provides
 * a minimal interface that higher layers can call; more sophisticated ARQ
 * (retransmit/state machines) belong in transport layer.
 */

CeePewErr_t ecc_arq_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, uint16_t max_out_len){
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* For now, pass-through and let transport add redundancy later */
    if (in_len > max_out_len) {
        return CEEPEW_ERR_BOUNDS;
    }
    memcpy(out, in, in_len);
    *out_len = in_len;
    return CEEPEW_OK;
}

CeePewErr_t ecc_arq_decode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, uint16_t max_out_len, bool *corrected){
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL && corrected != NULL, CEEPEW_ERR_NULL_PTR);
    if (in_len > max_out_len){
        return CEEPEW_ERR_BOUNDS;
    }
    memcpy(out, in, in_len);
    *out_len = in_len;
    *corrected = false;
    return CEEPEW_OK;
}
