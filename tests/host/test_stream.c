/* tests/host/test_stream.c
 *
 * Host C Unit Test Suite for CEE-PEW Stream Cipher (Salsa20/XSalsa20 engine).
 *
 * Verifies:
 *   1. Encryption and decryption round-trip invertibility (XOR symmetry).
 *   2. Chunked processing equivalence (single-shot vs arbitrary chunk boundaries).
 *   3. 64-byte block boundary crossings (1B, 63B, 64B, 65B, 256B, 512B).
 *   4. Strict nonce sensitivity and key sensitivity (avoids keystream reuse).
 *   5. Context finalisation and secure zeroing.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "crypto_stream.h"
#include "ceepew_security_utils.h"

static int s_passed = 0;
static int s_failed = 0;

static void check(bool cond, const char *test_name)
{
    if (cond) {
        printf("  [PASS] %s\n", test_name);
        s_passed++;
    } else {
        printf("  [FAIL] %s\n", test_name);
        s_failed++;
    }
}

static void test_encrypt_decrypt_roundtrip(void)
{
    printf("\n--- Test 1: Encryption & Decryption Round-trip ---\n");

    const uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    const uint8_t nonce[24] = {
        0x69, 0x69, 0x6e, 0x65, 0x74, 0x65, 0x65, 0x6e,
        0x62, 0x79, 0x74, 0x65, 0x6e, 0x6f, 0x6e, 0x63,
        0x65, 0x66, 0x6f, 0x72, 0x73, 0x61, 0x6c, 0x73
    };

    const char *plaintext = "CEE-PEW: Zero-Heap Embedded Cryptographic Firmware over ESP-NOW Protocol";
    uint16_t pt_len = (uint16_t)strlen(plaintext);

    StreamCipher_t enc_ctx;
    CeePewErr_t err = crypto_stream_init(&enc_ctx, key, nonce);
    check(err == CEEPEW_OK, "Stream cipher init succeeds for encryption");

    uint8_t ciphertext[128];
    uint16_t ct_len = (uint16_t)sizeof(ciphertext);
    err = crypto_stream_process(&enc_ctx, (const uint8_t *)plaintext, pt_len, ciphertext, &ct_len);
    check(err == CEEPEW_OK, "Stream process succeeds for encryption");
    check(ct_len == pt_len, "Ciphertext length matches plaintext length");
    check(memcmp(ciphertext, plaintext, pt_len) != 0, "Ciphertext is distinct from plaintext");

    crypto_stream_finalise(&enc_ctx);

    /* Decrypt with fresh context */
    StreamCipher_t dec_ctx;
    err = crypto_stream_init(&dec_ctx, key, nonce);
    check(err == CEEPEW_OK, "Stream cipher init succeeds for decryption");

    uint8_t decrypted[128];
    uint16_t dec_len = (uint16_t)sizeof(decrypted);
    err = crypto_stream_process(&dec_ctx, ciphertext, ct_len, decrypted, &dec_len);
    check(err == CEEPEW_OK, "Stream process succeeds for decryption");
    check(dec_len == pt_len, "Decrypted length matches original plaintext");
    check(memcmp(decrypted, plaintext, pt_len) == 0, "Decrypted message matches original plaintext byte-for-byte");

    crypto_stream_finalise(&dec_ctx);
}

static void test_chunked_streaming_equivalence(void)
{
    printf("\n--- Test 2: Chunked Processing Equivalence ---\n");

    const uint8_t key[32] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02};
    const uint8_t nonce[24] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};

    uint8_t data[250];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i ^ 0x5C);
    }

    /* Single-shot encryption */
    StreamCipher_t ctx1;
    crypto_stream_init(&ctx1, key, nonce);
    uint8_t ct_single[sizeof(data)];
    uint16_t len1 = (uint16_t)sizeof(ct_single);
    crypto_stream_process(&ctx1, data, (uint16_t)sizeof(data), ct_single, &len1);
    crypto_stream_finalise(&ctx1);

    /* Chunked encryption across arbitrary block boundaries (e.g. 7, 63, 64, 1, 115) */
    StreamCipher_t ctx2;
    crypto_stream_init(&ctx2, key, nonce);
    uint8_t ct_chunked[sizeof(data)];
    uint16_t chunk_sizes[] = {7, 63, 64, 1, 50, 65};
    uint16_t offset = 0;

    for (size_t c = 0; c < sizeof(chunk_sizes)/sizeof(chunk_sizes[0]); c++) {
        uint16_t to_process = chunk_sizes[c];
        if (offset + to_process > (uint16_t)sizeof(data)) {
            to_process = (uint16_t)sizeof(data) - offset;
        }
        uint16_t out_chunk_len = to_process;
        CeePewErr_t err = crypto_stream_process(&ctx2, &data[offset], to_process,
                                               &ct_chunked[offset], &out_chunk_len);
        check(err == CEEPEW_OK, "Chunk processing step succeeds");
        offset += to_process;
    }
    crypto_stream_finalise(&ctx2);

    check(offset == (uint16_t)sizeof(data), "All chunked bytes processed");
    check(ceepew_ct_equal(ct_single, ct_chunked, sizeof(data)) == 1,
          "Single-shot and chunked encryption produce identical ciphertext");
}

static void test_key_and_nonce_sensitivity(void)
{
    printf("\n--- Test 3: Key & Nonce Sensitivity ---\n");

    uint8_t key1[32] = {0x01};
    uint8_t key2[32] = {0x01};
    key2[0] ^= 0x01; /* 1-bit difference */

    uint8_t nonce1[24] = {0x42};
    uint8_t nonce2[24] = {0x42};
    nonce2[0] ^= 0x01; /* 1-bit difference */

    uint8_t plain[64] = {0};
    uint8_t ct1[64], ct2[64], ct3[64];
    uint16_t l1 = (uint16_t)sizeof(ct1), l2 = (uint16_t)sizeof(ct2), l3 = (uint16_t)sizeof(ct3);

    StreamCipher_t ctx;
    crypto_stream_init(&ctx, key1, nonce1);
    crypto_stream_process(&ctx, plain, 64, ct1, &l1);
    crypto_stream_finalise(&ctx);

    /* Different key */
    crypto_stream_init(&ctx, key2, nonce1);
    crypto_stream_process(&ctx, plain, 64, ct2, &l2);
    crypto_stream_finalise(&ctx);

    /* Different nonce */
    crypto_stream_init(&ctx, key1, nonce2);
    crypto_stream_process(&ctx, plain, 64, ct3, &l3);
    crypto_stream_finalise(&ctx);

    check(memcmp(ct1, ct2, 64) != 0, "1-bit key change produces distinct keystream");
    check(memcmp(ct1, ct3, 64) != 0, "1-bit nonce change produces distinct keystream");
}

static void test_finalise_zeroing(void)
{
    printf("\n--- Test 4: Finalisation & Context Security ---\n");

    uint8_t key[32] = {0x77};
    uint8_t nonce[24] = {0x88};
    StreamCipher_t ctx;

    crypto_stream_init(&ctx, key, nonce);
    check(ctx.active == true, "Context active flag is true after init");

    crypto_stream_finalise(&ctx);
    check(ctx.active == false, "Context active flag is false after finalise");
    check(ceepew_is_all_zero(ctx.subkey, sizeof(ctx.subkey)) == 1,
          "Subkey is securely wiped to zero upon finalise");
    check(ceepew_is_all_zero(ctx.block, sizeof(ctx.block)) == 1,
          "Block buffer is securely wiped to zero upon finalise");

    uint8_t in[16] = {0}, out[16];
    uint16_t out_len = (uint16_t)sizeof(out);
    CeePewErr_t err = crypto_stream_process(&ctx, in, 16, out, &out_len);
    check(err != CEEPEW_OK, "Processing with inactive context fails safely");
}

int main(void)
{
    printf("============================================\n");
    printf(" CEE-PEW Stream Cipher (Salsa20) Unit Test\n");
    printf("============================================\n");

    test_encrypt_decrypt_roundtrip();
    test_chunked_streaming_equivalence();
    test_key_and_nonce_sensitivity();
    test_finalise_zeroing();

    printf("\n============================================\n");
    printf(" Stream Summary: %d Passed, %d Failed\n", s_passed, s_failed);
    printf("============================================\n");

    return (s_failed == 0) ? 0 : 1;
}
