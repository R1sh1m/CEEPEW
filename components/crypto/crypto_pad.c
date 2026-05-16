/* components/crypto/crypto_pad.c */

#include "crypto_hkdf.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>
#include <string.h>

/* Padding helpers for transport frames. Use deterministic padding scheme so
 * that length-leakage is bounded. Implemented here as an API placeholder.
 */

CeePewErr_t crypto_pad_apply(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len){
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* Placeholder: copy input to output and set out_len. Replace with real
     * padding algorithm (e.g., PKCS7-like or custom fixed-block scheme).
     */
    if (*out_len < in_len){ return CEEPEW_ERR_BOUNDS;}

    memcpy(out, in, in_len);
    *out_len = in_len;
    return CEEPEW_ERR_UNSUPPORTED;
}

CeePewErr_t crypto_pad_remove(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len){
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    if (*out_len < in_len){  return CEEPEW_ERR_BOUNDS; }

    memcpy(out, in, in_len);
    *out_len = in_len;
    return CEEPEW_ERR_UNSUPPORTED;
}
