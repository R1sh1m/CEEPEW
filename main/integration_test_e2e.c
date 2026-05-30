/* main/integration_test_e2e.c
 *
 * End-to-end integration test suite for CEE-PEW firmware.
 * Tests complete workflows: session FSM, nonce enforcement, compression, encryption, transport.
 *
 * DESIGN: Tests are stateless and can be run sequentially or independently.
 * Each test validates a single complete flow and reports PASS/FAIL.
 * Failures are logged but do not halt execution (to detect multiple broken paths).
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_region.h"
#include "session_fsm.h"
#include "session_msgstore.h"
#include "crypto_hkdf.h"
#include <esp_log.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "CEE-PEW-E2E-TEST";

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Fixtures                                                              */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Fixed test values */
static const uint8_t DEVICE_A_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static const uint8_t DEVICE_B_MAC[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
static const uint8_t SESSION_CODE[32] = {
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,  /* "12345678" */
    0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x30, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46
};

static uint32_t s_tests_passed = 0U;
static uint32_t s_tests_failed = 0U;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Utilities                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */

static void test_assert_eq_u32(uint32_t expected, uint32_t actual, const char *name){
    if (expected == actual) {
        ESP_LOGI(TAG, "[PASS] %s: %lu", name, actual);
        s_tests_passed++;
    } 
    else {
        ESP_LOGE(TAG, "[FAIL] %s: expected %lu, got %lu", name, expected, actual);
        s_tests_failed++;
    }
}

static void test_assert_eq_u64(uint64_t expected, uint64_t actual, const char *name){
    if (expected == actual) {
        ESP_LOGI(TAG, "[PASS] %s: %llu", name, actual);
        s_tests_passed++;
    } 
    else {
        ESP_LOGE(TAG, "[FAIL] %s: expected %llu, got %llu", name, expected, actual);
        s_tests_failed++;
    }
}

static void test_assert_ok(CeePewErr_t err, const char *name){
    if (err == CEEPEW_OK) {
        ESP_LOGI(TAG, "[PASS] %s", name);
        s_tests_passed++;
    } 
    else {
        ESP_LOGE(TAG, "[FAIL] %s: error code %d", name, (int)err);
        s_tests_failed++;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Suite 1: Session FSM Lifecycle                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static void test_session_phase1_discovery(void){
    ESP_LOGI(TAG, "=== Test: Session Phase 1 (Discovery) ===");

    CeePewErr_t err = session_phase1_init(DEVICE_A_MAC);
    test_assert_ok(err, "session_phase1_init");

    uint8_t phase = session_get_phase();
    test_assert_eq_u32(1U, phase, "phase after init");

    bool active = session_is_active();
    if (!active) {
        ESP_LOGI(TAG, "[PASS] session not yet active");
        s_tests_passed++;
    } 
    else {
        ESP_LOGE(TAG, "[FAIL] session should not be active in phase 1");
        s_tests_failed++;
    }

    err = session_phase1_accept_peer(DEVICE_B_MAC);
    test_assert_ok(err, "session_phase1_accept_peer");
}

static void test_session_phase2_pairing(void){
    ESP_LOGI(TAG, "=== Test: Session Phase 2 (Pairing) ===");

    /* First initialize phase 1 */
    CeePewErr_t err = session_phase1_init(DEVICE_A_MAC);
    test_assert_ok(err, "session_phase1_init");

    err = session_phase1_accept_peer(DEVICE_B_MAC);
    test_assert_ok(err, "session_phase1_accept_peer");

    /* Move to phase 2 */
    err = session_phase2_initiate(SESSION_CODE);
    test_assert_ok(err, "session_phase2_initiate");

    uint8_t phase = session_get_phase();
    test_assert_eq_u32(2U, phase, "phase after initiate");

    /* Derive key */
    err = session_phase2_derive_key();
    test_assert_ok(err, "session_phase2_derive_key");

    phase = session_get_phase();
    test_assert_eq_u32(3U, phase, "phase after key derivation");

    bool active = session_is_active();
    if (active) {
        ESP_LOGI(TAG, "[PASS] session now active");
        s_tests_passed++;
    } 
    else {
        ESP_LOGE(TAG, "[FAIL] session should be active in phase 3");
        s_tests_failed++;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Suite 2: Nonce Enforcement                                           */
/* ─────────────────────────────────────────────────────────────────────────── */

static void test_nonce_enforcement(void){
    ESP_LOGI(TAG, "=== Test: Nonce Enforcement ===");

    /* Initialize and activate session */
    CeePewErr_t err = session_phase1_init(DEVICE_A_MAC);
    test_assert_ok(err, "session_phase1_init");

    err = session_phase1_accept_peer(DEVICE_B_MAC);
    test_assert_ok(err, "session_phase1_accept_peer");

    err = session_phase2_initiate(SESSION_CODE);
    test_assert_ok(err, "session_phase2_initiate");

    err = session_phase2_derive_key();
    test_assert_ok(err, "session_phase2_derive_key");

    /* Verify nonce starts at 1 */
    uint64_t nonce_0 = session_get_nonce_counter();
    test_assert_eq_u64(0ULL, nonce_0, "initial nonce counter");

    /* Call enforce_nonce_limit multiple times */
    for (uint32_t i = 0U; i < 10U; i++) {
        err = session_enforce_nonce_limit();
        if (err == CEEPEW_OK) {
            uint64_t nonce_after = session_get_nonce_counter();
            test_assert_eq_u64((uint64_t)(i + 1U), nonce_after, "nonce counter increment");
        } 
        else {
            ESP_LOGE(TAG, "[FAIL] enforce_nonce_limit failed at iteration %lu", i);
            s_tests_failed++;
            break;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Suite 3: MAC Lock Verification                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

static void test_mac_lock_check(void){
    ESP_LOGI(TAG, "=== Test: MAC Lock Verification ===");

    /* Initialize and activate session with DEVICE_B as peer */
    CeePewErr_t err = session_phase1_init(DEVICE_A_MAC);
    test_assert_ok(err, "session_phase1_init");

    err = session_phase1_accept_peer(DEVICE_B_MAC);
    test_assert_ok(err, "session_phase1_accept_peer");

    err = session_phase2_initiate(SESSION_CODE);
    test_assert_ok(err, "session_phase2_initiate");

    err = session_phase2_derive_key();
    test_assert_ok(err, "session_phase2_derive_key");

    /* MAC lock check with correct peer should pass */
    err = session_mac_lock_check(DEVICE_B_MAC);
    test_assert_ok(err, "mac_lock_check with correct peer");

    /* MAC lock check with wrong peer should fail */
    uint8_t wrong_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    err = session_mac_lock_check(wrong_mac);
    if (err != CEEPEW_OK) {
        ESP_LOGI(TAG, "[PASS] mac_lock_check correctly rejected wrong peer");
        s_tests_passed++;
    } 
    else {
        ESP_LOGE(TAG, "[FAIL] mac_lock_check should reject wrong peer");
        s_tests_failed++;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Suite 4: Session Termination & Cleanup                               */
/* ─────────────────────────────────────────────────────────────────────────── */

static void test_session_termination(void){
    ESP_LOGI(TAG, "=== Test: Session Termination ===");

    /* Initialize and activate session */
    CeePewErr_t err = session_phase1_init(DEVICE_A_MAC);
    test_assert_ok(err, "session_phase1_init");

    err = session_phase1_accept_peer(DEVICE_B_MAC);
    test_assert_ok(err, "session_phase1_accept_peer");

    err = session_phase2_initiate(SESSION_CODE);
    test_assert_ok(err, "session_phase2_initiate");

    err = session_phase2_derive_key();
    test_assert_ok(err, "session_phase2_derive_key");

    bool active_before = session_is_active();
    if (active_before) {
        ESP_LOGI(TAG, "[PASS] session active before termination");
        s_tests_passed++;
    }
    else {
        ESP_LOGE(TAG, "[FAIL] session should be active before termination");
        s_tests_failed++;
    }

    /* Terminate session */
    err = session_end();
    test_assert_ok(err, "session_end");

    /* Verify session is no longer active */
    uint8_t phase_after = session_get_phase();
    test_assert_eq_u32(0U, phase_after, "phase after termination");

    bool active_after = session_is_active();
    if (!active_after) {
        ESP_LOGI(TAG, "[PASS] session inactive after termination");
        s_tests_passed++;
    }
    else {
        ESP_LOGE(TAG, "[FAIL] session should be inactive after termination");
        s_tests_failed++;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Test Master Function                                                       */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Small region alloc/reset test */
static void test_region_alloc_reset(void){
    ESP_LOGI(TAG, "=== Test: Region alloc after reset ===");

    Region_t r;
    CeePewErr_t err = region_init(&r);
    test_assert_ok(err, "region_init");

    void *p = region_alloc(&r, 100U);
    if (p != NULL) {
        ESP_LOGI(TAG, "[PASS] region_alloc(100) first");
        s_tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] region_alloc(100) first");
        s_tests_failed++;
    }

    region_reset(&r);
    p = region_alloc(&r, 100U);
    if (p != NULL) {
        ESP_LOGI(TAG, "[PASS] region_alloc(100) after reset");
        s_tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] region_alloc(100) after reset");
        s_tests_failed++;
    }
}

/* external pipeline test (defined in test_pipeline_memory.c) */
void test_pipeline_memory(void);

/* replay test (defined in test_replay.c) */
void test_replay_window(void);

/* comprehensive replay test (defined in test_replay_comprehensive.c) */
void test_replay_window_comprehensive(void);

/* power test (defined in test_power.c) */
void test_power(void);

/* session message store test (defined below) */
static void test_session_message_store(void){
    ESP_LOGI(TAG, "=== Test: Session Message Store ===");

    CeePewErr_t err = msg_store_init();
    test_assert_ok(err, "msg_store_init");

    uint8_t enc[32];
    for (uint8_t i = 0U; i < sizeof(enc); i++) { enc[i] = (uint8_t)(i + 1U); }

    err = msg_store_add(enc, (uint16_t)sizeof(enc), 16U, 0U);
    test_assert_ok(err, "msg_store_add");

    uint8_t count = msg_store_count();
    test_assert_eq_u32(1U, (uint32_t)count, "msg_store_count after add");

    const StoredMsg_t *m = msg_store_get(0U);
    if (m != NULL) {
        if (memcmp(m->encrypted, enc, sizeof(enc)) == 0) {
            ESP_LOGI(TAG, "[PASS] stored encrypted matches input");
            s_tests_passed++;
        } else {
            ESP_LOGE(TAG, "[FAIL] stored encrypted mismatch");
            s_tests_failed++;
        }
    } else {
        ESP_LOGE(TAG, "[FAIL] msg_store_get returned NULL");
        s_tests_failed++;
    }

    err = msg_store_wipe_all();
    test_assert_ok(err, "msg_store_wipe_all");

    count = msg_store_count();
    test_assert_eq_u32(0U, (uint32_t)count, "msg_store_count after wipe");
}

/* crypto HKDF smoke test (RFC5869 test vector 1 - SHA256) */
static void test_crypto_hkdf_smoke(void){
    ESP_LOGI(TAG, "=== Test: Crypto HKDF smoke (RFC5869 vector 1) ===");

    uint8_t ikm[22];
    for (uint8_t i = 0U; i < sizeof(ikm); i++) { ikm[i] = 0x0b; }
    uint8_t salt[13] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c};
    uint8_t info[10] = {0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9};

    uint8_t out[42];
    CeePewErr_t err = crypto_hkdf_derive(ikm, (uint8_t)sizeof(ikm), salt, (uint8_t)sizeof(salt), info, (uint8_t)sizeof(info), out, (uint8_t)sizeof(out));
    test_assert_ok(err, "crypto_hkdf_derive");

    /* Expected first 8 bytes from RFC5869 test vector 1 */
    uint8_t expected_prefix[8] = {0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a};
    if (memcmp(out, expected_prefix, sizeof(expected_prefix)) == 0) {
        ESP_LOGI(TAG, "[PASS] hkdf output prefix matches RFC5869 vector 1");
        s_tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] hkdf output prefix mismatch");
        s_tests_failed++;
    }
}

/* transport hop deterministic test - call unit test helper */
void test_hop_determinism(void);

void integration_tests_run_all(void){
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  CEE-PEW End-to-End Integration Test Suite                 ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");

    /* Session FSM tests */
    test_session_phase1_discovery();
    test_session_phase2_pairing();

    /* Nonce enforcement tests */
    test_nonce_enforcement();

    /* Security tests */
    test_mac_lock_check();

    /* Lifecycle tests */
    test_session_termination();

    /* Region alloc/reset test */
    test_region_alloc_reset();

    /* Pipeline memory test */
    test_pipeline_memory();

    /* Replay window tests */
    test_replay_window();
    
    /* Comprehensive replay window tests (edge cases) */
    test_replay_window_comprehensive();

    /* Session message store tests */
    test_session_message_store();

    /* Crypto HKDF smoke test */
    test_crypto_hkdf_smoke();

    /* Transport hop permutation deterministic test */
    test_hop_determinism();

    /* Power/wakeup tests */
    test_power();

    /* Summary */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Test Results                                              ║");
    ESP_LOGI(TAG, "║  PASSED: %lu                                               ║", s_tests_passed);
    ESP_LOGI(TAG, "║  FAILED: %lu                                               ║", s_tests_failed);
    ESP_LOGI(TAG, "║  TOTAL:  %lu                                               ║", s_tests_passed + s_tests_failed);
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");

    if (s_tests_failed == 0U) {
        ESP_LOGI(TAG, "✓ All tests PASSED");
    } 
    else {
        ESP_LOGE(TAG, "✗ %lu test(s) FAILED", s_tests_failed);
    }
}
