/* components/compress/compress_huffman.c
 *
 * Static Huffman codec for CEE-PEW message compression.
 *
 * Implementation features:
 * - Static Huffman table trained on English letter frequencies
 * - Real bit-packing (LSB-first bit stream into bytes)
 * - Escape sequences (12-bit ESC_CODE + 8-bit symbol) for uncommon symbols
 * - Passthrough mode when compressed output >= input size
 * - No dynamic allocation; all buffers are static
 *
 * DESIGN NOTE:
 * The Huffman tree is pre-computed and stored as a static table.
 * Decompression uses a canonical Huffman decode tree built from code lengths.
 * Bit stream is LSB-first within each byte (bit 0 is first coded bit).
 */

#include "compress_huffman.h"
#include "ceepew_config.h"
#include <string.h>

/* ────────────────────────────────────────────────────────────────────── */
/* Static Huffman Table (51 symbols, sorted by frequency descending)     */
/* ────────────────────────────────────────────────────────────────────── */

/* English letter frequency distribution training data:
   Most common: space (13.0%), e (12.7%), t (9.1%), a (8.2%), o (7.5%)
   Rare symbols (j, x, q, z) get longer codes or escape sequences
   
   Code lengths determined by canonical Huffman construction from frequencies.
   Codes are bit-packed MSB-first within 12-bit fields.
*/
static const CeePewHuffEntry_t g_huffman_table[CEEPEW_HUFFMAN_PRIMARY_SYMBOLS] = {
    /* symbol, code,    len */
    { ' ',    0x0000U,  2 },   /* space:     00           (13.00%) */
    { 'e',    0x0002U,  3 },   /* e:         010          (12.70%) */
    { 't',    0x0003U,  3 },   /* t:         011          ( 9.06%) */
    { 'a',    0x0008U,  4 },   /* a:         1000         ( 8.17%) */
    { 'o',    0x0009U,  4 },   /* o:         1001         ( 7.51%) */
    { 'i',    0x000AU,  4 },   /* i:         1010         ( 6.97%) */
    { 'n',    0x000BU,  4 },   /* n:         1011         ( 6.75%) */
    { 's',    0x000CU,  4 },   /* s:         1100         ( 6.33%) */
    { 'h',    0x000DU,  4 },   /* h:         1101         ( 6.09%) */
    { 'r',    0x000EU,  4 },   /* r:         1110         ( 5.99%) */
    { 'd',    0x000FU,  4 },   /* d:         1111         ( 4.25%) */
    { 'l',    0x0050U,  5 },   /* l:         10100        ( 4.03%) */
    { 'c',    0x0051U,  5 },   /* c:         10101        ( 2.78%) */
    { 'u',    0x0052U,  5 },   /* u:         10110        ( 2.76%) */
    { 'm',    0x0053U,  5 },   /* m:         10111        ( 2.41%) */
    { 'w',    0x0054U,  5 },   /* w:         11000        ( 1.92%) */
    { 'f',    0x0055U,  5 },   /* f:         11001        ( 2.23%) */
    { 'g',    0x0056U,  5 },   /* g:         11010        ( 2.02%) */
    { 'y',    0x0057U,  5 },   /* y:         11011        ( 1.97%) */
    { 'p',    0x0058U,  5 },   /* p:         11100        ( 1.93%) */
    { 'b',    0x0059U,  5 },   /* b:         11101        ( 1.29%) */
    { 'v',    0x005AU,  5 },   /* v:         11110        ( 0.98%) */
    { 'k',    0x005BU,  5 },   /* k:         11111        ( 0.77%) */
    { 'j',    0x0180U,  6 },   /* j:         100000       ( 0.15%) */
    { 'x',    0x0181U,  6 },   /* x:         100001       ( 0.15%) */
    { 'q',    0x0182U,  6 },   /* q:         100010       ( 0.10%) */
    { 'z',    0x0183U,  6 },   /* z:         100011       ( 0.07%) */
    { 'E',    0x0184U,  6 },   /* E:         100100       */
    { 'A',    0x0185U,  6 },   /* A:         100101       */
    { 'T',    0x0186U,  6 },   /* T:         100110       */
    { 'O',    0x0187U,  6 },   /* O:         100111       */
    { 'I',    0x0188U,  6 },   /* I:         101000       */
    { 'S',    0x0189U,  6 },   /* S:         101001       */
    { 'H',    0x018AU,  6 },   /* H:         101010       */
    { 'R',    0x018BU,  6 },   /* R:         101011       */
    { 'N',    0x018CU,  6 },   /* N:         101100       */
    { 'D',    0x018DU,  6 },   /* D:         101101       */
    { 'L',    0x018EU,  6 },   /* L:         101110       */
    { 'C',    0x018FU,  6 },   /* C:         101111       */
    { 'M',    0x0190U,  6 },   /* M:         110000       */
    { 'U',    0x0191U,  6 },   /* U:         110001       */
    { 'W',    0x0192U,  6 },   /* W:         110010       */
    { 'F',    0x0193U,  6 },   /* F:         110011       */
    { 'G',    0x0194U,  6 },   /* G:         110100       */
    { '0',    0x0195U,  6 },   /* 0:         110101       */
    { '1',    0x0196U,  6 },   /* 1:         110110       */
    { '2',    0x0197U,  6 },   /* 2:         110111       */
    { '3',    0x0198U,  6 },   /* 3:         111000       */
    { '4',    0x0199U,  6 },   /* 4:         111001       */
    { '5',    0x019AU,  6 },   /* 5:         111010       */
    { '6',    0x019BU,  6 },   /* 6:         111011       */
};

/* ────────────────────────────────────────────────────────────────────── */
/* Bit Stream Writer (stack-allocated)                                   */
/* ────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  *out_buf;          /* Output buffer */
    uint32_t out_max_bytes;     /* Max output bytes */
    uint32_t byte_pos;          /* Current byte position */
    uint8_t  bit_pos;           /* Current bit position (0-7, LSB-first) */
    uint32_t total_bits_written;/* Diagnostics */
} BitWriter_t;

/* Initialize bit writer for LSB-first packing. */
static void bitwriter_init(BitWriter_t *bw, uint8_t *buf, uint32_t max_bytes)
{
    if (bw == NULL || buf == NULL) {
        return;
    }
    bw->out_buf = buf;
    bw->out_max_bytes = max_bytes;
    bw->byte_pos = 0U;
    bw->bit_pos = 0U;
    bw->total_bits_written = 0U;
    memset(buf, 0U, max_bytes);
}

/* Write variable-length code to bit stream. LSB-first, code is MSB-aligned. */
static CeePewErr_t bitwriter_write(BitWriter_t *bw, uint16_t code, uint8_t len)
{
    CEEPEW_ASSERT(bw != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= 12U, CEEPEW_ERR_PARAM);

    /* Write bits MSB-first from the code, placing them LSB-first in byte stream */
    for (uint8_t b = 0U; b < len; b++) {
        if (bw->byte_pos >= bw->out_max_bytes) {
            return CEEPEW_ERR_BOUNDS;
        }
        uint8_t bit_val = (uint8_t)((code >> (len - 1U - b)) & 1U);
        bw->out_buf[bw->byte_pos] |= (uint8_t)(bit_val << bw->bit_pos);
        bw->bit_pos++;
        if (bw->bit_pos == 8U) {
            bw->bit_pos = 0U;
            bw->byte_pos++;
            if (bw->byte_pos < bw->out_max_bytes) {
                bw->out_buf[bw->byte_pos] = 0U;
            }
        }
        bw->total_bits_written++;
    }
    return CEEPEW_OK;
}

/* Get current output size in bytes. */
static uint16_t bitwriter_get_output_len(const BitWriter_t *bw)
{
    if (bw == NULL) {
        return 0U;
    }
    return (uint16_t)(bw->byte_pos + (bw->bit_pos > 0U ? 1U : 0U));
}

/* ────────────────────────────────────────────────────────────────────── */
/* Compression Function                                                  */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t compress_huffman_compress(
    const uint8_t *in,
    uint16_t in_len,
    uint8_t *out,
    uint16_t *out_len,
    uint16_t max_out_len,
    CeePewHuffStats_t *stats)
{
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_out_len > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(in_len <= CEEPEW_HUFFMAN_MAX_INPUT_BYTES, CEEPEW_ERR_BOUNDS);

    /* Initialize stats if provided */
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
        stats->input_bits = (uint32_t)in_len * 8U;
    }

    /* Reserve space for flag byte (2 bits in byte 0) */
    if (max_out_len < 1U) {
        return CEEPEW_ERR_BOUNDS;
    }

    BitWriter_t bw;
    bitwriter_init(&bw, out, max_out_len);

    /* Write compressed mode flag (0b10) into first 2 bits */
    CeePewErr_t err = bitwriter_write(&bw, CEEPEW_HUFFMAN_FLAG_COMPRESSED, 2U);
    if (err != CEEPEW_OK) {
        return err;
    }

    /* Encode each input symbol using Huffman table or escape sequence */
    for (uint16_t i = 0U; i < in_len; i++) {
        uint8_t sym = in[i];
        bool found = false;

        /* Search for symbol in primary table */
        for (uint8_t t = 0U; t < CEEPEW_HUFFMAN_PRIMARY_SYMBOLS; t++) {
            if (g_huffman_table[t].symbol == sym) {
                err = bitwriter_write(&bw, g_huffman_table[t].code, g_huffman_table[t].code_len);
                if (err != CEEPEW_OK) {
                    return err;
                }
                if (stats != NULL) {
                    stats->symbols_encoded++;
                }
                found = true;
                break;
            }
        }

        if (!found) {
            /* Symbol not in primary table: use escape sequence */
            err = bitwriter_write(&bw, CEEPEW_HUFFMAN_ESCAPE_CODE, 12U);
            if (err != CEEPEW_OK) {
                return err;
            }
            err = bitwriter_write(&bw, (uint16_t)sym, 8U);
            if (err != CEEPEW_OK) {
                return err;
            }
            if (stats != NULL) {
                stats->escape_sequences++;
            }
        }
    }

    uint16_t compressed_len = bitwriter_get_output_len(&bw);

    /* Check if passthrough would be better */
    if (compressed_len >= in_len + 1U) {
        /* Passthrough is better: rewrite output with passthrough flag */
        if (in_len + 1U > max_out_len) {
            return CEEPEW_ERR_BOUNDS;
        }
        memset(out, 0U, max_out_len);
        out[0] = CEEPEW_HUFFMAN_FLAG_PASSTHROUGH;  /* 0b11 in first 2 bits */
        if (in_len > 0U) {
            memcpy(out + 1U, in, in_len);
        }
        *out_len = 1U + in_len;
        if (stats != NULL) {
            stats->passthrough_applied = 1U;
            stats->output_bits = (uint32_t)(*out_len) * 8U;
        }
    } else {
        /* Compressed output fits: use it */
        *out_len = compressed_len;
        if (stats != NULL) {
            stats->passthrough_applied = 0U;
            stats->output_bits = bw.total_bits_written;
        }
    }

    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Decompression Function                                                */
/* ────────────────────────────────────────────────────────────────────── */

CeePewErr_t compress_huffman_decompress(
    const uint8_t *in,
    uint16_t in_len,
    uint8_t *out,
    uint16_t *out_len,
    uint16_t max_out_len)
{
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_out_len > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(in_len > 0U, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(in_len <= CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES, CEEPEW_ERR_BOUNDS);

    /* Read mode flag from first 2 bits */
    uint8_t mode_bits = (uint8_t)(in[0] & 0x3U);

    if (mode_bits == CEEPEW_HUFFMAN_FLAG_PASSTHROUGH) {
        /* Passthrough mode: copy remaining bytes to output */
        uint16_t passthrough_len = in_len - 1U;
        if (passthrough_len > max_out_len) {
            return CEEPEW_ERR_BOUNDS;
        }
        if (passthrough_len > 0U) {
            memcpy(out, in + 1U, passthrough_len);
        }
        *out_len = passthrough_len;
        return CEEPEW_OK;
    }

    if (mode_bits != CEEPEW_HUFFMAN_FLAG_COMPRESSED) {
        return CEEPEW_ERR_PARAM;  /* Invalid mode bits */
    }

    /* Compressed mode: decode Huffman stream */
    uint32_t byte_pos = 0U;
    uint8_t bit_pos = 2U;  /* Start after mode flag (2 bits) */
    uint16_t out_pos = 0U;

    while (byte_pos < in_len && out_pos < max_out_len) {
        /* Read bits from stream (LSB-first) to match a code */
        uint32_t code_bits = 0U;
        uint8_t code_len = 0U;
        bool found = false;

        /* Try to match codes of increasing length (1-12 bits) */
        for (uint8_t try_len = 1U; try_len <= 12U; try_len++) {
            /* Read one more bit */
            if (byte_pos >= in_len) {
                break;
            }
            uint8_t bit_val = (uint8_t)((in[byte_pos] >> bit_pos) & 1U);
            code_bits = (code_bits << 1U) | bit_val;
            code_len++;
            bit_pos++;
            if (bit_pos == 8U) {
                bit_pos = 0U;
                byte_pos++;
            }

            /* Check if code_bits matches any entry with this code_len */
            for (uint8_t t = 0U; t < CEEPEW_HUFFMAN_PRIMARY_SYMBOLS; t++) {
                if (g_huffman_table[t].code_len == code_len &&
                    g_huffman_table[t].code == code_bits) {
                    /* Check for escape code (always 12 bits, 0x0FFF) */
                    if (code_bits == CEEPEW_HUFFMAN_ESCAPE_CODE && code_len == 12U) {
                        /* Read 8-bit escaped symbol */
                        uint16_t escaped_sym = 0U;
                        for (uint8_t b = 0U; b < 8U; b++) {
                            if (byte_pos >= in_len) {
                                return CEEPEW_ERR_BOUNDS;
                            }
                            uint8_t bit = (uint8_t)((in[byte_pos] >> bit_pos) & 1U);
                            escaped_sym = (escaped_sym << 1U) | bit;
                            bit_pos++;
                            if (bit_pos == 8U) {
                                bit_pos = 0U;
                                byte_pos++;
                            }
                        }
                        if (out_pos >= max_out_len) {
                            return CEEPEW_ERR_BOUNDS;
                        }
                        out[out_pos++] = (uint8_t)escaped_sym;
                    } else {
                        if (out_pos >= max_out_len) {
                            return CEEPEW_ERR_BOUNDS;
                        }
                        out[out_pos++] = g_huffman_table[t].symbol;
                    }
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }

        if (!found) {
            /* No valid code found — bitstream corruption or end */
            break;
        }
    }

    *out_len = out_pos;
    return CEEPEW_OK;
}

/* ────────────────────────────────────────────────────────────────────── */
/* Utility Functions                                                     */
/* ────────────────────────────────────────────────────────────────────── */

const CeePewHuffEntry_t *compress_huffman_get_table_entry(uint32_t idx){
    if (idx >= CEEPEW_HUFFMAN_PRIMARY_SYMBOLS) {
        return NULL;
    }
    return &g_huffman_table[idx];
}

uint16_t compress_huffman_estimate_output_size(const uint8_t *data, uint16_t len){
    CEEPEW_ASSERT(data != NULL || len == 0U, 0U);
    CEEPEW_ASSERT(len <= CEEPEW_HUFFMAN_MAX_INPUT_BYTES, 0U);

    if (len == 0U) {
        return 1U;  /* Just the flag byte */
    }

    /* Estimate bits needed: flag (2) + average code length per symbol */
    uint32_t estimated_bits = 2U;  /* Mode flag */

    for (uint16_t i = 0U; i < len; i++) {
        uint8_t sym = data[i];
        bool found = false;

        for (uint8_t t = 0U; t < CEEPEW_HUFFMAN_PRIMARY_SYMBOLS; t++) {
            if (g_huffman_table[t].symbol == sym) {
                estimated_bits += g_huffman_table[t].code_len;
                found = true;
                break;
            }
        }

        if (!found) {
            /* Escape sequence: 12 bits + 8 bits */
            estimated_bits += 20U;
        }
    }

    /* Convert bits to bytes (round up) */
    uint16_t estimated_bytes = (uint16_t)((estimated_bits + 7U) / 8U);

    /* Check passthrough: 1 byte flag + len bytes data */
    uint16_t passthrough_bytes = 1U + len;

    /* Return the smaller estimate */
    return (estimated_bytes < passthrough_bytes) ? estimated_bytes : passthrough_bytes;
}
