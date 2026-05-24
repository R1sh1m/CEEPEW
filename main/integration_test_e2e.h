/* main/integration_test_e2e.h
 *
 * End-to-end integration test suite for CEE-PEW firmware.
 */

#ifndef INTEGRATION_TEST_E2E_H
#define INTEGRATION_TEST_E2E_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run all integration tests. Logs results to ESP_LOG.
 * Tests are non-destructive and can be run multiple times. */
void integration_tests_run_all(void);

#ifdef __cplusplus
}
#endif

#endif /* INTEGRATION_TEST_E2E_H */
