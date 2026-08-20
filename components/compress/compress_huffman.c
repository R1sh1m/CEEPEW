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
/* Static Huffman Table (88 symbols, canonical prefix-free codes)        */
/* ────────────────────────────────────────────────────────────────────── */

/* Canonical prefix-free Huffman codes derived from English letter
   frequency distribution. Code lengths (5/6/7/8 bits) satisfy the Kraft
   inequality: Σ 2^(-len) = 10/32 + 24/64 + 24/128 + 30/256 = 0.9922 ≤ 1.0.

   The table covers the complete compose keyboard charset (a-z, A-Z, 0-9,
   space, and common punctuation) so user-typed messages never require
   escapes. The 12-bit escape sentinel (0x0FE0) is prefix-disjoint from
   every table code at all lengths 1..11, making escape decoding
   unambiguous.

   Codes are assigned MSB-first in the bit stream (encoder writes the
   highest bit of the code value first). Sorted by frequency descending
   within each code length group for canonical code assignment.

   Frequency distribution: space (~13%), e (~12.7%), t (~9.1%),
   a/o/i/n/s/h/r/d (~4-8%), l/c/u/m/w/f/g/y/p/b/v/k (~1-4%),
   j/x/q/z/E/A/T/O/I/S/H/R/N/D/L/C/M/U/W/F/G/Y/P/B/V/K/J/X/Q/Z/0-9
   and punctuation (<1% each).
*/
static const CeePewHuffEntry_t g_huffman_table[CEEPEW_HUFFMAN_PRIMARY_SYMBOLS] = {
    /* Len 5 (10 most frequent symbols) */
    { ' ',    0x0000U,  5 },
    { 'e',    0x0001U,  5 },
    { 't',    0x0002U,  5 },
    { 'a',    0x0003U,  5 },
    { 'o',    0x0004U,  5 },
    { 'i',    0x0005U,  5 },
    { 'n',    0x0006U,  5 },
    { 's',    0x0007U,  5 },
    { 'h',    0x0008U,  5 },
    { 'r',    0x0009U,  5 },
    /* Len 6 (24 medium-frequency symbols) */
    { 'd',    0x0014U,  6 },
    { 'l',    0x0015U,  6 },
    { 'c',    0x0016U,  6 },
    { 'u',    0x0017U,  6 },
    { 'm',    0x0018U,  6 },
    { 'w',    0x0019U,  6 },
    { 'f',    0x001AU,  6 },
    { 'g',    0x001BU,  6 },
    { 'y',    0x001CU,  6 },
    { 'p',    0x001DU,  6 },
    { 'b',    0x001EU,  6 },
    { 'v',    0x001FU,  6 },
    { 'k',    0x0020U,  6 },
    { 'j',    0x0021U,  6 },
    { 'x',    0x0022U,  6 },
    { 'q',    0x0023U,  6 },
    { 'z',    0x0024U,  6 },
    { 'E',    0x0025U,  6 },
    { 'A',    0x0026U,  6 },
    { 'T',    0x0027U,  6 },
    { 'O',    0x0028U,  6 },
    { 'I',    0x0029U,  6 },
    { 'S',    0x002AU,  6 },
    { 'H',    0x002BU,  6 },
    /* Len 7 (24 less frequent symbols) */
    { 'R',    0x0058U,  7 },
    { 'N',    0x0059U,  7 },
    { 'D',    0x005AU,  7 },
    { 'L',    0x005BU,  7 },
    { 'C',    0x005CU,  7 },
    { 'M',    0x005DU,  7 },
    { 'U',    0x005EU,  7 },
    { 'W',    0x005FU,  7 },
    { 'F',    0x0060U,  7 },
    { 'G',    0x0061U,  7 },
    { 'Y',    0x0062U,  7 },
    { 'P',    0x0063U,  7 },
    { 'B',    0x0064U,  7 },
    { 'V',    0x0065U,  7 },
    { 'K',    0x0066U,  7 },
    { 'J',    0x0067U,  7 },
    { 'X',    0x0068U,  7 },
    { 'Q',    0x0069U,  7 },
    { 'Z',    0x006AU,  7 },
    { '0',    0x006BU,  7 },
    { '1',    0x006CU,  7 },
    { '2',    0x006DU,  7 },
    { '3',    0x006EU,  7 },
    { '4',    0x006FU,  7 },
    /* Len 8 (30 least frequent symbols: digits 5-9, punctuation) */
    { '5',    0x00E0U,  8 },
    { '6',    0x00E1U,  8 },
    { '7',    0x00E2U,  8 },
    { '8',    0x00E3U,  8 },
    { '9',    0x00E4U,  8 },
    { '.',    0x00E5U,  8 },
    { ',',    0x00E6U,  8 },
    { '!',    0x00E7U,  8 },
    { '?',    0x00E8U,  8 },
    { ';',    0x00E9U,  8 },
    { ':',    0x00EAU,  8 },
    { '\'',   0x00EBU,  8 },
    { '"',    0x00ECU,  8 },
    { '_',    0x00EDU,  8 },
    { '-',    0x00EEU,  8 },
    { '@',    0x00EFU,  8 },
    { '#',    0x00F0U,  8 },
    { '/',    0x00F1U,  8 },
    { '(',    0x00F2U,  8 },
    { ')',    0x00F3U,  8 },
    { '+',    0x00F4U,  8 },
    { '=',    0x00F5U,  8 },
    { '%',    0x00F6U,  8 },
    { '&',    0x00F7U,  8 },
    { '*',    0x00F8U,  8 },
    { '<',    0x00F9U,  8 },
    { '>',    0x00FAU,  8 },
    { '[',    0x00FBU,  8 },
    { ']',    0x00FCU,  8 },
    { '{',    0x00FDU,  8 },
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

    /* If passthrough would not be worse, skip compression entirely.
     * This avoids overflowing the output buffer with escape sequences
     * for incompressible data, and saves CPU. */
    if (in_len > 0U && compress_huffman_estimate_output_size(in, in_len) >= in_len + 1U) {
        if (in_len + 1U > max_out_len) {
            return CEEPEW_ERR_BOUNDS;
        }
        memset(out, 0U, max_out_len);
        out[0] = CEEPEW_HUFFMAN_FLAG_PASSTHROUGH;
        memcpy(out + 1U, in, in_len);
        *out_len = 1U + in_len;
        if (stats != NULL) {
            stats->passthrough_applied = 1U;
            stats->output_bits = (uint32_t)(*out_len) * 8U;
            stats->symbols_encoded = in_len;
        }
        return CEEPEW_OK;
    }

    /* Reserve space for flag byte (2 bits in byte 0) */
    if (max_out_len < 1U) {
        return CEEPEW_ERR_BOUNDS;
    }

    BitWriter_t bw;
    bitwriter_init(&bw, out, max_out_len);

    /* Write compressed mode flag into first 2 bits of byte 0,
     * followed by the original input length (2 bytes, little-endian).
     * The length prefix lets the decoder stop after exactly in_len
     * symbols, avoiding false matches from zero-padding bits. */
    out[0] = CEEPEW_HUFFMAN_FLAG_COMPRESSED; /* 0x02 → bits 1:0 = 10 */
    out[1] = (uint8_t)(in_len & 0xFFU);
    out[2] = (uint8_t)((in_len >> 8) & 0xFFU);
    bw.bit_pos = 0U;
    bw.byte_pos = 3U;   /* skip flag byte + 2 length bytes */
    bw.total_bits_written = 2U + 16U;

    CeePewErr_t err;

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

    /* OPTIONAL PADDING FOR SIDE-CHANNEL RESISTANCE:
     * 
     * If side-channel attack resistance is required, the caller can apply
     * optional fixed-size padding to the output to hide the compressed size:
     * 
     *   // Hide compressed size via constant-size padding
     *   uint16_t padded_len = CEEPEW_HUFFMAN_PAD_BLOCK_SIZE;
     *   if (*out_len < CEEPEW_HUFFMAN_PAD_BLOCK_SIZE) {
     *       memset(out + *out_len, CEEPEW_HUFFMAN_PAD_BYTE, 
     *              CEEPEW_HUFFMAN_PAD_BLOCK_SIZE - *out_len);
     *       *out_len = CEEPEW_HUFFMAN_PAD_BLOCK_SIZE;
     *   }
     * 
     * This prevents an eavesdropper from inferring message content by observing
     * the ciphertext length. Without padding, each message leaks its original
     * length class (short, medium, long messages compress differently).
     * 
     * Trade-off: Padding reduces bandwidth efficiency to improve secrecy.
     * Recommended padding block size: 64 bytes (aligns with crypto block size).
     * 
     * Note: Padding is NOT applied by default to keep this function focused
     * on compression. Padding should be applied by the session layer if needed.
     */

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

    /* Read original input length from bytes 1-2 (little-endian) */
    if (in_len < 3U) {
        return CEEPEW_ERR_BOUNDS;  /* Need at least 3 bytes (flag + 2 len) */
    }
    uint16_t expected_out_len = (uint16_t)((uint16_t)in[1] | ((uint16_t)in[2] << 8U));
    if (expected_out_len > max_out_len) {
        return CEEPEW_ERR_BOUNDS;
    }

    /* Compressed mode: decode Huffman stream */
    uint32_t byte_pos = 0U;
    uint8_t bit_pos = 2U;  /* Start after mode flag (2 bits, byte 0) */
    uint16_t out_pos = 0U;

    /* Skip to byte 3 (after flag + length prefix) */
    byte_pos = 3U;
    bit_pos = 0U;
    /* If length bytes straddle into subsequent bytes, advance byte_pos */
    if (byte_pos >= in_len) {
        return CEEPEW_ERR_BOUNDS;
    }

    while (out_pos < expected_out_len && out_pos < max_out_len) {
        /* Save stream position before this symbol */
        uint32_t save_byte = byte_pos;
        uint8_t save_bit  = bit_pos;

        uint32_t code_bits = 0U;
        uint8_t  code_len = 0U;
        bool     found    = false;
        uint8_t  best_len = 0U;
        uint8_t  best_idx = 0U;
        bool     is_escape = false;

        /* Read up to 12 bits, tracking the longest valid match.
         * The stored code values in the static table are not strictly
         * prefix-free (shorter codes can appear as prefixes of longer
         * bit sequences when read past symbol boundaries), so taking
         * the longest match resolves the ambiguity correctly.
         * The length prefix (bytes 1-2) ensures we stop after the
         * correct number of symbols regardless of trailing bits. */
        for (uint8_t try_len = 1U; try_len <= 12U; try_len++) {
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

            for (uint8_t t = 0U; t < CEEPEW_HUFFMAN_PRIMARY_SYMBOLS; t++) {
                if (g_huffman_table[t].code_len != code_len) {
                    continue;
                }
                /* Mask: compare only the lower code_len bits */
                uint16_t masked = g_huffman_table[t].code & ((1U << code_len) - 1U);
                if (masked == (uint16_t)code_bits) {
                    best_len = code_len;
                    best_idx = t;
                    found = true;
                }
            }
        }

        /* If no table entry matched, check for 12-bit escape code */
        if (!found && code_len >= 12U &&
            (code_bits & 0xFFFU) == CEEPEW_HUFFMAN_ESCAPE_CODE) {
            is_escape = true;
            found = true;
            best_len = 12U;
        }

        if (!found) {
            break;  /* Bitstream corruption */
        }

        if (is_escape) {
            /* Stream already advanced past the 12-bit escape code;
             * read 8 more bits for the literal symbol. */
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
            /* Rewind to start of symbol and consume exactly best_len bits */
            byte_pos = save_byte;
            bit_pos  = save_bit;
            for (uint8_t c = 0U; c < best_len; c++) {
                bit_pos++;
                if (bit_pos == 8U) {
                    bit_pos = 0U;
                    byte_pos++;
                }
            }
            if (out_pos >= max_out_len) {
                return CEEPEW_ERR_BOUNDS;
            }
            out[out_pos++] = g_huffman_table[best_idx].symbol;
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

    /* Estimate bits needed: 3-byte header (flag byte + 2-byte length prefix,
     * fully allocated as 24 bits by the writer) + symbol code lengths. */
    uint32_t estimated_bits = 24U;  /* Flag byte + length prefix bytes */

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
