/* tests/host/test_huffman.c — Static Huffman codec host unit test */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "compress_huffman.h"
#include "ceepew_security_utils.h"

static int test_roundtrip_english(void)
{
    const uint8_t input[] = "the quick brown fox jumps over the lazy dog";
    uint16_t in_len = (uint16_t)(sizeof(input) - 1U);
    uint8_t comp[CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES];
    uint16_t comp_len = 0;
    uint8_t decomp[CEEPEW_HUFFMAN_MAX_INPUT_BYTES];
    uint16_t decomp_len = 0;
    CeePewHuffStats_t stats;

    CeePewErr_t err = compress_huffman_compress(input, in_len, comp, &comp_len,
                                                 (uint16_t)sizeof(comp), &stats);
    if (err != CEEPEW_OK) {
        printf("FAIL: roundtrip_english compress returned %d\n", err);
        return 1;
    }

    err = compress_huffman_decompress(comp, comp_len, decomp, &decomp_len,
                                       (uint16_t)sizeof(decomp));
    if (err != CEEPEW_OK) {
        printf("FAIL: roundtrip_english decompress returned %d\n", err);
        return 1;
    }

    if (decomp_len != in_len || memcmp(decomp, input, in_len) != 0) {
        printf("FAIL: roundtrip_english data mismatch (%u vs %u)\n",
               (unsigned)decomp_len, (unsigned)in_len);
        printf("  input:  '");
        for (uint16_t i = 0; i < in_len; i++) printf("%c", input[i]);
        printf("'\n  output: '");
        for (uint16_t i = 0; i < decomp_len; i++) printf("%c", decomp[i] >= 32 && decomp[i] < 127 ? decomp[i] : '?');
        printf("'\n  diff: ");
        for (uint16_t i = 0; i < (in_len < decomp_len ? in_len : decomp_len); i++) {
            if (input[i] != decomp[i]) printf("pos%u(in=0x%02x out=0x%02x) ", i, input[i], decomp[i]);
        }
        printf("\n");
        return 1;
    }

    if (stats.passthrough_applied) {
        printf("FAIL: roundtrip_english unexpectedly used passthrough\n");
        return 1;
    }

    printf("PASS: round-trip English text (%u -> %u bytes, %.0f%%)\n",
           (unsigned)in_len, (unsigned)comp_len,
           100.0 * (double)comp_len / (double)in_len);
    return 0;
}

static int test_empty_input(void)
{
    uint8_t out[CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES];
    uint16_t out_len = 0;
    CeePewErr_t err = compress_huffman_compress(NULL, 0U, out, &out_len,
                                                 (uint16_t)sizeof(out), NULL);
    if (err != CEEPEW_OK) {
        printf("FAIL: empty_input compress returned %d\n", err);
        return 1;
    }
    if (out_len != 1U) {
        printf("FAIL: empty_input expected 1 byte output, got %u\n",
               (unsigned)out_len);
        return 1;
    }

    uint8_t decomp[1];
    uint16_t decomp_len = 99;
    err = compress_huffman_decompress(out, out_len, decomp, &decomp_len,
                                       (uint16_t)sizeof(decomp));
    if (err != CEEPEW_OK || decomp_len != 0U) {
        printf("FAIL: empty_input decompress returned %d, len=%u\n",
               err, (unsigned)decomp_len);
        return 1;
    }
    printf("PASS: empty input\n");
    return 0;
}

static int test_single_char(void)
{
    const uint8_t input[] = "a";
    uint16_t in_len = 1U;
    uint8_t comp[CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES];
    uint16_t comp_len = 0;
    uint8_t decomp[CEEPEW_HUFFMAN_MAX_INPUT_BYTES];
    uint16_t decomp_len = 0;

    CeePewErr_t err = compress_huffman_compress(input, in_len, comp, &comp_len,
                                                 (uint16_t)sizeof(comp), NULL);
    if (err != CEEPEW_OK) {
        printf("FAIL: single_char compress returned %d\n", err);
        return 1;
    }

    err = compress_huffman_decompress(comp, comp_len, decomp, &decomp_len,
                                       (uint16_t)sizeof(decomp));
    if (err != CEEPEW_OK || decomp_len != 1U || decomp[0] != 'a') {
        printf("FAIL: single_char round-trip failed\n");
        return 1;
    }
    printf("PASS: single character\n");
    return 0;
}

static int test_escape_symbols(void)
{
    const uint8_t input[] = {0xAB, 0xCD, 0xEF};
    uint16_t in_len = 3U;
    uint8_t comp[CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES];
    uint16_t comp_len = 0;
    uint8_t decomp[CEEPEW_HUFFMAN_MAX_INPUT_BYTES];
    uint16_t decomp_len = 0;
    CeePewHuffStats_t stats;

    CeePewErr_t err = compress_huffman_compress(input, in_len, comp, &comp_len,
                                                 (uint16_t)sizeof(comp), &stats);
    if (err != CEEPEW_OK) {
        printf("FAIL: escape_symbols compress returned %d\n", err);
        return 1;
    }

    err = compress_huffman_decompress(comp, comp_len, decomp, &decomp_len,
                                       (uint16_t)sizeof(decomp));
    if (err != CEEPEW_OK) {
        printf("FAIL: escape_symbols decompress returned %d\n", err);
        return 1;
    }

    if (decomp_len != in_len || memcmp(decomp, input, in_len) != 0) {
        printf("FAIL: escape_symbols data mismatch\n");
        return 1;
    }

    if (stats.passthrough_applied) {
        printf("PASS: escape symbols (passthrough for incompressible data)\n");
        return 0;
    }
    if (stats.escape_sequences == 0U) {
        printf("FAIL: escape_symbols expected escapes or passthrough\n");
        return 1;
    }
    printf("PASS: escape symbols (%u escapes)\n", (unsigned)stats.escape_sequences);
    return 0;
}

static int test_passthrough_trigger(void)
{
    /* Incompressible data should trigger passthrough mode */
    uint8_t input[CEEPEW_HUFFMAN_MAX_INPUT_BYTES];
    for (uint16_t i = 0; i < (uint16_t)sizeof(input); i++) {
        input[i] = (uint8_t)i;
    }
    uint16_t in_len = (uint16_t)sizeof(input);
    uint8_t comp[CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES];
    uint16_t comp_len = 0;
    uint8_t decomp[CEEPEW_HUFFMAN_MAX_INPUT_BYTES];
    uint16_t decomp_len = 0;
    CeePewHuffStats_t stats;

    CeePewErr_t err = compress_huffman_compress(input, in_len, comp, &comp_len,
                                                 (uint16_t)sizeof(comp), &stats);
    if (err != CEEPEW_OK) {
        printf("FAIL: passthrough_trigger compress returned %d\n", err);
        return 1;
    }

    if (!stats.passthrough_applied) {
        printf("FAIL: passthrough_trigger expected passthrough mode\n");
        return 1;
    }

    err = compress_huffman_decompress(comp, comp_len, decomp, &decomp_len,
                                       (uint16_t)sizeof(decomp));
    if (err != CEEPEW_OK) {
        printf("FAIL: passthrough_trigger decompress returned %d\n", err);
        return 1;
    }

    if (decomp_len != in_len || memcmp(decomp, input, in_len) != 0) {
        printf("FAIL: passthrough_trigger data mismatch\n");
        return 1;
    }
    printf("PASS: passthrough trigger (incompressible data)\n");
    return 0;
}

static int test_get_table_entry_bounds(void)
{
    const CeePewHuffEntry_t *e0 = compress_huffman_get_table_entry(0U);
    if (e0 == NULL || e0->code_len == 0U) {
        printf("FAIL: get_table_entry(0) invalid\n");
        return 1;
    }

    const CeePewHuffEntry_t *e50 = compress_huffman_get_table_entry(50U);
    if (e50 == NULL || e50->code_len == 0U) {
        printf("FAIL: get_table_entry(50) invalid\n");
        return 1;
    }

    const CeePewHuffEntry_t *e54 = compress_huffman_get_table_entry(CEEPEW_HUFFMAN_PRIMARY_SYMBOLS);
    if (e54 != NULL) {
        printf("FAIL: get_table_entry(%u) should be out of bounds\n",
               (unsigned)CEEPEW_HUFFMAN_PRIMARY_SYMBOLS);
        return 1;
    }
    printf("PASS: table entry bounds\n");
    return 0;
}

static int test_estimate_accuracy(void)
{
    const uint8_t input[] = "the quick brown fox jumps over the lazy dog";
    uint16_t in_len = (uint16_t)(sizeof(input) - 1U);
    uint8_t comp[CEEPEW_HUFFMAN_MAX_OUTPUT_BYTES];
    uint16_t comp_len = 0;

    uint16_t estimated = compress_huffman_estimate_output_size(input, in_len);
    CeePewErr_t err = compress_huffman_compress(input, in_len, comp, &comp_len,
                                                 (uint16_t)sizeof(comp), NULL);
    if (err != CEEPEW_OK) {
        printf("FAIL: estimate_accuracy compress failed\n");
        return 1;
    }

    if (estimated < comp_len) {
        printf("FAIL: estimate_accuracy estimated %u < actual %u\n",
               (unsigned)estimated, (unsigned)comp_len);
        return 1;
    }
    printf("PASS: estimate accuracy (est=%u, actual=%u)\n",
           (unsigned)estimated, (unsigned)comp_len);
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_roundtrip_english();
    failures += test_empty_input();
    failures += test_single_char();
    failures += test_escape_symbols();
    failures += test_passthrough_trigger();
    failures += test_get_table_entry_bounds();
    failures += test_estimate_accuracy();
    printf("\nStatic Huffman: %d failures\n", failures);
    return failures;
}
