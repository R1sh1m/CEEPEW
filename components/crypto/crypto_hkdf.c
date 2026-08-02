/* components/crypto/crypto_hkdf.c */

#include "crypto_hkdf.h"
#include "crypto_hmac.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include <stdint.h>
#include <string.h>

/* SECURITY: The HKDF salt must be SHA256(digital_sum_mix(code) || code) as
 * per Final Spec §3.3. The session code is the only secret the HKDF caller
 * holds — without it, ECDH shared secret alone is insufficient to derive the
 * session key.
 */
CeePewErr_t crypto_hkdf_build_info(const uint8_t *label, uint8_t label_len,
                                   const uint8_t id_a[6], const uint8_t id_b[6],
                                   const uint8_t commitment[32], uint32_t t_round,
                                   uint8_t *out_info, uint8_t out_info_max_len,
                                   uint8_t *out_len)
{
    CEEPEW_ASSERT(label != NULL && label_len > 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(id_a != NULL && id_b != NULL && commitment != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out_info != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* Compute required length and validate against buffer capacity.
     * Required: label_len + 6 (id_a) + 6 (id_b) + 32 (commitment) + 4 (t_round) */
    uint32_t required = (uint32_t)label_len + 6U + 6U + 32U + 4U;
    CEEPEW_ASSERT(required <= (uint32_t)out_info_max_len, CEEPEW_ERR_BOUNDS);

    uint8_t off = 0U;
    memcpy(out_info + off, label, label_len); off += label_len;
    memcpy(out_info + off, id_a, 6U); off += 6U;
    memcpy(out_info + off, id_b, 6U); off += 6U;
    memcpy(out_info + off, commitment, 32U); off += 32U;

    /* Canonical t_round encoding: big-endian u32 */
    out_info[off++] = (uint8_t)((t_round >> 24) & 0xFFU);
    out_info[off++] = (uint8_t)((t_round >> 16) & 0xFFU);
    out_info[off++] = (uint8_t)((t_round >> 8) & 0xFFU);
    out_info[off++] = (uint8_t)(t_round & 0xFFU);

    *out_len = off;
    return CEEPEW_OK;
}

/* SECURITY: The HKDF salt must be SHA256(digital_sum_mix(code) || code) as
 * per Final Spec §3.3. The session code is the only secret the HKDF caller
 * holds — without it, ECDH shared secret alone is insufficient to derive the
 * session key.
 */
CeePewErr_t crypto_hkdf_derive(const uint8_t *ikm, uint8_t ikm_len, const uint8_t *salt, uint8_t salt_len, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len) {
    CEEPEW_ASSERT(ikm != NULL && ikm_len > 0U && ikm_len <= 32U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(out != NULL && out_len > 0U && out_len <= 64U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(salt != NULL || salt_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(info != NULL || info_len == 0U, CEEPEW_ERR_NULL_PTR);

    uint8_t prk[32U];
    CeePewErr_t err = crypto_hmac_sha256(salt, (uint16_t)salt_len, ikm, (uint32_t)ikm_len, prk);
    if (err != CEEPEW_OK) {
        return err;
    }

    uint8_t t[32U];
    uint8_t generated = 0U;
    uint8_t previous_len = 0U;
    uint8_t nblocks = (uint8_t)((out_len + 31U) / 32U);
    uint8_t input_buf[320U];

    for (uint8_t block = 0U; block < nblocks; block++) {
        size_t pos = 0U;
        if (previous_len > 0U) {
            memcpy(input_buf + pos, t, previous_len);
            pos += previous_len;
        }
        if (info_len > 0U) {
            memcpy(input_buf + pos, info, info_len);
            pos += info_len;
        }
        input_buf[pos++] = (uint8_t)(block + 1U);

        err = crypto_hmac_sha256(prk, (uint16_t)sizeof(prk), input_buf, (uint32_t)pos, t);
        if (err != CEEPEW_OK) {
            volatile uint8_t *vp = (volatile uint8_t *)prk;
            for (uint32_t i = 0U; i < sizeof(prk); i++) { vp[i] = 0U; }
            __asm__ __volatile__("" ::: "memory");
            ceepew_secure_zero(t, sizeof(t));
            return err;
        }

        uint8_t to_copy = (uint8_t)((out_len - generated) < 32U ? (out_len - generated) : 32U);
        memcpy(out + generated, t, to_copy);
        generated = (uint8_t)(generated + to_copy);
        previous_len = 32U;
    }

    ceepew_secure_zero(prk, sizeof(prk));
    ceepew_secure_zero(t, sizeof(t));

    return CEEPEW_OK;
}

/* HKDF-Expand only (using a pre-computed PRK).
 * prk: 32-byte pseudorandom key (output of HKDF-Extract)
 * info: context info string
 * info_len: length of info
 * out: output buffer
 * out_len: desired output length (<= 64) */
CeePewErr_t crypto_hkdf_expand(const uint8_t *prk, const uint8_t *info, uint8_t info_len, uint8_t *out, uint8_t out_len)
{
    CEEPEW_ASSERT(prk != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len > 0U && out_len <= 64U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(info != NULL || info_len == 0U, CEEPEW_ERR_NULL_PTR);

    uint8_t t[32U];
    uint8_t generated = 0U;
    uint8_t previous_len = 0U;
    uint8_t nblocks = (uint8_t)((out_len + 31U) / 32U);
    uint8_t input_buf[320U];

    for (uint8_t block = 0U; block < nblocks; block++) {
        size_t pos = 0U;
        if (previous_len > 0U) {
            memcpy(input_buf + pos, t, previous_len);
            pos += previous_len;
        }
        if (info_len > 0U) {
            memcpy(input_buf + pos, info, info_len);
            pos += info_len;
        }
        input_buf[pos++] = (uint8_t)(block + 1U);

        CeePewErr_t err = crypto_hmac_sha256(prk, 32U, input_buf, (uint32_t)pos, t);
        if (err != CEEPEW_OK) {
            ceepew_secure_zero(t, sizeof(t));
            return err;
        }

        uint8_t to_copy = (uint8_t)((out_len - generated) < 32U ? (out_len - generated) : 32U);
        memcpy(out + generated, t, to_copy);
        generated = (uint8_t)(generated + to_copy);
        previous_len = 32U;
    }

    volatile uint8_t *vt = (volatile uint8_t *)t;
    for (uint32_t i = 0U; i < sizeof(t); i++) { vt[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");

    return CEEPEW_OK;
}
