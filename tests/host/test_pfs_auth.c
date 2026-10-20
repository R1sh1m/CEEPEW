/* tests/host/test_pfs_auth.c
 *
 * Host test for PFS handshake authentication.
 * Exercises crypto_hkdf_derive + crypto_hmac_sha256_trunc with the convention
 * header(24)||type(1)||pubkey(32).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include "crypto_hkdf.h"
#include "crypto_hmac.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %-40s ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; return 0; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return 1; } while(0)

/* Test (a): Symmetry - two "peers" derive identical k_pfs_auth from same box_seed/session_id */
static int test_symmetry(void)
{
    TEST("Symmetry: identical keys from same seed/session_id");
    
    uint8_t box_seed[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    uint8_t session_id[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    
    uint8_t key_peer1[32], key_peer2[32];
    static const uint8_t info[] = "CEEPEW_PFS_AUTH_v1";
    
    CeePewErr_t err1 = crypto_hkdf_derive(box_seed, 32, session_id, 8,
                                          info, sizeof(info) - 1, key_peer1, 32);
    CeePewErr_t err2 = crypto_hkdf_derive(box_seed, 32, session_id, 8,
                                          info, sizeof(info) - 1, key_peer2, 32);
    
    if (err1 != CEEPEW_OK || err2 != CEEPEW_OK) {
        FAIL("HKDF derivation failed");
    }
    
    if (!ceepew_ct_equal(key_peer1, key_peer2, 32)) {
        FAIL("Keys differ between peers");
    }
    
    ceepew_secure_zero(key_peer1, sizeof(key_peer1));
    ceepew_secure_zero(key_peer2, sizeof(key_peer2));
    ceepew_secure_zero(box_seed, sizeof(box_seed));
    
    PASS();
}

/* Test (b): Round-trip - build auth_data, compute tag, recompute and compare */
static int test_roundtrip(void)
{
    TEST("Round-trip: tag recomputation matches");
    
    uint8_t box_seed[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    uint8_t session_id[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t auth_key[32];
    static const uint8_t info[] = "CEEPEW_PFS_AUTH_v1";
    
    CeePewErr_t err = crypto_hkdf_derive(box_seed, 32, session_id, 8,
                                         info, sizeof(info) - 1, auth_key, 32);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        FAIL("HKDF derivation failed");
    }
    
    /* Build auth_data = header(24) || type(1) || pubkey(32) = 57 bytes */
    uint8_t header[24] = {
        0x45, 0x53,  /* ESL magic */
        0x01,        /* version */
        0x00,        /* flags */
        0x00, 0x00, 0x00, 0x00,  /* timestamp */
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* seq */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00   /* nonce_counter */
    };
    uint8_t msg_type = 0x01;  /* PFS_INIT */
    uint8_t pubkey[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    
    uint8_t auth_data[24 + 1 + 32];
    memcpy(auth_data, header, 24);
    auth_data[24] = msg_type;
    memcpy(auth_data + 25, pubkey, 32);
    
    uint8_t tag1[16], tag2[16];
    err = crypto_hmac_sha256_trunc(auth_key, 32, auth_data, sizeof(auth_data), tag1, 16);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        FAIL("HMAC truncation failed");
    }
    
    err = crypto_hmac_sha256_trunc(auth_key, 32, auth_data, sizeof(auth_data), tag2, 16);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag1, sizeof(tag1));
        FAIL("HMAC truncation failed (2nd call)");
    }
    
    if (!ceepew_ct_equal(tag1, tag2, 16)) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag1, sizeof(tag1));
        ceepew_secure_zero(tag2, sizeof(tag2));
        FAIL("Tags differ on recomputation");
    }
    
    ceepew_secure_zero(box_seed, sizeof(box_seed));
    ceepew_secure_zero(auth_key, sizeof(auth_key));
    ceepew_secure_zero(tag1, sizeof(tag1));
    ceepew_secure_zero(tag2, sizeof(tag2));
    
    PASS();
}

/* Test (c): Tamper-reject - flip bits in pubkey, type, and header */
static int test_tamper_reject(void)
{
    TEST("Tamper-reject: bit flips detected");
    
    uint8_t box_seed[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    uint8_t session_id[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t auth_key[32];
    static const uint8_t info[] = "CEEPEW_PFS_AUTH_v1";
    
    CeePewErr_t err = crypto_hkdf_derive(box_seed, 32, session_id, 8,
                                         info, sizeof(info) - 1, auth_key, 32);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        FAIL("HKDF derivation failed");
    }
    
    uint8_t header[24] = {
        0x45, 0x53, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t msg_type = 0x01;
    uint8_t pubkey[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    
    uint8_t auth_data[24 + 1 + 32];
    memcpy(auth_data, header, 24);
    auth_data[24] = msg_type;
    memcpy(auth_data + 25, pubkey, 32);
    
    uint8_t tag_orig[16];
    err = crypto_hmac_sha256_trunc(auth_key, 32, auth_data, sizeof(auth_data), tag_orig, 16);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag_orig, sizeof(tag_orig));
        FAIL("HMAC truncation failed");
    }
    
    /* Test 1: Flip bit in pubkey */
    auth_data[25 + 5] ^= 0x01;
    uint8_t tag_tampered1[16];
    err = crypto_hmac_sha256_trunc(auth_key, 32, auth_data, sizeof(auth_data), tag_tampered1, 16);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag_orig, sizeof(tag_orig));
        ceepew_secure_zero(tag_tampered1, sizeof(tag_tampered1));
        FAIL("HMAC truncation failed (tampered1)");
    }
    if (ceepew_ct_equal(tag_orig, tag_tampered1, 16)) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag_orig, sizeof(tag_orig));
        ceepew_secure_zero(tag_tampered1, sizeof(tag_tampered1));
        FAIL("Pubkey tamper not detected");
    }
    ceepew_secure_zero(tag_tampered1, sizeof(tag_tampered1));
    auth_data[25 + 5] ^= 0x01;  /* Restore */
    
    /* Test 2: Flip bit in type byte */
    auth_data[24] ^= 0x01;
    uint8_t tag_tampered2[16];
    err = crypto_hmac_sha256_trunc(auth_key, 32, auth_data, sizeof(auth_data), tag_tampered2, 16);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag_orig, sizeof(tag_orig));
        ceepew_secure_zero(tag_tampered2, sizeof(tag_tampered2));
        FAIL("HMAC truncation failed (tampered2)");
    }
    if (ceepew_ct_equal(tag_orig, tag_tampered2, 16)) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag_orig, sizeof(tag_orig));
        ceepew_secure_zero(tag_tampered2, sizeof(tag_tampered2));
        FAIL("Type byte tamper not detected");
    }
    ceepew_secure_zero(tag_tampered2, sizeof(tag_tampered2));
    auth_data[24] ^= 0x01;  /* Restore */
    
    /* Test 3: Flip bit in header */
    auth_data[0] ^= 0x01;
    uint8_t tag_tampered3[16];
    err = crypto_hmac_sha256_trunc(auth_key, 32, auth_data, sizeof(auth_data), tag_tampered3, 16);
    if (err != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag_orig, sizeof(tag_orig));
        ceepew_secure_zero(tag_tampered3, sizeof(tag_tampered3));
        FAIL("HMAC truncation failed (tampered3)");
    }
    if (ceepew_ct_equal(tag_orig, tag_tampered3, 16)) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key, sizeof(auth_key));
        ceepew_secure_zero(tag_orig, sizeof(tag_orig));
        ceepew_secure_zero(tag_tampered3, sizeof(tag_tampered3));
        FAIL("Header tamper not detected");
    }
    ceepew_secure_zero(tag_tampered3, sizeof(tag_tampered3));
    
    ceepew_secure_zero(box_seed, sizeof(box_seed));
    ceepew_secure_zero(auth_key, sizeof(auth_key));
    ceepew_secure_zero(tag_orig, sizeof(tag_orig));
    
    PASS();
}

/* Test (d): Domain separation - different session_id or box_seed yields different tag */
static int test_domain_separation(void)
{
    TEST("Domain separation: different session_id/box_seed");
    
    uint8_t box_seed[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    uint8_t session_id1[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t session_id2[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x99};  /* Different */
    
    uint8_t auth_key1[32], auth_key2[32];
    static const uint8_t info[] = "CEEPEW_PFS_AUTH_v1";
    
    CeePewErr_t err1 = crypto_hkdf_derive(box_seed, 32, session_id1, 8,
                                          info, sizeof(info) - 1, auth_key1, 32);
    CeePewErr_t err2 = crypto_hkdf_derive(box_seed, 32, session_id2, 8,
                                          info, sizeof(info) - 1, auth_key2, 32);
    
    if (err1 != CEEPEW_OK || err2 != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key1, sizeof(auth_key1));
        ceepew_secure_zero(auth_key2, sizeof(auth_key2));
        FAIL("HKDF derivation failed");
    }
    
    if (ceepew_ct_equal(auth_key1, auth_key2, 32)) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key1, sizeof(auth_key1));
        ceepew_secure_zero(auth_key2, sizeof(auth_key2));
        FAIL("Keys identical with different session_id");
    }
    
    /* Build auth_data */
    uint8_t header[24] = {
        0x45, 0x53, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t msg_type = 0x01;
    uint8_t pubkey[32] = {0};
    
    uint8_t auth_data[24 + 1 + 32];
    memcpy(auth_data, header, 24);
    auth_data[24] = msg_type;
    memcpy(auth_data + 25, pubkey, 32);
    
    uint8_t tag1[16], tag2[16];
    err1 = crypto_hmac_sha256_trunc(auth_key1, 32, auth_data, sizeof(auth_data), tag1, 16);
    err2 = crypto_hmac_sha256_trunc(auth_key2, 32, auth_data, sizeof(auth_data), tag2, 16);
    
    if (err1 != CEEPEW_OK || err2 != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key1, sizeof(auth_key1));
        ceepew_secure_zero(auth_key2, sizeof(auth_key2));
        ceepew_secure_zero(tag1, sizeof(tag1));
        ceepew_secure_zero(tag2, sizeof(tag2));
        FAIL("HMAC truncation failed");
    }
    
    if (ceepew_ct_equal(tag1, tag2, 16)) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(auth_key1, sizeof(auth_key1));
        ceepew_secure_zero(auth_key2, sizeof(auth_key2));
        ceepew_secure_zero(tag1, sizeof(tag1));
        ceepew_secure_zero(tag2, sizeof(tag2));
        FAIL("Tags identical with different session_id");
    }
    
    /* Test with different box_seed */
    uint8_t box_seed2[32];
    memcpy(box_seed2, box_seed, 32);
    box_seed2[0] ^= 0x01;  /* Flip one bit */
    
    uint8_t auth_key3[32];
    CeePewErr_t err3 = crypto_hkdf_derive(box_seed2, 32, session_id1, 8,
                                          info, sizeof(info) - 1, auth_key3, 32);
    if (err3 != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(box_seed2, sizeof(box_seed2));
        ceepew_secure_zero(auth_key1, sizeof(auth_key1));
        ceepew_secure_zero(auth_key3, sizeof(auth_key3));
        ceepew_secure_zero(tag1, sizeof(tag1));
        FAIL("HKDF derivation failed (box_seed2)");
    }
    
    uint8_t tag3[16];
    err3 = crypto_hmac_sha256_trunc(auth_key3, 32, auth_data, sizeof(auth_data), tag3, 16);
    if (err3 != CEEPEW_OK) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(box_seed2, sizeof(box_seed2));
        ceepew_secure_zero(auth_key1, sizeof(auth_key1));
        ceepew_secure_zero(auth_key3, sizeof(auth_key3));
        ceepew_secure_zero(tag1, sizeof(tag1));
        ceepew_secure_zero(tag3, sizeof(tag3));
        FAIL("HMAC truncation failed (tag3)");
    }
    
    if (ceepew_ct_equal(tag1, tag3, 16)) {
        ceepew_secure_zero(box_seed, sizeof(box_seed));
        ceepew_secure_zero(box_seed2, sizeof(box_seed2));
        ceepew_secure_zero(auth_key1, sizeof(auth_key1));
        ceepew_secure_zero(auth_key3, sizeof(auth_key3));
        ceepew_secure_zero(tag1, sizeof(tag1));
        ceepew_secure_zero(tag3, sizeof(tag3));
        FAIL("Tags identical with different box_seed");
    }
    
    ceepew_secure_zero(box_seed, sizeof(box_seed));
    ceepew_secure_zero(box_seed2, sizeof(box_seed2));
    ceepew_secure_zero(auth_key1, sizeof(auth_key1));
    ceepew_secure_zero(auth_key2, sizeof(auth_key2));
    ceepew_secure_zero(auth_key3, sizeof(auth_key3));
    ceepew_secure_zero(tag1, sizeof(tag1));
    ceepew_secure_zero(tag2, sizeof(tag2));
    ceepew_secure_zero(tag3, sizeof(tag3));
    
    PASS();
}

int main(void)
{
    printf("PFS Authentication Test Suite\n");
    printf("==============================\n\n");
    
    int result;
    
    result = test_symmetry();
    if (result != 0) { printf("    FAILED: test_symmetry\n\n"); }
    
    result = test_roundtrip();
    if (result != 0) { printf("    FAILED: test_roundtrip\n\n"); }
    
    result = test_tamper_reject();
    if (result != 0) { printf("    FAILED: test_tamper_reject\n\n"); }
    
    result = test_domain_separation();
    if (result != 0) { printf("    FAILED: test_domain_separation\n\n"); }
    
    printf("\n==============================\n");
    printf("Tests: %d/%d passed\n", tests_passed, tests_run);
    
    return (tests_passed == tests_run) ? 0 : 1;
}
