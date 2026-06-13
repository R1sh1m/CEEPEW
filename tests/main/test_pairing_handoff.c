/* main/test_pairing_handoff.c
 *
 * Regression test for the beacon-based commitment handshake.
 *
 * The pairing handshake is now connectionless: each device broadcasts a
 * 16-byte truncated SHA-256 commitment in SCAN_RSP (manufacturer AD
 * 0xCEEE/0x50).  The first device to receive a peer beacon calls
 * transport_ble_verify_pending_commitment() which:
 *
 *   - defers if the local commitment is not yet ready
 *   - returns CEEPEW_OK on match and sets
 *       g_ble_ctx.commitment_verified = true
 *       g_ble_ctx.handoff_ready       = true
 *       g_ble_ctx.ready_for_chat      = true
 *       g_ble_ctx.state               = BLE_DONE
 *   - returns CEEPEW_ERR_AUTH_FAIL on mismatch
 *
 * This test exercises the four cases by directly manipulating g_ble_ctx
 * and asserting the public API.  No second physical device is required.
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "transport_ble.h"
#include "session_fsm.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "TEST-PAIRING-HANDOFF";

static bool handoff_check(bool ok, const char *label)
{
    if (ok) {
        ESP_LOGI(TAG, "[PASS] %s", label);
        return true;
    }
    ESP_LOGE(TAG, "[FAIL] %s", label);
    return false;
}

/*
 * Test 1: A pending peer commitment with no local commitment loaded must
 * DEFER — the call must return CEEPEW_OK and leave handoff_ready false.
 */
static bool handoff_test_defers_without_local_commitment(void)
{
    bool ok = true;

    g_ble_ctx.commitment_verified        = false;
    g_ble_ctx.handoff_ready              = false;
    g_ble_ctx.ready_for_chat             = false;
    g_ble_ctx.state                      = BLE_PAIRING;
    g_ble_ctx.local_commitment_len       = 0U;
    memset(g_ble_ctx.commitment_digest, 0U, sizeof(g_ble_ctx.commitment_digest));
    g_ble_ctx.peer_commitment_pending    = true;
    g_ble_ctx.pending_peer_commitment_len = CEEPEW_COMMITMENT_ADV_BYTES;
    memset(g_ble_ctx.pending_peer_commitment, 0xAAU, CEEPEW_COMMITMENT_ADV_BYTES);

    CeePewErr_t err = transport_ble_verify_pending_commitment();
    ok &= handoff_check(err == CEEPEW_OK, "deferred verification returns OK");
    ok &= handoff_check(g_ble_ctx.peer_commitment_pending,
                        "peer_commitment_pending stays true while deferred");
    ok &= handoff_check(!g_ble_ctx.commitment_verified,
                        "commitment_verified stays false while deferred");
    ok &= handoff_check(!g_ble_ctx.handoff_ready,
                        "handoff_ready stays false while deferred");
    ok &= handoff_check(!transport_ble_handoff_ready(),
                        "transport_ble_handoff_ready() returns false while deferred");

    return ok;
}

/*
 * Test 2: Matching beacon commitments must PROMOTE the device to
 * BLE_DONE / handoff_ready / commitment_verified.
 */
static bool handoff_test_matching_beacon_promotes(void)
{
    bool ok = true;

    uint8_t shared_commitment[CEEPEW_COMMITMENT_ADV_BYTES];
    for (uint8_t i = 0U; i < CEEPEW_COMMITMENT_ADV_BYTES; i++) {
        shared_commitment[i] = (uint8_t)(0x10U + i);
    }

    /* Build a full CEEPEW_COMMITMENT_BYTES commitment whose first
     * CEEPEW_COMMITMENT_ADV_BYTES match the beacon so that
     * session_verify_peer_commitment_with_sig() succeeds in the ADV path. */
    uint8_t full_commitment[CEEPEW_COMMITMENT_BYTES];
    memcpy(full_commitment, shared_commitment, CEEPEW_COMMITMENT_ADV_BYTES);
    memset(full_commitment + CEEPEW_COMMITMENT_ADV_BYTES, 0x42U,
           CEEPEW_COMMITMENT_BYTES - CEEPEW_COMMITMENT_ADV_BYTES);
    session_test_set_commitment(full_commitment);

    g_ble_ctx.commitment_verified          = false;
    g_ble_ctx.handoff_ready                = false;
    g_ble_ctx.ready_for_chat               = false;
    g_ble_ctx.state                        = BLE_ADVERTISING_AND_SCANNING;
    g_ble_ctx.local_commitment_len         = CEEPEW_COMMITMENT_BYTES;
    memcpy(g_ble_ctx.commitment_digest, full_commitment,
           CEEPEW_COMMITMENT_BYTES);
    g_ble_ctx.peer_commitment_pending      = true;
    g_ble_ctx.pending_peer_commitment_len  = CEEPEW_COMMITMENT_ADV_BYTES;
    memcpy(g_ble_ctx.pending_peer_commitment, shared_commitment,
           CEEPEW_COMMITMENT_ADV_BYTES);

    CeePewErr_t err = transport_ble_verify_pending_commitment();
    ok &= handoff_check(err == CEEPEW_OK, "matching beacon returns OK");
    ok &= handoff_check(g_ble_ctx.commitment_verified,
                        "commitment_verified set on match");
    ok &= handoff_check(g_ble_ctx.handoff_ready,
                        "handoff_ready set on match");
    ok &= handoff_check(g_ble_ctx.ready_for_chat,
                        "ready_for_chat set on match");
    ok &= handoff_check(g_ble_ctx.state == BLE_DONE,
                        "state advanced to BLE_DONE on match");
    ok &= handoff_check(!g_ble_ctx.peer_commitment_pending,
                        "peer_commitment_pending cleared on match");
    ok &= handoff_check(transport_ble_handoff_ready(),
                        "transport_ble_handoff_ready() returns true after match");

    return ok;
}

/*
 * Test 3: Mismatching beacon commitments must FAIL the verification and
 * return CEEPEW_ERR_AUTH_FAIL.  The UI state machine handles the user-
 * facing failure transition; we assert that the BLE state does NOT advance.
 */
static bool handoff_test_mismatch_does_not_promote(void)
{
    bool ok = true;

    uint8_t local_commitment[CEEPEW_COMMITMENT_ADV_BYTES];
    uint8_t peer_commitment[CEEPEW_COMMITMENT_ADV_BYTES];
    for (uint8_t i = 0U; i < CEEPEW_COMMITMENT_ADV_BYTES; i++) {
        local_commitment[i] = (uint8_t)(0x20U + i);
        peer_commitment[i]  = (uint8_t)(0x90U + i);
    }

    /* Build a full CEEPEW_COMMITMENT_BYTES commitment from the local ADV
     * data and register it so session_get_commitment() returns a value that
     * matches the local side but mismatches the peer. */
    uint8_t full_commitment[CEEPEW_COMMITMENT_BYTES];
    memcpy(full_commitment, local_commitment, CEEPEW_COMMITMENT_ADV_BYTES);
    memset(full_commitment + CEEPEW_COMMITMENT_ADV_BYTES, 0x43U,
           CEEPEW_COMMITMENT_BYTES - CEEPEW_COMMITMENT_ADV_BYTES);
    session_test_set_commitment(full_commitment);

    g_ble_ctx.commitment_verified          = false;
    g_ble_ctx.handoff_ready                = false;
    g_ble_ctx.ready_for_chat               = false;
    g_ble_ctx.state                        = BLE_ADVERTISING_AND_SCANNING;
    g_ble_ctx.local_commitment_len         = CEEPEW_COMMITMENT_BYTES;
    memcpy(g_ble_ctx.commitment_digest, full_commitment,
           CEEPEW_COMMITMENT_BYTES);
    g_ble_ctx.peer_commitment_pending      = true;
    g_ble_ctx.pending_peer_commitment_len  = CEEPEW_COMMITMENT_ADV_BYTES;
    memcpy(g_ble_ctx.pending_peer_commitment, peer_commitment,
           CEEPEW_COMMITMENT_ADV_BYTES);

    CeePewErr_t err = transport_ble_verify_pending_commitment();
    ok &= handoff_check(err == CEEPEW_ERR_AUTH_FAIL,
                        "mismatching beacon returns ERR_AUTH_FAIL");
    ok &= handoff_check(!g_ble_ctx.commitment_verified,
                        "commitment_verified stays false on mismatch");
    ok &= handoff_check(!g_ble_ctx.handoff_ready,
                        "handoff_ready stays false on mismatch");
    ok &= handoff_check(!g_ble_ctx.ready_for_chat,
                        "ready_for_chat stays false on mismatch");
    ok &= handoff_check(g_ble_ctx.state == BLE_DONE,
                        "state is BLE_DONE after failed verification");
    ok &= handoff_check(!transport_ble_handoff_ready(),
                        "transport_ble_handoff_ready() returns false on mismatch");

    return ok;
}

/*
 * Test 4: No pending peer commitment → the call is a no-op and returns
 * CEEPEW_OK.  This is the expected state between beacon exchanges.
 */
static bool handoff_test_no_pending_returns_ok(void)
{
    bool ok = true;

    g_ble_ctx.commitment_verified     = false;
    g_ble_ctx.handoff_ready           = false;
    g_ble_ctx.ready_for_chat          = false;
    g_ble_ctx.state                   = BLE_ADVERTISING_AND_SCANNING;
    g_ble_ctx.local_commitment_len    = CEEPEW_COMMITMENT_BYTES;
    g_ble_ctx.peer_commitment_pending = false;
    g_ble_ctx.pending_peer_commitment_len = 0U;

    CeePewErr_t err = transport_ble_verify_pending_commitment();
    ok &= handoff_check(err == CEEPEW_OK,
                        "no pending commitment returns OK");
    ok &= handoff_check(!g_ble_ctx.commitment_verified,
                        "commitment_verified stays false on no-op");
    ok &= handoff_check(!g_ble_ctx.handoff_ready,
                        "handoff_ready stays false on no-op");

    return ok;
}

/* Public entry point. */
void test_pairing_handoff_run(void)
{
    ESP_LOGI(TAG, "=== Pairing handoff (beacon) test ===");

    /* Save the entire BLE context before any test mutates it. */
    BleContext_t saved_ctx;
    memcpy(&saved_ctx, &g_ble_ctx, sizeof(BleContext_t));

    uint32_t passed = 0U, failed = 0U;
    bool ok;

    ok = handoff_test_defers_without_local_commitment();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    ok = handoff_test_matching_beacon_promotes();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    ok = handoff_test_mismatch_does_not_promote();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    ok = handoff_test_no_pending_returns_ok();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    /* Restore BLE context — zero side effects on production state. */
    memcpy(&g_ble_ctx, &saved_ctx, sizeof(BleContext_t));

    /* Clear test commitment so later test suites aren't affected. */
    session_test_unset_commitment();

    ESP_LOGI(TAG, "Pairing handoff test summary: passed=%u failed=%u (g_ble_ctx restored)",
             (unsigned)passed, (unsigned)failed);
}
