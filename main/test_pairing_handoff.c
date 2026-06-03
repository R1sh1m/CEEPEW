/* main/test_pairing_handoff.c
 *
 * Regression test for the "optimistic BLE handoff" bug.
 *
 * Pre-fix behaviour: the initiator flipped handoff_ready / commitment_verified
 * / state = BLE_DONE immediately on receiving a GATTC write ACK for its own
 * commitment. The BLE-stack ACK only proves the responder's GATT layer
 * received the bytes — not that the responder's application verified the
 * session code. The result was the "one device enters chat, the other fails"
 * symptom observed on the bench.
 *
 * Post-fix behaviour: the initiator only promotes to BLE_DONE after
 * peer_verification_result == CEEPEW_VERIFY_OK is observed. The
 * transport_ble_check_verification_result() poll is the single point at which
 * the handoff becomes ready.
 *
 * This test exercises the post-fix path by directly manipulating
 * g_ble_ctx.peer_verification_result and asserting the right outcomes from
 * the public verification API. It does not require a second physical device.
 */

#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "transport_ble.h"
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
 * Test 1: A GATTC write ACK alone (no peer verification result observed)
 * must NOT promote the initiator to BLE_DONE / handoff_ready.
 *
 * We simulate this by ensuring peer_verification_pending is set and
 * peer_verification_result remains CEEPEW_VERIFY_PENDING. After polling
 * transport_ble_check_verification_result, handoff_ready must remain false.
 */
static bool handoff_test_write_ack_does_not_promote(void)
{
    bool ok = true;

    /* Establish a baseline: clear flags, set pending, leave result at PENDING. */
    g_ble_ctx.commitment_verified = false;
    g_ble_ctx.handoff_ready       = false;
    g_ble_ctx.state               = BLE_PAIRING;
    g_ble_ctx.peer_verification_pending = true;
    g_ble_ctx.peer_verification_result  = CEEPEW_VERIFY_PENDING;
    g_ble_ctx.verification_timeout_ms   = 0U;  /* never time out in this test */

    /* Poll the verification-result watchdog. It must NOT promote handoff. */
    CeePewErr_t err = transport_ble_check_verification_result();
    ok &= handoff_check(err == CEEPEW_OK, "poll returns OK while result is PENDING");
    ok &= handoff_check(!g_ble_ctx.commitment_verified,
                        "commitment_verified stays false on PENDING");
    ok &= handoff_check(!g_ble_ctx.handoff_ready,
                        "handoff_ready stays false on PENDING");
    ok &= handoff_check(g_ble_ctx.state == BLE_PAIRING,
                        "state stays BLE_PAIRING on PENDING");

    return ok;
}

/*
 * Test 2: A CEEPEW_VERIFY_OK result must promote the initiator to
 * BLE_DONE / handoff_ready, but only via the verification-result path
 * (NOT via a write-ACK path that the bug fix removed).
 */
static bool handoff_test_ok_result_promotes(void)
{
    bool ok = true;

    g_ble_ctx.commitment_verified = false;
    g_ble_ctx.handoff_ready       = false;
    g_ble_ctx.state               = BLE_PAIRING;
    g_ble_ctx.peer_verification_pending = true;
    g_ble_ctx.peer_verification_result  = CEEPEW_VERIFY_OK;
    g_ble_ctx.verification_timeout_ms   = 0U;

    CeePewErr_t err = transport_ble_check_verification_result();
    ok &= handoff_check(err == CEEPEW_OK, "poll returns OK on VERIFY_OK");
    ok &= handoff_check(g_ble_ctx.commitment_verified,
                        "commitment_verified flipped to true on VERIFY_OK");
    ok &= handoff_check(g_ble_ctx.handoff_ready,
                        "handoff_ready flipped to true on VERIFY_OK");
    ok &= handoff_check(!g_ble_ctx.peer_verification_pending,
                        "peer_verification_pending cleared on VERIFY_OK");
    ok &= handoff_check(transport_ble_handoff_ready(),
                        "transport_ble_handoff_ready() returns true after OK");

    return ok;
}

/*
 * Test 3: A CEEPEW_VERIFY_MISMATCH result must NOT promote; it must return
 * CEEPEW_ERR_AUTH_FAIL so the caller (task_session) can transition to
 * UI_STATE_PAIRING_FAILED.
 */
static bool handoff_test_mismatch_does_not_promote(void)
{
    bool ok = true;

    g_ble_ctx.commitment_verified = false;
    g_ble_ctx.handoff_ready       = false;
    g_ble_ctx.state               = BLE_PAIRING;
    g_ble_ctx.peer_verification_pending = true;
    g_ble_ctx.peer_verification_result  = CEEPEW_VERIFY_MISMATCH;
    g_ble_ctx.verify_fail_count         = 0U;
    g_ble_ctx.verification_timeout_ms   = 0U;

    CeePewErr_t err = transport_ble_check_verification_result();
    ok &= handoff_check(err == CEEPEW_ERR_AUTH_FAIL,
                        "poll returns ERR_AUTH_FAIL on MISMATCH");
    ok &= handoff_check(!g_ble_ctx.commitment_verified,
                        "commitment_verified stays false on MISMATCH");
    ok &= handoff_check(!g_ble_ctx.handoff_ready,
                        "handoff_ready stays false on MISMATCH");
    ok &= handoff_check(!g_ble_ctx.peer_verification_pending,
                        "peer_verification_pending cleared on MISMATCH");
    ok &= handoff_check(g_ble_ctx.verify_fail_count == 1U,
                        "verify_fail_count incremented on MISMATCH");
    ok &= handoff_check(!transport_ble_handoff_ready(),
                        "transport_ble_handoff_ready() returns false on MISMATCH");

    return ok;
}

/*
 * Test 4: A verification-result timeout must return CEEPEW_ERR_HW so the
 * caller can fail over to UI_STATE_PAIRING_FAILED with reason LINK_FAIL.
 */
static bool handoff_test_timeout_does_not_promote(void)
{
    bool ok = true;

    g_ble_ctx.commitment_verified = false;
    g_ble_ctx.handoff_ready       = false;
    g_ble_ctx.state               = BLE_PAIRING;
    g_ble_ctx.peer_verification_pending = true;
    g_ble_ctx.peer_verification_result  = CEEPEW_VERIFY_PENDING;
    g_ble_ctx.verify_fail_count         = 0U;
    /* Force timeout by setting verification_timeout_ms to a value already in the past. */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    g_ble_ctx.verification_timeout_ms = (now_ms > 0U) ? (now_ms - 1U) : 0U;

    CeePewErr_t err = transport_ble_check_verification_result();
    ok &= handoff_check(err == CEEPEW_ERR_HW,
                        "poll returns ERR_HW on verification timeout");
    ok &= handoff_check(!g_ble_ctx.commitment_verified,
                        "commitment_verified stays false on timeout");
    ok &= handoff_check(!g_ble_ctx.handoff_ready,
                        "handoff_ready stays false on timeout");
    ok &= handoff_check(g_ble_ctx.verify_fail_count == 1U,
                        "verify_fail_count incremented on timeout");

    return ok;
}

/* Public entry point. */
void test_pairing_handoff_run(void)
{
    ESP_LOGI(TAG, "=== Pairing handoff regression test ===");

    uint32_t passed = 0U, failed = 0U;
    bool ok;

    ok = handoff_test_write_ack_does_not_promote();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    ok = handoff_test_ok_result_promotes();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    ok = handoff_test_mismatch_does_not_promote();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    ok = handoff_test_timeout_does_not_promote();
    passed += ok ? 1U : 0U; failed += ok ? 0U : 1U;

    ESP_LOGI(TAG, "Pairing handoff test summary: passed=%u failed=%u",
             (unsigned)passed, (unsigned)failed);
}
