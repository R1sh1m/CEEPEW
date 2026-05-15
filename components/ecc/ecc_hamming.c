/* components/ecc/ecc_hamming.c */

#include "ecc_hamming.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Local module context type requested by spec; kept module-static. */
typedef struct {
    uint8_t perm[CEEPEW_FEC_BLOCK_SIZE]; /* column permutation */
    bool    initialised;
} EslCtx_t;

static EslCtx_t fec_perm_ctx = { .initialised = false };

/* Maximum number of Hamming codewords that can encode CEEPEW_MAX_MSG_BYTES */
#define CEEPEW_MAX_HAMMING_CODEWORDS ((CEEPEW_MAX_MSG_BYTES * 8U + CEEPEW_FEC_DATA_SIZE - 1U) / CEEPEW_FEC_DATA_SIZE)

/* Syndrome table (4-bit syndrome -> bit position to flip in 1..15).
 * Stored in flash as constant per requirements. For standard Hamming (15,11)
 * the syndrome value equals the error position (0 == no error).
 */
static const uint8_t SYNDROME_TABLE[16] = {
    0,  /* 0 -> no error */
    1, 2, 3, 4, 5, 6, 7,
    8, 9,10,11,12,13,14,15
};

/* SECURITY: Session-keyed FEC permutation — prevents standard Hamming analysis
 * without session key. See Final Spec §9.1 and Addendum C.
 */
CeePewErr_t ecc_hamming_init_session(const uint8_t seed[CEEPEW_SESSION_KEY_BYTES])
{
    CEEPEW_ASSERT(seed != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(CEEPEW_SESSION_KEY_BYTES == 16U, CEEPEW_ERR_PARAM);

    /* Initialize identity permutation */
    for (uint8_t i = 0U; i < CEEPEW_FEC_BLOCK_SIZE; i++) {
        fec_perm_ctx.perm[i] = i;
    }

    /* Simple 32-bit LCG seeded from first 4 bytes of seed (deterministic).
     * Fisher-Yates shuffle using LCG; bounded loops (compile-time constant).
     */
    uint32_t state = 0U;
    for (uint8_t i = 0U; i < 4U; i++) {
        state = (state << 8) | seed[i];
    }

    for (int i = (int)CEEPEW_FEC_BLOCK_SIZE - 1; i > 0; i--) {
        /* LCG constants (Numerical Recipes style) */
        state = state * 1664525U + 1013904223U;
        uint32_t rnd = state % (uint32_t)(i + 1);
        uint8_t tmp = fec_perm_ctx.perm[i];
        fec_perm_ctx.perm[i] = fec_perm_ctx.perm[rnd];
        fec_perm_ctx.perm[rnd] = tmp;
    }

    fec_perm_ctx.initialised = true;
    return CEEPEW_OK;
}

/* Helper: compute parity bit for a set of positions (1-based) within 15-bit word */
static uint8_t parity_of(uint16_t word, uint16_t mask)
{
    uint16_t v = word & mask;
    /* Popcount parity */
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (uint8_t)(v & 1U);
}

/* Pack and unpack operate on bitstreams. Each encoded 15-bit codeword is written
 * as 2 bytes (LSB first). encode: reads input bitstream 11 bits at a time.
 */

CeePewErr_t ecc_hamming_encode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len)
{
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len <= CEEPEW_MAX_MSG_BYTES, CEEPEW_ERR_BOUNDS);

    CEEPEW_ASSERT(fec_perm_ctx.initialised, CEEPEW_ERR_FEC);

    /* Treat *out_len as capacity in bytes on entry; will be set to actual produced bytes on return */
    uint16_t out_capacity = *out_len;
    CEEPEW_ASSERT(out_capacity > 0U, CEEPEW_ERR_BOUNDS);

    /* Bitstream read */
    uint16_t produced = 0U;
    uint32_t total_bits = (uint32_t)in_len * 8U;

    /* bounded loop over maximum possible codewords */
    for (uint16_t cw = 0U; cw < CEEPEW_MAX_HAMMING_CODEWORDS; cw++) {
        uint32_t bitpos = (uint32_t)cw * CEEPEW_FEC_DATA_SIZE;
        if (bitpos >= total_bits) { break; }

        /* gather 11 bits (pad with zeros if necessary) */
        uint16_t data_bits = 0U;
        for (uint8_t b = 0U; b < CEEPEW_FEC_DATA_SIZE; b++) {
            uint32_t src_bit = bitpos + b;
            uint8_t bit = 0U;
            if (src_bit < total_bits) {
                uint32_t byte_idx = src_bit / 8U;
                uint8_t bit_idx = (uint8_t)(src_bit % 8U);
                bit = (in[byte_idx] >> bit_idx) & 0x1U;
            }
            data_bits |= (uint16_t)(bit << b);
        }

        /* Construct systematic codeword: positions 1,2,4,8 are parity (1-based)
         * We'll place data into the remaining positions in increasing order.
         */
        uint16_t code = 0U;
        uint8_t data_i = 0U;
        for (uint8_t pos = 1U; pos <= CEEPEW_FEC_BLOCK_SIZE; pos++) {
            /* parity positions are powers of two */
            bool is_parity = ((pos & (pos - 1U)) == 0U);
            if (!is_parity) {
                uint8_t bit = (data_bits >> data_i) & 0x1U;
                code |= (uint16_t)(bit << (pos - 1U));
                data_i++;
            }
        }

        /* Compute parity bits */
        /* parity bit 1 covers positions with LSB=1: positions 1,3,5,7,9,11,13,15 */
        uint16_t p1_mask = 0x5555U; /* 0101010101010101 */
        uint16_t p2_mask = 0x3333U; /* 0011001100110011 */
        uint16_t p4_mask = 0x0F0FU; /* 0000111100001111 */
        uint16_t p8_mask = 0x00FFU; /* 0000000011111111 */

        uint8_t p1 = parity_of(code, p1_mask);
        uint8_t p2 = parity_of(code, p2_mask);
        uint8_t p4 = parity_of(code, p4_mask);
        uint8_t p8 = parity_of(code, p8_mask);

        code |= (uint16_t)(p1 << (1 - 1));
        code |= (uint16_t)(p2 << (2 - 1));
        code |= (uint16_t)(p4 << (4 - 1));
        code |= (uint16_t)(p8 << (8 - 1));

        /* Apply session permutation to code bits before transmission. */
        uint16_t permuted_code = 0U;
        for (uint8_t p = 0U; p < CEEPEW_FEC_BLOCK_SIZE; p++) {
            uint8_t src = fec_perm_ctx.perm[p];
            uint8_t bit = (uint8_t)((code >> src) & 0x1U);
            permuted_code |= (uint16_t)(bit << p);
        }

        /* Write 15-bit permuted code as two bytes, LSB first */
        /* Ensure we don't exceed provided out_capacity */
        uint32_t write_pos = (uint32_t)produced * 2U;
        CEEPEW_ASSERT(write_pos + 2U <= out_capacity, CEEPEW_ERR_BOUNDS);
        out[write_pos + 0U] = (uint8_t)(permuted_code & 0xFFU);
        out[write_pos + 1U] = (uint8_t)((permuted_code >> 8) & 0x7FU);

        produced++;
        bitpos += CEEPEW_FEC_DATA_SIZE;
        /* enforce compile-time bounded loop behavior in caller; here safe */
    }

    *out_len = (uint16_t)(produced * 2U);
    return CEEPEW_OK;
}

CeePewErr_t ecc_hamming_decode(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, bool *corrected)
{
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL && corrected != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len % 2U == 0U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(fec_perm_ctx.initialised, CEEPEW_ERR_FEC);

    uint16_t codewords = in_len / 2U;
    uint32_t bitpos = 0U; /* output bit position */
    *corrected = false;

    /* Zero output buffer for the expected output length */
    uint16_t out_bytes_est = (uint16_t)((codewords * CEEPEW_FEC_DATA_SIZE + 7U) / 8U);
    if (out_bytes_est > 0U) { memset(out, 0, (size_t)out_bytes_est); }

    /* bounded loop over maximum possible codewords; break when processed all */
    for (uint16_t cw = 0U; cw < CEEPEW_MAX_HAMMING_CODEWORDS; cw++) {
        if (cw >= codewords) { break; }

        uint16_t permuted_code = (uint16_t)in[cw * 2U] | ((uint16_t)in[cw * 2U + 1U] << 8U);

        /* Un-permute received columns to recover original codeword ordering. */
        uint16_t code = 0U;
        for (uint8_t p = 0U; p < CEEPEW_FEC_BLOCK_SIZE; p++) {
            uint8_t bit = (uint8_t)((permuted_code >> p) & 0x1U);
            uint8_t dst = fec_perm_ctx.perm[p];
            code |= (uint16_t)(bit << dst);
        }

        /* compute syndrome bits on unpermuted code */
        uint8_t s1 = parity_of(code, 0x5555U);
        uint8_t s2 = parity_of(code, 0x3333U);
        uint8_t s4 = parity_of(code, 0x0F0FU);
        uint8_t s8 = parity_of(code, 0x00FFU);
        uint8_t syndrome = (uint8_t)((s8 << 3) | (s4 << 2) | (s2 << 1) | s1);

        if (syndrome != 0U) {
            uint8_t flip_pos = SYNDROME_TABLE[syndrome];
            if (flip_pos > 0U && flip_pos <= CEEPEW_FEC_BLOCK_SIZE) {
                /* flip that bit (1-based -> 0-based) */
                code ^= (uint16_t)1U << (flip_pos - 1U);
                *corrected = true;
            } else {
                /* uncorrectable? leave as-is */
            }
        }

        /* extract data bits (positions that are not powers of two) */
        uint8_t data_i = 0U;
        for (uint8_t pos = 1U; pos <= CEEPEW_FEC_BLOCK_SIZE; pos++) {
            bool is_parity = ((pos & (pos - 1U)) == 0U);
            if (!is_parity) {
                uint8_t bit = (uint8_t)((code >> (pos - 1U)) & 0x1U);
                uint32_t out_bit = bitpos + data_i;
                uint32_t byte_idx = out_bit / 8U;
                uint8_t bit_idx = (uint8_t)(out_bit % 8U);
                out[byte_idx] &= (uint8_t)~(1U << bit_idx);
                out[byte_idx] |= (uint8_t)(bit << bit_idx);
                data_i++;
            }
        }

        bitpos += CEEPEW_FEC_DATA_SIZE;
    }

    *out_len = (uint16_t)((bitpos + 7U) / 8U);
    return CEEPEW_OK;
}
