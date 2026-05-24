/* components/compress/compress_huffman.h
 *
 * Production-grade static Huffman codec for CEE-PEW message compression.
 *
 * Features:
 * - Static Huffman table trained on English letter frequencies
 * - Real bit-packing (LSB-first) for compressed output
 * - Escape sequences for symbols outside primary table
 * - Passthrough mode when compressed size >= original size
 * - No dynamic allocation; bounded buffer operations
 *
 * DESIGN:
 * The primary code table covers 51 symbols (a-z, A-Z, 0-9, space, common punctuation).
 * Uncommon symbols are encoded via 12-bit escape: ESC_CODE (0x0FFF) + 8-bit symbol.
 * Passthrough flag in first 2 bits of output: 0b11 = passthrough, 0b10 = compressed.
 *
 * THREAD SAFETY: All functions are stateless. Caller responsible for buffer
 * synchronization if compressing/decompressing the same buffer from multiple tasks.
 */
#ifndef COMPRESS_HUFFMAN_H
#define COMPRESS_HUFFMAN_H

#include <stdint.h>
#include <stdbool.h>
#include "ceepew_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────── */
/* Constants                                                              */
/* ────────────────────────────────────────────────────────────────────── */

/* Number of symbols in primary Huffman table */
#define CEEPEW_HUFFMAN_PRIMARY_SYMBOLS  51U

/* Escape code for symbols not in primary table */
#define CEEPEW_HUFFMAN_ESCAPE_CODE      (0x0FFFU)  /* 12-bit sentinel */

/* Passthrough mode flag (stored in first 2 bits of output byte 0) */
#define CEEPEW_HUFFMAN_FLAG_PASSTHROUGH 0x3U       /* 0b11 */
#define CEEPEW_HUFFMAN_FLAG_COMPRESSED  0x2U       /* 0b10 */

/* Maximum input/output sizes (region allocator bound) */
#define CEEPEW_HUFFMAN_MAX_INPUT_BYTES  (160U)
#define CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES (200U)

/* ────────────────────────────────────────────────────────────────────── */
/* Huffman Encoding Entry (static table)                                  */
/* ────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  symbol;       /* ASCII character or value (a-z, A-Z, 0-9, space, etc.) */
    uint16_t code;         /* Huffman code word (MSB-first, up to 12 bits) */
    uint8_t  code_len;     /* Length in bits (1-12) */
} CeePewHuffEntry_t;

/* ────────────────────────────────────────────────────────────────────── */
/* Compression Statistics                                                */
/* ────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t symbols_encoded;      /* Number of symbols successfully encoded */
    uint32_t escape_sequences;     /* Number of escape codes used */
    uint8_t  passthrough_applied;  /* 1 if output was passthrough, 0 if compressed */
    uint32_t input_bits;           /* Total input bits */
    uint32_t output_bits;          /* Total output bits (including flag) */
} CeePewHuffStats_t;

/* ────────────────────────────────────────────────────────────────────── */
/* Function Declarations                                                 */
/* ────────────────────────────────────────────────────────────────────── */

/* Compress input data using static Huffman coding.
 *
 * PARAMETERS:
 *   in:         Input plaintext buffer (not NULL unless in_len == 0)
 *   in_len:     Input length (0 to CEEPEW_HUFFMAN_MAX_INPUT_BYTES)
 *   out:        Output buffer for compressed or passthrough data (not NULL)
 *   out_len:    [OUT] Actual output length written (not NULL)
 *   max_out_len: Size of output buffer (> 0)
 *   stats:      [OUT] Compression statistics (may be NULL)
 *
 * RETURNS:
 *   CEEPEW_OK             — Compression succeeded (may be passthrough)
 *   CEEPEW_ERR_NULL_PTR   — in or out is NULL (and in_len > 0)
 *   CEEPEW_ERR_BOUNDS     — Output buffer too small or invalid length
 *   CEEPEW_ERR_PARAM      — Invalid parameter (max_out_len == 0)
 *
 * NOTES:
 *   - If compressed output >= input size, passthrough is used
 *   - First 2 bits of output[0] indicate mode (PASSTHROUGH vs COMPRESSED)
 *   - Remaining output bits are compressed stream or plaintext copy
 *   - stats may be NULL; if provided, will be populated
 *
 * ASSERTION REQUIREMENTS:
 *   - in != NULL || in_len == 0
 *   - out != NULL
 *   - out_len != NULL
 *   - max_out_len > 0
 *   - in_len <= CEEPEW_HUFFMAN_MAX_INPUT_BYTES
 */
CeePewErr_t compress_huffman_compress(
    const uint8_t *in,
    uint16_t in_len,
    uint8_t *out,
    uint16_t *out_len,
    uint16_t max_out_len,
    CeePewHuffStats_t *stats
);

/* Decompress Huffman-compressed or passthrough data.
 *
 * PARAMETERS:
 *   in:         Compressed/passthrough buffer from compress_huffman_compress()
 *   in_len:     Length of compressed data (1 to CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES)
 *   out:        Output plaintext buffer (not NULL)
 *   out_len:    [OUT] Actual output length written (not NULL)
 *   max_out_len: Size of output buffer (> 0)
 *
 * RETURNS:
 *   CEEPEW_OK             — Decompression succeeded
 *   CEEPEW_ERR_NULL_PTR   — in or out is NULL
 *   CEEPEW_ERR_BOUNDS     — Output buffer too small or bit stream corrupt
 *   CEEPEW_ERR_PARAM      — Invalid parameter or corrupt flag bits
 *   CEEPEW_ERR_CRYPTO     — Huffman tree mismatch (shouldn't happen; indicates corruption)
 *
 * NOTES:
 *   - Reads first 2 bits of in[0] to determine mode (PASSTHROUGH or COMPRESSED)
 *   - If passthrough: copies bits[2:] (plus remaining bytes) to output
 *   - If compressed: decodes using static Huffman tree
 *   - Corrupt bit streams may cause bit misalignment; output will be truncated
 *
 * ASSERTION REQUIREMENTS:
 *   - in != NULL
 *   - out != NULL
 *   - out_len != NULL
 *   - max_out_len > 0
 *   - in_len > 0
 */
CeePewErr_t compress_huffman_decompress(
    const uint8_t *in,
    uint16_t in_len,
    uint8_t *out,
    uint16_t *out_len,
    uint16_t max_out_len
);

/* Get symbol from primary Huffman table by index.
 *
 * PARAMETERS:
 *   idx:  Index into primary table (0 to CEEPEW_HUFFMAN_PRIMARY_SYMBOLS-1)
 *
 * RETURNS:
 *   Pointer to CeePewHuffEntry_t, or NULL if index out of bounds
 *
 * NOTES:
 *   - For diagnostics and testing only
 *   - Table is flash-resident; pointer valid for duration of program
 */
const CeePewHuffEntry_t *compress_huffman_get_table_entry(uint32_t idx);

/* Get compression ratio estimate without actually compressing.
 *
 * Useful for deciding whether to compress before allocating buffers.
 *
 * PARAMETERS:
 *   data:       Plaintext to analyze (not NULL unless len == 0)
 *   len:        Length of data (0 to CEEPEW_HUFFMAN_MAX_INPUT_BYTES)
 *
 * RETURNS:
 *   Estimated compressed size in bytes (upper bound; actual may be smaller)
 *
 * NOTES:
 *   - Does NOT perform actual compression
 *   - Scans input and estimates based on frequency of each symbol
 *   - If estimate >= len, compression will use passthrough
 *
 * ASSERTION REQUIREMENTS:
 *   - data != NULL || len == 0
 *   - len <= CEEPEW_HUFFMAN_MAX_INPUT_BYTES
 */
uint16_t compress_huffman_estimate_output_size(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* COMPRESS_HUFFMAN_H */
