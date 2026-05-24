/* components/crypto/crypto_ascon.c */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"

#include <stdint.h>
#include <string.h>

#define ASCON128_IV 0x80400c0600000000ULL
#define ASCON_RATE_BYTES 8U

static const uint64_t ASCON_RC[12] = {
    0xf0ULL, 0xe1ULL, 0xd2ULL, 0xc3ULL,
    0xb4ULL, 0xa5ULL, 0x96ULL, 0x87ULL,
    0x78ULL, 0x69ULL, 0x5aULL, 0x4bULL
};

static uint64_t load64_le(const uint8_t *in){
    return ((uint64_t)in[0]) |
           ((uint64_t)in[1] << 8U) |
           ((uint64_t)in[2] << 16U) |
           ((uint64_t)in[3] << 24U) |
           ((uint64_t)in[4] << 32U) |
           ((uint64_t)in[5] << 40U) |
           ((uint64_t)in[6] << 48U) |
           ((uint64_t)in[7] << 56U);
}

static void store64_le(uint8_t *out, uint64_t v){
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8U);
    out[2] = (uint8_t)(v >> 16U);
    out[3] = (uint8_t)(v >> 24U);
    out[4] = (uint8_t)(v >> 32U);
    out[5] = (uint8_t)(v >> 40U);
    out[6] = (uint8_t)(v >> 48U);
    out[7] = (uint8_t)(v >> 56U);
}

static uint64_t rotr64(uint64_t x, uint8_t n){ return (x >> n) | (x << (64U - n)); }

static void ascon_p(uint64_t s[5], uint8_t rounds) {
    for (uint8_t r = (uint8_t)(12U - rounds); r < 12U; r++) {
        s[2] ^= ASCON_RC[r];
        s[0] ^= s[4];
        s[4] ^= s[3];
        s[2] ^= s[1];
        uint64_t t0 = ~s[0] & s[1];
        uint64_t t1 = ~s[1] & s[2];
        uint64_t t2 = ~s[2] & s[3];
        uint64_t t3 = ~s[3] & s[4];
        uint64_t t4 = ~s[4] & s[0];
        s[0] ^= t1;
        s[1] ^= t2;
        s[2] ^= t3;
        s[3] ^= t4;
        s[4] ^= t0;
        s[1] ^= s[0];
        s[0] ^= s[4];
        s[3] ^= s[2];
        s[2] = ~s[2];
        s[0] ^= rotr64(s[0], 19U) ^ rotr64(s[0], 28U);
        s[1] ^= rotr64(s[1], 61U) ^ rotr64(s[1], 39U);
        s[2] ^= rotr64(s[2], 1U) ^ rotr64(s[2], 6U);
        s[3] ^= rotr64(s[3], 10U) ^ rotr64(s[3], 17U);
        s[4] ^= rotr64(s[4], 7U) ^ rotr64(s[4], 41U);
    }
}

static void ascon_pad(uint8_t block[ASCON_RATE_BYTES], uint16_t used) {
    for (uint8_t i = 0U; i < ASCON_RATE_BYTES; i++) { block[i] = 0U;}
    if (used < ASCON_RATE_BYTES) { block[used] = 0x80U;}
}

CeePewErr_t crypto_ascon_aead_encrypt(const uint8_t key[16], const uint8_t nonce[16], const uint8_t *ad, uint16_t ad_len, const uint8_t *pt, uint16_t pt_len, uint8_t *ct, uint16_t *ct_len) {
    CEEPEW_ASSERT(key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ct != NULL && ct_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(*ct_len >= (uint16_t)(pt_len + CEEPEW_ASCON_TAG_BYTES), CEEPEW_ERR_BOUNDS);
    uint64_t k0 = load64_le(&key[0]);
    uint64_t k1 = load64_le(&key[8]);
    uint64_t n0 = load64_le(&nonce[0]);
    uint64_t n1 = load64_le(&nonce[8]);
    uint64_t s[5] = {ASCON128_IV, k0, k1, n0, n1};
    ascon_p(s, 12U);
    s[3] ^= k0;
    s[4] ^= k1;
    if (ad != NULL && ad_len > 0U) {
        uint16_t off = 0U;
        while ((uint32_t)off + ASCON_RATE_BYTES <= ad_len) {
            s[0] ^= load64_le(&ad[off]);
            ascon_p(s, 6U);
            off = (uint16_t)(off + ASCON_RATE_BYTES);
        }
        uint8_t block[ASCON_RATE_BYTES];
        ascon_pad(block, (uint16_t)(ad_len - off));
        for (uint16_t i = 0U; i < (uint16_t)(ad_len - off); i++) { block[i] = ad[off + i]; }
        s[0] ^= load64_le(block);
        s[4] ^= 1U;
    }
    else { s[4] ^= 1U; }

    uint16_t out_pos = 0U;
    uint16_t in_pos = 0U;
    while ((uint32_t)in_pos + ASCON_RATE_BYTES <= pt_len) {
        s[0] ^= load64_le(&pt[in_pos]);
        store64_le(&ct[out_pos], s[0]);
        ascon_p(s, 6U);
        in_pos = (uint16_t)(in_pos + ASCON_RATE_BYTES);
        out_pos = (uint16_t)(out_pos + ASCON_RATE_BYTES);
    }

    uint16_t rem = (uint16_t)(pt_len - in_pos);
    uint8_t block[ASCON_RATE_BYTES];
    ascon_pad(block, rem);
    for (uint16_t i = 0U; i < rem; i++) { block[i] = pt[in_pos + i]; }
    s[0] ^= load64_le(block);
    store64_le(block, s[0]);
    for (uint16_t i = 0U; i < rem; i++) { ct[out_pos + i] = block[i];}
    s[1] ^= k0;
    s[2] ^= k1;
    ascon_p(s, 12U);
    s[3] ^= k0;
    s[4] ^= k1;
    store64_le(&ct[out_pos + rem], s[3]);
    store64_le(&ct[out_pos + rem + 8U], s[4]);
    *ct_len = (uint16_t)(pt_len + CEEPEW_ASCON_TAG_BYTES);
    return CEEPEW_OK;
}

CeePewErr_t crypto_ascon_aead_decrypt(const uint8_t key[16], const uint8_t nonce[16], const uint8_t *ad, uint16_t ad_len, const uint8_t *ct, uint16_t ct_len, uint8_t *pt, uint16_t *pt_len){
    CEEPEW_ASSERT(key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pt != NULL && pt_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ct != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(ct_len >= CEEPEW_ASCON_TAG_BYTES, CEEPEW_ERR_PARAM);
    uint16_t msg_len = (uint16_t)(ct_len - CEEPEW_ASCON_TAG_BYTES);
    CEEPEW_ASSERT(*pt_len >= msg_len, CEEPEW_ERR_BOUNDS);
    uint64_t k0 = load64_le(&key[0]);
    uint64_t k1 = load64_le(&key[8]);
    uint64_t n0 = load64_le(&nonce[0]);
    uint64_t n1 = load64_le(&nonce[8]);
    uint64_t s[5] = {ASCON128_IV, k0, k1, n0, n1};
    ascon_p(s, 12U);
    s[3] ^= k0;
    s[4] ^= k1;
    if (ad != NULL && ad_len > 0U) {
        uint16_t off = 0U;
        while ((uint32_t)off + ASCON_RATE_BYTES <= ad_len) {
            s[0] ^= load64_le(&ad[off]);
            ascon_p(s, 6U);
            off = (uint16_t)(off + ASCON_RATE_BYTES);
        }
        uint8_t block[ASCON_RATE_BYTES];
        ascon_pad(block, (uint16_t)(ad_len - off));
        for (uint16_t i = 0U; i < (uint16_t)(ad_len - off); i++) { block[i] = ad[off + i]; }
        s[0] ^= load64_le(block);
        s[4] ^= 1U;
    }
    else { s[4] ^= 1U; }

    uint16_t in_pos = 0U;
    uint16_t out_pos = 0U;
    while ((uint32_t)in_pos + ASCON_RATE_BYTES <= msg_len) {
        uint64_t cblk = load64_le(&ct[in_pos]);
        uint64_t pblk = s[0] ^ cblk;
        store64_le(&pt[out_pos], pblk);
        s[0] = cblk;
        ascon_p(s, 6U);
        in_pos = (uint16_t)(in_pos + ASCON_RATE_BYTES);
        out_pos = (uint16_t)(out_pos + ASCON_RATE_BYTES);
    }

    uint16_t rem = (uint16_t)(msg_len - in_pos);
    uint8_t block[ASCON_RATE_BYTES];
    for (uint8_t i = 0U; i < ASCON_RATE_BYTES; i++) { block[i] = 0U;}
    for (uint16_t i = 0U; i < rem; i++) { block[i] = ct[in_pos + i]; }
    uint64_t cblk = load64_le(block);
    uint64_t pblk = s[0] ^ cblk;
    store64_le(block, pblk);
    for (uint16_t i = 0U; i < rem; i++) { pt[out_pos + i] = block[i]; }
    s[0] = cblk;
    s[1] ^= k0;
    s[2] ^= k1;
    ascon_p(s, 12U);
    s[3] ^= k0;
    s[4] ^= k1;
    uint8_t tag[16];
    store64_le(&tag[0], s[3]);
    store64_le(&tag[8], s[4]);
    if (!ceepew_ct_equal(tag, &ct[msg_len], 16U)){ return CEEPEW_ERR_CRYPTO; }
    *pt_len = msg_len;
    return CEEPEW_OK;
}
