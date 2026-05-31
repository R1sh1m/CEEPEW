/* components/crypto/crypto_box_wrap.c */

#include "crypto_box_wrap.h"
#include "crypto_stream.h"
#include "crypto_sha256.h"
#include "ceepew_security_utils.h"
#include "session_fsm.h"

#include "curve25519.h"

#include <string.h>

static uint8_t s_nacl_pt_buf[CRYPTO_BOX_ZEROBYTES + CEEPEW_MAX_MSG_BYTES];
static uint8_t s_nacl_ct_buf[CRYPTO_BOX_ZEROBYTES + CEEPEW_MAX_MSG_BYTES];

static CeePewErr_t hmac_sha256(const uint8_t *key,
                               uint16_t key_len,
                               const uint8_t *msg,
                               uint16_t msg_len,
                               uint8_t out[32U])
{
    CEEPEW_ASSERT(key != NULL || key_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t key_block[64U];
    uint8_t inner_pad[64U];
    uint8_t outer_pad[64U];
    uint8_t inner_hash[32U];

    memset(key_block, 0, sizeof(key_block));
    if (key_len > sizeof(key_block)) {
        CeePewErr_t err = crypto_sha256_compute(key, key_len, key_block);
        if (err != CEEPEW_OK) {
            ceepew_secure_zero(key_block, (uint32_t)sizeof(key_block));
            return err;
        }
    } else if (key_len > 0U) {
        memcpy(key_block, key, key_len);
    }

    for (uint8_t i = 0U; i < 64U; i++) {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36U);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5CU);
    }

    uint8_t inner_msg[64U + CEEPEW_MAX_MSG_BYTES];
    memcpy(inner_msg, inner_pad, 64U);
    if (msg_len > 0U) {
        memcpy(inner_msg + 64U, msg, msg_len);
    }
    CeePewErr_t err = crypto_sha256_compute(inner_msg, (uint32_t)(64U + msg_len), inner_hash);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(key_block, (uint32_t)sizeof(key_block));
        ceepew_secure_zero(inner_pad, (uint32_t)sizeof(inner_pad));
        ceepew_secure_zero(outer_pad, (uint32_t)sizeof(outer_pad));
        ceepew_secure_zero(inner_hash, (uint32_t)sizeof(inner_hash));
        ceepew_secure_zero(inner_msg, (uint32_t)sizeof(inner_msg));
        return err;
    }

    uint8_t outer_msg[64U + 32U];
    memcpy(outer_msg, outer_pad, 64U);
    memcpy(outer_msg + 64U, inner_hash, 32U);
    err = crypto_sha256_compute(outer_msg, 96U, out);

    ceepew_secure_zero(key_block, (uint32_t)sizeof(key_block));
    ceepew_secure_zero(inner_pad, (uint32_t)sizeof(inner_pad));
    ceepew_secure_zero(outer_pad, (uint32_t)sizeof(outer_pad));
    ceepew_secure_zero(inner_hash, (uint32_t)sizeof(inner_hash));
    ceepew_secure_zero(inner_msg, (uint32_t)sizeof(inner_msg));
    ceepew_secure_zero(outer_msg, (uint32_t)sizeof(outer_msg));
    return err;
}

static void box_make_nonce_from_counter(const CryptoCtx_t *ctx, uint64_t nonce_counter, uint8_t nonce[CRYPTO_BOX_NONCEBYTES])
{
    memcpy(nonce, ctx->session_id, 8U);
    for (uint8_t i = 0U; i < 8U; i++) {
        nonce[8U + i] = (uint8_t)((nonce_counter >> (8U * i)) & 0xFFU);
    }
    memset(nonce + 16U, 0, 8U);
}

static CeePewErr_t box_derive_shared_secret(const CryptoCtx_t *ctx,
                                            const uint8_t peer_public_key[32U],
                                            uint8_t shared_secret[32U])
{
    uint8_t scalar[32U];
    memcpy(scalar, ctx->box_seed, sizeof(scalar));
    curve25519_clamp(scalar);
    int rc = curve25519_scalarmult(shared_secret, scalar, peer_public_key);
    ceepew_secure_zero(scalar, (uint32_t)sizeof(scalar));
    return (rc == 0) ? CEEPEW_OK : CEEPEW_ERR_CRYPTO;
}

static CeePewErr_t box_derive_stream_key(const uint8_t shared_secret[32U],
                                         uint8_t stream_key[32U])
{
    return crypto_sha256_compute(shared_secret, 32U, stream_key);
}

CeePewErr_t crypto_box_encrypt(CryptoCtx_t *ctx,
                               const uint8_t peer_public_key[32U],
                               const uint8_t *msg,
                               uint16_t msg_len,
                               uint8_t *out,
                               uint16_t *out_len)
{
    CEEPEW_ASSERT(ctx != NULL && ctx->session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(peer_public_key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg != NULL || msg_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(msg_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(*out_len >= (uint16_t)(msg_len + CRYPTO_BOX_BOXZEROBYTES),
                  CEEPEW_ERR_BOUNDS);

    uint64_t nonce_counter = session_get_nonce_counter();

    uint8_t nonce[CRYPTO_BOX_NONCEBYTES];
    box_make_nonce_from_counter(ctx, nonce_counter, nonce);

    uint8_t shared_secret[32U];
    uint8_t stream_key[32U];
    CeePewErr_t err = box_derive_shared_secret(ctx, peer_public_key, shared_secret);
    if (err != CEEPEW_OK) {
        return err;
    }
    err = box_derive_stream_key(shared_secret, stream_key);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        return err;
    }

    StreamCipher_t stream;
    err = crypto_stream_init(&stream, stream_key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        return err;
    }

    memset(s_nacl_pt_buf, 0, CRYPTO_BOX_ZEROBYTES);
    if (msg_len > 0U) {
        memcpy(s_nacl_pt_buf + CRYPTO_BOX_ZEROBYTES, msg, msg_len);
    }
    uint16_t work_len = (uint16_t)(CRYPTO_BOX_ZEROBYTES + msg_len);
    uint16_t out_work_len = work_len;
    err = crypto_stream_process(&stream, s_nacl_pt_buf, work_len, s_nacl_ct_buf, &out_work_len);
    if (err != CEEPEW_OK) {
        crypto_stream_finalise(&stream);
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        return err;
    }

    uint8_t mac_key[32U];
    memcpy(mac_key, s_nacl_ct_buf, 32U);
    uint8_t mac[32U];
    err = hmac_sha256(mac_key, 32U, s_nacl_ct_buf + CRYPTO_BOX_ZEROBYTES, msg_len, mac);
    if (err != CEEPEW_OK) {
        crypto_stream_finalise(&stream);
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
        ceepew_secure_zero(mac, (uint32_t)sizeof(mac));
        return err;
    }

    memcpy(s_nacl_ct_buf + CRYPTO_BOX_BOXZEROBYTES, mac, CRYPTO_BOX_BOXZEROBYTES);
    memset(s_nacl_ct_buf, 0, CRYPTO_BOX_BOXZEROBYTES);
    memcpy(out, s_nacl_ct_buf + CRYPTO_BOX_BOXZEROBYTES,
           (size_t)msg_len + CRYPTO_BOX_BOXZEROBYTES);
    *out_len = (uint16_t)(msg_len + CRYPTO_BOX_BOXZEROBYTES);

    crypto_stream_finalise(&stream);
    ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
    ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
    ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
    ceepew_secure_zero(mac, (uint32_t)sizeof(mac));
    ceepew_secure_zero(nonce, (uint32_t)sizeof(nonce));
    ceepew_secure_zero(s_nacl_pt_buf, (uint32_t)sizeof(s_nacl_pt_buf));
    ceepew_secure_zero(s_nacl_ct_buf, (uint32_t)sizeof(s_nacl_ct_buf));

    return CEEPEW_OK;
}

CeePewErr_t crypto_box_decrypt(const CryptoCtx_t *ctx,
                               const uint8_t nonce[CRYPTO_BOX_NONCEBYTES],
                               const uint8_t peer_public_key[32U],
                               const uint8_t *in,
                               uint16_t in_len,
                               uint8_t *out,
                               uint16_t *out_len)
{
    CEEPEW_ASSERT(ctx != NULL && ctx->session_active, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(nonce != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(peer_public_key != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len >= CRYPTO_BOX_BOXZEROBYTES, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT((uint16_t)(in_len - CRYPTO_BOX_BOXZEROBYTES) <= CEEPEW_MAX_MSG_BYTES,
                  CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(*out_len >= (uint16_t)(in_len - CRYPTO_BOX_BOXZEROBYTES),
                  CEEPEW_ERR_BOUNDS);

    uint16_t msg_len = (uint16_t)(in_len - CRYPTO_BOX_BOXZEROBYTES);

    uint8_t shared_secret[32U];
    uint8_t stream_key[32U];
    CeePewErr_t err = box_derive_shared_secret(ctx, peer_public_key, shared_secret);
    if (err != CEEPEW_OK) {
        return err;
    }
    err = box_derive_stream_key(shared_secret, stream_key);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        return err;
    }

    StreamCipher_t stream;
    err = crypto_stream_init(&stream, stream_key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        return err;
    }

    memset(s_nacl_pt_buf, 0, CRYPTO_BOX_ZEROBYTES);
    uint16_t work_len = CRYPTO_BOX_ZEROBYTES;
    uint16_t out_work_len = work_len;
    err = crypto_stream_process(&stream, s_nacl_pt_buf, work_len, s_nacl_ct_buf, &out_work_len);
    if (err != CEEPEW_OK) {
        crypto_stream_finalise(&stream);
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        return err;
    }

    uint8_t mac_key[32U];
    memcpy(mac_key, s_nacl_ct_buf, 32U);
    uint8_t calc_mac[32U];
    err = hmac_sha256(mac_key, 32U, in + CRYPTO_BOX_BOXZEROBYTES, msg_len, calc_mac);
    if (err != CEEPEW_OK) {
        crypto_stream_finalise(&stream);
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
        ceepew_secure_zero(calc_mac, (uint32_t)sizeof(calc_mac));
        return err;
    }

    if (!ceepew_ct_equal(in, calc_mac, CRYPTO_BOX_BOXZEROBYTES)) {
        crypto_stream_finalise(&stream);
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
        ceepew_secure_zero(calc_mac, (uint32_t)sizeof(calc_mac));
        return CEEPEW_ERR_CRYPTO;
    }

    crypto_stream_finalise(&stream);
    err = crypto_stream_init(&stream, stream_key, nonce);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
        ceepew_secure_zero(calc_mac, (uint32_t)sizeof(calc_mac));
        return err;
    }
    work_len = CRYPTO_BOX_ZEROBYTES;
    out_work_len = work_len;
    err = crypto_stream_process(&stream, s_nacl_pt_buf, work_len, s_nacl_ct_buf, &out_work_len);
    if (err != CEEPEW_OK) {
        crypto_stream_finalise(&stream);
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
        ceepew_secure_zero(calc_mac, (uint32_t)sizeof(calc_mac));
        return err;
    }

    err = crypto_stream_process(&stream, in + CRYPTO_BOX_BOXZEROBYTES, msg_len, out, out_len);
    crypto_stream_finalise(&stream);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
        ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
        ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
        ceepew_secure_zero(calc_mac, (uint32_t)sizeof(calc_mac));
        return err;
    }

    ceepew_secure_zero(shared_secret, (uint32_t)sizeof(shared_secret));
    ceepew_secure_zero(stream_key, (uint32_t)sizeof(stream_key));
    ceepew_secure_zero(mac_key, (uint32_t)sizeof(mac_key));
    ceepew_secure_zero(calc_mac, (uint32_t)sizeof(calc_mac));
    ceepew_secure_zero(s_nacl_pt_buf, (uint32_t)sizeof(s_nacl_pt_buf));
    ceepew_secure_zero(s_nacl_ct_buf, (uint32_t)sizeof(s_nacl_ct_buf));
    return CEEPEW_OK;
}
