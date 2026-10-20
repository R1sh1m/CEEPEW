/* tests/host/test_ascon.c — Ascon-128 AEAD host unit test */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ceepew_assert.h"
#include "crypto_ascon.h"

/* Test vector from the Ascon reference implementation */
static const uint8_t key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};
static const uint8_t nonce[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
};
static const uint8_t plaintext[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
static const uint8_t ad[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

static int test_ascon_roundtrip(void)
{
    uint8_t ct[64];
    uint16_t ct_len = sizeof(ct);

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        key, nonce, ad, sizeof(ad),
        plaintext, sizeof(plaintext),
        ct, &ct_len);

    if (err != CEEPEW_OK) {
        printf("FAIL: encrypt returned error %d\n", (int)err);
        return 1;
    }

    if (ct_len != sizeof(plaintext) + 16) {
        printf("FAIL: ct_len = %u, expected %zu\n", ct_len, sizeof(plaintext) + 16);
        return 1;
    }

    uint8_t pt2[64];
    uint16_t pt2_len = sizeof(pt2);

    err = crypto_ascon_aead_decrypt(
        key, nonce, ad, sizeof(ad),
        ct, ct_len, pt2, &pt2_len);

    if (err != CEEPEW_OK) {
        printf("FAIL: decrypt returned error %d\n", (int)err);
        return 1;
    }

    if (pt2_len != sizeof(plaintext)) {
        printf("FAIL: pt2_len = %u, expected %zu\n", pt2_len, sizeof(plaintext));
        return 1;
    }

    if (memcmp(pt2, plaintext, sizeof(plaintext)) != 0) {
        printf("FAIL: decrypted plaintext does not match original\n");
        return 1;
    }

    printf("PASS: Ascon-128 round-trip\n");
    return 0;
}

static int test_ascon_wrong_key_fails(void)
{
    uint8_t ct[64];
    uint16_t ct_len = sizeof(ct);

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        key, nonce, ad, sizeof(ad),
        plaintext, sizeof(plaintext),
        ct, &ct_len);

    if (err != CEEPEW_OK) return 1;

    uint8_t wrong_key[16];
    memset(wrong_key, 0xFF, 16);
    uint8_t pt2[64];
    uint16_t pt2_len = sizeof(pt2);

    err = crypto_ascon_aead_decrypt(
        wrong_key, nonce, ad, sizeof(ad),
        ct, ct_len, pt2, &pt2_len);

    if (err == CEEPEW_OK) {
        printf("FAIL: decrypt with wrong key should have failed\n");
        return 1;
    }

    printf("PASS: Ascon-128 wrong key rejected\n");
    return 0;
}

static int test_ascon_tampered_ct_fails(void)
{
    uint8_t ct[64];
    uint16_t ct_len = sizeof(ct);

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        key, nonce, ad, sizeof(ad),
        plaintext, sizeof(plaintext), ct, &ct_len);
    if (err != CEEPEW_OK) return 1;

    ct[0] ^= 0x01; /* flip a bit in ciphertext */

    uint8_t pt2[64];
    uint16_t pt2_len = sizeof(pt2);

    err = crypto_ascon_aead_decrypt(
        key, nonce, ad, sizeof(ad),
        ct, ct_len, pt2, &pt2_len);

    if (err == CEEPEW_OK) {
        printf("FAIL: tampered ciphertext should have failed auth\n");
        return 1;
    }

    printf("PASS: Ascon-128 tampered ciphertext rejected\n");
    return 0;
}

/* Exact-output reference vectors, independently produced with the
 * designers' reference implementation (ascon.iaik.tugraz.at, Ascon-128).
 * Our implementation is checked byte-for-byte against these outputs. */
static const uint8_t ref_key[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};
static const uint8_t ref_nonce[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};
static const uint8_t ref_ad[4] = { 0x00, 0x01, 0x02, 0x03 };
static const uint8_t ref_pt[8] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
static const uint8_t ref_ct_tag[24] = {
    0x77, 0x63, 0xf8, 0xba, 0x6c, 0xe9, 0x1e, 0xd1,
    0xc2, 0xe6, 0xd6, 0xfa, 0x23, 0x09, 0xef, 0x26,
    0xd1, 0xe6, 0xdf, 0x20, 0xfa, 0xeb, 0x33, 0xa6
};

static int test_ascon_reference_vectors(void)
{
    uint8_t ct[64];
    uint16_t ct_len = sizeof(ct);

    CeePewErr_t err = crypto_ascon_aead_encrypt(
        ref_key, ref_nonce, ref_ad, sizeof(ref_ad),
        ref_pt, sizeof(ref_pt), ct, &ct_len);

    if (err != CEEPEW_OK) {
        printf("FAIL: reference-vector encrypt returned error %d\n", (int)err);
        return 1;
    }
    if (ct_len != sizeof(ref_ct_tag)) {
        printf("FAIL: reference-vector ct_len = %u, expected %zu\n",
               ct_len, sizeof(ref_ct_tag));
        return 1;
    }
    if (memcmp(ct, ref_ct_tag, sizeof(ref_ct_tag)) != 0) {
        printf("FAIL: ciphertext+tag does not match reference implementation output\n");
        return 1;
    }

    uint8_t pt2[64];
    uint16_t pt2_len = sizeof(pt2);
    err = crypto_ascon_aead_decrypt(
        ref_key, ref_nonce, ref_ad, sizeof(ref_ad),
        ct, ct_len, pt2, &pt2_len);
    if (err != CEEPEW_OK || pt2_len != sizeof(ref_pt) ||
        memcmp(pt2, ref_pt, sizeof(ref_pt)) != 0) {
        printf("FAIL: reference-vector decrypt round-trip\n");
        return 1;
    }

    printf("PASS: Ascon-128 reference vectors (designers' implementation)\n");
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Header-bound (associated-data) test.
 *
 * Models the CEE-PEW fix: the 24-byte ESL header is passed as Ascon-128 AEAD
 * associated data, so mutating ANY header byte must break tag verification.
 * The header layout mirrors EslHeader_t exactly:
 *   [0]magic0 [1]magic1 [2]version [3]flags [4..7]timestamp_s(LE)
 *   [8..15]seq(LE) [16..23]nonce_counter(LE)
 * This is the exact attack surface from the review: a captured frame whose
 * seq/timestamp is edited (CRC recomputed) must now fail at the AEAD tag.
 * ─────────────────────────────────────────────────────────────────────────── */
#define ESL_HDR_BYTES 24U

static void hdr_put_le32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void hdr_put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v >> (8*i)); }
}

static void hdr_build(uint8_t hdr[ESL_HDR_BYTES], uint64_t seq, uint64_t nonce_counter)
{
    hdr[0] = 0xCEU;               /* magic0 */
    hdr[1] = 0xEEU;               /* magic1 */
    hdr[2] = 0x01U;               /* version */
    hdr[3] = 0x00U;               /* flags */
    hdr_put_le32(&hdr[4], 1234567U);   /* timestamp_s */
    hdr_put_le64(&hdr[8], seq);        /* seq */
    hdr_put_le64(&hdr[16], nonce_counter); /* nonce_counter */
}

static int test_ascon_header_ad_binding(void)
{
    uint8_t hdr[ESL_HDR_BYTES];
    hdr_build(hdr, 42U, 1000U);

    uint8_t pt[58];
    for (int i = 0; i < 58; i++) { pt[i] = (uint8_t)i; }

    uint8_t ct[128];
    uint16_t ct_len = sizeof(ct);
    CeePewErr_t err = crypto_ascon_aead_encrypt(key, nonce, hdr, ESL_HDR_BYTES,
                                                pt, sizeof(pt), ct, &ct_len);
    if (err != CEEPEW_OK) { printf("FAIL: header-AD encrypt err=%d\n", (int)err); return 1; }

    /* Control: identical header must verify and recover the plaintext. */
    uint8_t pt2[128];
    uint16_t pt2_len = sizeof(pt2);
    err = crypto_ascon_aead_decrypt(key, nonce, hdr, ESL_HDR_BYTES, ct, ct_len, pt2, &pt2_len);
    if (err != CEEPEW_OK || pt2_len != sizeof(pt) || memcmp(pt2, pt, sizeof(pt)) != 0) {
        printf("FAIL: header-AD control decrypt (err=%d)\n", (int)err);
        return 1;
    }

    /* Tamper: flip a bit in every header byte, one at a time. Each mutated
     * associated-data vector MUST be rejected (tag mismatch). */
    int rejected = 0;
    for (unsigned i = 0; i < ESL_HDR_BYTES; i++) {
        uint8_t bad[ESL_HDR_BYTES];
        memcpy(bad, hdr, ESL_HDR_BYTES);
        bad[i] ^= 0x01U;
        uint8_t out[128];
        uint16_t out_len = sizeof(out);
        CeePewErr_t e = crypto_ascon_aead_decrypt(key, nonce, bad, ESL_HDR_BYTES,
                                                  ct, ct_len, out, &out_len);
        if (e != CEEPEW_OK) { rejected++; }
    }
    if (rejected != (int)ESL_HDR_BYTES) {
        printf("FAIL: header-AD tamper: %d/%u mutated headers rejected\n",
               rejected, (unsigned)ESL_HDR_BYTES);
        return 1;
    }

    /* Explicit seq/timestamp variants (the reported attack fields). */
    uint8_t bad[ESL_HDR_BYTES];
    memcpy(bad, hdr, ESL_HDR_BYTES); hdr_put_le64(&bad[8], (1ULL << 62)); /* seq jump */
    uint8_t out[128]; uint16_t out_len = sizeof(out);
    if (crypto_ascon_aead_decrypt(key, nonce, bad, ESL_HDR_BYTES, ct, ct_len, out, &out_len) == CEEPEW_OK) {
        printf("FAIL: far-forward seq header accepted\n"); return 1;
    }
    memcpy(bad, hdr, ESL_HDR_BYTES); hdr_put_le32(&bad[4], 9999999U); /* refresh ts */
    out_len = sizeof(out);
    if (crypto_ascon_aead_decrypt(key, nonce, bad, ESL_HDR_BYTES, ct, ct_len, out, &out_len) == CEEPEW_OK) {
        printf("FAIL: refreshed-timestamp header accepted\n"); return 1;
    }

    printf("PASS: Ascon-128 binds 24-byte ESL header as AD (%u/24 header-byte tampers + seq/ts variants rejected)\n",
           (unsigned)ESL_HDR_BYTES);
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_ascon_roundtrip();
    failures += test_ascon_wrong_key_fails();
    failures += test_ascon_tampered_ct_fails();
    failures += test_ascon_reference_vectors();
    failures += test_ascon_header_ad_binding();
    printf("\nAscon: %d failures\n", failures);
    return failures;
}
