/* components/ceepew_hal/hal_i2c_scanner.h
 *
 * I2C Bus Scanner — Boot-time Diagnostic Utility
 * Scans the I2C bus for responding devices and logs results to serial console.
 * Intended for troubleshooting OLED discovery and wiring issues.
 */

#ifndef CEE_PEW_HAL_I2C_SCANNER_H
#define CEE_PEW_HAL_I2C_SCANNER_H

#include <stdint.h>
#include "ceepew_assert.h"

/* I2C Address Range for Scanning
 *
 * Standard 7-bit I2C addresses range from 0x00 to 0x7F.
 * Reserved ranges:
 *   0x00–0x02: General call addresses
 *   0x78–0x7F: Reserved for future use
 *
 * Usable device addresses: 0x03–0x77
 */
#define I2C_SCAN_ADDR_MIN  0x03U
#define I2C_SCAN_ADDR_MAX  0x77U
#define I2C_SCAN_PROBE_TIMEOUT_MS 20U

/* Scan the I2C bus on the primary pins (CEEPEW_PIN_I2C_SDA, CEEPEW_PIN_I2C_SCL)
 * at the standard frequency (CEEPEW_I2C_FREQ_HZ).
 *
 * Probes all addresses from I2C_SCAN_ADDR_MIN to I2C_SCAN_ADDR_MAX and logs
 * each responding device to ESP_LOG with tag "i2c_scanner".
 *
 * Returns CEEPEW_OK if the scan completes (even if no devices are found).
 * Returns CEEPEW_ERR_HW if the I2C bus cannot be initialized.
 *
 * This function is blocking and synchronous. It allocates and deletes an I2C
 * master bus internally, so it should only be called once per boot (or for
 * explicit diagnostics).
 */
CeePewErr_t hal_i2c_scanner_scan_bus(void);

#endif /* CEE_PEW_HAL_I2C_SCANNER_H */
