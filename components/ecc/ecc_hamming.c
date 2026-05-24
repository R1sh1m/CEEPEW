/* components/ecc/ecc_hamming.c */

#include "ecc_hamming.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define CEEPEW_HAMMING_CODE_BITS 15U
#define CEEPEW_HAMMING_DATA_BITS 11U
#define CEEPEW_HAMMING_PARITY_BITS 4U

typedef struct {
    uint8_t perm[CEEPEW_HAMMING_CODE_BITS];
    uint8_t inv_perm[CEEPEW_HAMMING_CODE_BITS];
    bool initialised;
} EccHammingCtx_t;

static EccHammingCtx_t s_ctx = {.initialised = false};

/* Maps 4-bit syndrome -> bit position to flip (0-14), 15 = uncorrectable */
static const uint8_t SYNDROME_TABLE[16] = {0U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 15U};

static inline uint8_t bit_get_msb(const uint8_t *buf, uint32_t bit_pos, uint32_t total_bits) {
    if (bit_pos >= total_bits) { return 0U;}
    uint8_t byte_val = buf[bit_pos / 8U];
    uint8_t bit_idx = (uint8_t)(7U - (bit_pos % 8U));
    return (uint8_t)((byte_val >> bit_idx) & 1U);
}

static inline void bit_set_msb(uint8_t *buf, uint32_t bit_pos, uint8_t bit) {
    uint32_t byte_idx = bit_pos / 8U;
    uint8_t bit_idx = (uint8_t)(7U - (bit_pos % 8U));
    uint8_t mask = (uint8_t)(1U << bit_idx);
    if (bit != 0U) { buf[byte_idx] |= mask; }
    else { buf[byte_idx] &= (uint8_t)(~mask); }
}

static inline uint16_t extract_11_bits(const uint8_t *buf, uint32_t bit_offset, uint32_t total_bits) {
    uint16_t result = 0U;
    /* loop bound: 11 */
    for (uint8_t b = 0U; b < CEEPEW_HAMMING_DATA_BITS; b++) {
        uint32_t pos = bit_offset + b;
        uint8_t bit = bit_get_msb(buf, pos, total_bits);
        result = (uint16_t)((result << 1U) | bit);
    }
    return result;
}

static inline uint16_t extract_15_bits(const uint8_t *buf, uint32_t bit_offset, uint32_t total_bits) {
    uint16_t result = 0U;
    /* loop bound: 15 */
    for (uint8_t b = 0U; b < CEEPEW_HAMMING_CODE_BITS; b++) {
        uint8_t bit = bit_get_msb(buf, bit_offset + b, total_bits);
        result = (uint16_t)((result << 1U) | bit);
    }
    return result;
}

static inline void insert_15_bits(uint8_t *buf, uint32_t bit_offset, uint16_t value) {
    /* loop bound: 15 */
    for (uint8_t b = 0U; b < CEEPEW_HAMMING_CODE_BITS; b++) {
        uint8_t bit = (uint8_t)((value >> (CEEPEW_HAMMING_CODE_BITS - 1U - b)) & 1U);
        bit_set_msb(buf, bit_offset + b, bit);
    }
}

static inline void insert_11_bits(uint8_t *buf, uint32_t bit_offset, uint16_t value) {
    /* loop bound: 11 */
    for (uint8_t b = 0U; b < CEEPEW_HAMMING_DATA_BITS; b++) {
        uint8_t bit = (uint8_t)((value >> (CEEPEW_HAMMING_DATA_BITS - 1U - b)) & 1U);
        bit_set_msb(buf, bit_offset + b, bit);
    }
}

static inline uint8_t code_get_bit_pos1(uint16_t code, uint8_t pos1) {
    return (uint8_t)((code >> (CEEPEW_HAMMING_CODE_BITS - pos1)) & 1U);
}

static inline uint16_t code_set_bit_pos1(uint16_t code, uint8_t pos1, uint8_t bit) {
    uint16_t mask = (uint16_t)(1U << (CEEPEW_HAMMING_CODE_BITS - pos1));
    if (bit != 0U) { code |= mask; }
    else { code &= (uint16_t)(~mask); }
    return code;
}

static uint16_t hamming_encode_word(uint16_t data11) {
    uint16_t code = 0U;
    uint8_t data_idx = 0U;

    /* place data in non-parity positions (1,2,4,8 are parity) */
    /* loop bound: 15 */
    for (uint8_t pos1 = 1U; pos1 <= CEEPEW_HAMMING_CODE_BITS; pos1++) {
        bool is_parity = ((pos1 & (pos1 - 1U)) == 0U);
        if (!is_parity) {
            uint8_t bit = (uint8_t)((data11 >> (CEEPEW_HAMMING_DATA_BITS - 1U - data_idx)) & 1U);
            code = code_set_bit_pos1(code, pos1, bit);
            data_idx++;
        }
    }

    const uint8_t parity_positions[CEEPEW_HAMMING_PARITY_BITS] = {1U, 2U, 4U, 8U};
    /* loop bound: 4 */
    for (uint8_t p = 0U; p < CEEPEW_HAMMING_PARITY_BITS; p++) {
        uint8_t parity_pos = parity_positions[p];
        uint8_t parity = 0U;
        /* loop bound: 15 */
        for (uint8_t pos1 = 1U; pos1 <= CEEPEW_HAMMING_CODE_BITS; pos1++) {
            if ((pos1 & parity_pos) != 0U) {
                parity ^= code_get_bit_pos1(code, pos1);
            }
        }
        code = code_set_bit_pos1(code, parity_pos, parity);
    }
    return (uint16_t)(code & 0x7FFFU);
}

static uint8_t hamming_compute_syndrome(uint16_t code) {
    const uint8_t parity_positions[CEEPEW_HAMMING_PARITY_BITS] = {1U, 2U, 4U, 8U};
    uint8_t syndrome = 0U;

    /* loop bound: 4 */
    for (uint8_t p = 0U; p < CEEPEW_HAMMING_PARITY_BITS; p++) {
        uint8_t parity_pos = parity_positions[p];
        uint8_t parity = 0U;
        /* loop bound: 15 */
        for (uint8_t pos1 = 1U; pos1 <= CEEPEW_HAMMING_CODE_BITS; pos1++) {
            if ((pos1 & parity_pos) != 0U) {
                parity ^= code_get_bit_pos1(code, pos1);
            }
        }
        syndrome |= (uint8_t)(parity << p);
    }
    return syndrome;
}

static uint16_t hamming_extract_data11(uint16_t code) {
    uint16_t data = 0U;
    uint8_t data_idx = 0U;

    /* loop bound: 15 */
    for (uint8_t pos1 = 1U; pos1 <= CEEPEW_HAMMING_CODE_BITS; pos1++) {
        bool is_parity = ((pos1 & (pos1 - 1U)) == 0U);
        if (!is_parity) {
            uint8_t bit = code_get_bit_pos1(code, pos1);
            data |= (uint16_t)(bit << (CEEPEW_HAMMING_DATA_BITS - 1U - data_idx));
            data_idx++;
        }
    }
    return data;
}

static uint16_t apply_permutation(uint16_t code, const uint8_t perm[CEEPEW_HAMMING_CODE_BITS]) {
    uint16_t out = 0U;
    /* loop bound: 15 */
    for (uint8_t dst = 0U; dst < CEEPEW_HAMMING_CODE_BITS; dst++) {
        uint8_t src = perm[dst];
        uint8_t src_pos1 = (uint8_t)(src + 1U);
        uint8_t dst_pos1 = (uint8_t)(dst + 1U);
        uint8_t bit = code_get_bit_pos1(code, src_pos1);
        out = code_set_bit_pos1(out, dst_pos1, bit);
    }
    return out;
}

CeePewErr_t ecc_hamming_init_session(const uint8_t seed[CEEPEW_SESSION_KEY_BYTES]) {
    CEEPEW_ASSERT(seed != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(CEEPEW_SESSION_KEY_BYTES == 16U, CEEPEW_ERR_PARAM);
    memset(&s_ctx, 0, sizeof(s_ctx));
    /* identity */
    /* loop bound: 15 */
    for (uint8_t i = 0U; i < CEEPEW_HAMMING_CODE_BITS; i++) { s_ctx.perm[i] = i; }

    uint32_t prg = ((uint32_t)seed[0] << 24U) | ((uint32_t)seed[1] << 16U) | ((uint32_t)seed[2] << 8U) | ((uint32_t)seed[3]);
    /* loop bound: 12 */
    for (uint8_t i = 4U; i < CEEPEW_SESSION_KEY_BYTES; i++) {
        prg ^= ((uint32_t)seed[i] << ((i % 4U) * 8U));
        prg = prg * 1664525UL + 1013904223UL;
    }

    /* Fisher-Yates; loop bound: 14 */
    for (uint8_t i = CEEPEW_HAMMING_CODE_BITS - 1U; i > 0U; i--) {
        prg = prg * 1664525UL + 1013904223UL;
        uint8_t j = (uint8_t)((prg >> 16U) % ((uint32_t)i + 1U));
        uint8_t tmp = s_ctx.perm[i];
        s_ctx.perm[i] = s_ctx.perm[j];
        s_ctx.perm[j] = tmp;
    }

    /* build inverse permutation */
    /* loop bound: 15 */
    for (uint8_t i = 0U; i < CEEPEW_HAMMING_CODE_BITS; i++) {
        s_ctx.inv_perm[s_ctx.perm[i]] = i;
    }

    s_ctx.initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t ecc_hamming_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len) {
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_ctx.initialised, CEEPEW_ERR_FEC);
    CEEPEW_ASSERT(in_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);

    uint16_t out_capacity = *out_len;
    CEEPEW_ASSERT(out_capacity > 0U, CEEPEW_ERR_BOUNDS);

    uint32_t in_bits = (uint32_t)in_len * 8U;
    uint32_t codewords = (in_bits + (CEEPEW_HAMMING_DATA_BITS - 1U)) / CEEPEW_HAMMING_DATA_BITS;
    uint32_t out_bits = codewords * CEEPEW_HAMMING_CODE_BITS;
    uint16_t required_bytes = (uint16_t)((out_bits + 7U) / 8U);
    CEEPEW_ASSERT(required_bytes <= out_capacity, CEEPEW_ERR_BOUNDS);

    memset(out, 0, (size_t)required_bytes);

    /* loop bound: ceil((CEEPEW_MAX_MSG_BYTES*8)/11) */
    for (uint32_t cw = 0U; cw < codewords; cw++) {
        uint16_t data11 = extract_11_bits(in, cw * CEEPEW_HAMMING_DATA_BITS, in_bits);
        uint16_t code = hamming_encode_word(data11);
        uint16_t permuted = apply_permutation(code, s_ctx.perm);
        insert_15_bits(out, cw * CEEPEW_HAMMING_CODE_BITS, permuted);
    }

    *out_len = required_bytes;
    return CEEPEW_OK;
}

CeePewErr_t ecc_hamming_decode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, bool *corrected) {
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL && corrected != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_ctx.initialised, CEEPEW_ERR_FEC);

    uint16_t out_capacity = *out_len;
    CEEPEW_ASSERT(out_capacity > 0U, CEEPEW_ERR_BOUNDS);

    uint32_t in_bits = (uint32_t)in_len * 8U;
    uint32_t codewords = in_bits / CEEPEW_HAMMING_CODE_BITS;
    uint32_t out_bits = codewords * CEEPEW_HAMMING_DATA_BITS;
    uint16_t required_bytes = (uint16_t)((out_bits + 7U) / 8U);
    CEEPEW_ASSERT(required_bytes <= out_capacity, CEEPEW_ERR_BOUNDS);

    memset(out, 0, (size_t)required_bytes);
    *corrected = false;

    /* loop bound: floor((in_len*8)/15) */
    for (uint32_t cw = 0U; cw < codewords; cw++) {
        uint16_t rx_permuted = extract_15_bits(in, cw * CEEPEW_HAMMING_CODE_BITS, in_bits);
        uint16_t code = apply_permutation(rx_permuted, s_ctx.inv_perm);

        uint8_t syndrome = hamming_compute_syndrome(code);
        if (syndrome != 0U){
            uint8_t flip_idx = SYNDROME_TABLE[syndrome];
            if (flip_idx == 15U) { return CEEPEW_ERR_FEC; }
            uint8_t pos1 = (uint8_t)(flip_idx + 1U);
            code ^= (uint16_t)(1U << (CEEPEW_HAMMING_CODE_BITS - pos1));
            *corrected = true;
        }

        uint16_t data11 = hamming_extract_data11(code);
        insert_11_bits(out, cw * CEEPEW_HAMMING_DATA_BITS, data11);
    }

    *out_len = required_bytes;
    return CEEPEW_OK;
}
