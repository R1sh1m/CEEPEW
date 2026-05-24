/* components/ceepew_hal/esp_adc_cal.h
 *
 * ADC Calibration abstraction layer — production-grade stable API.
 *
 * Provides a unified ADC calibration interface across ESP-IDF versions
 * (v5.x legacy esp_adc_cal API vs v6.0+ modern esp_adc API).
 *
 * This header defines both:
 * 1. Legacy compatibility types for v5.x codebases
 * 2. Production calibration utilities (eFuse reading, linearity correction)
 * 3. Diagnostic helpers (calibration status reporting)
 *
 * All functions are constant-time where calibration data is sensitive
 * (vref, eFuse values are NOT secret in CEE-PEW — only keys are secret).
 *
 * THREAD SAFETY: All functions are stateless; no global state modification.
 * Caller responsible for synchronizing calls to the same adc_cali_handle.
 */
#ifndef ESP_ADC_CAL_H
#define ESP_ADC_CAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────── */
/* ADC Unit / Channel Definitions (cross-IDF compatibility)                */
/* ────────────────────────────────────────────────────────────────────── */

#define ADC_UNIT_1                     1
#define ADC_UNIT_2                     2
#define ADC_WIDTH_BIT_9                9
#define ADC_WIDTH_BIT_10               10
#define ADC_WIDTH_BIT_11               11
#define ADC_WIDTH_BIT_12               12
#define ADC_WIDTH_BIT_13               13
#define ADC_ATTEN_DB_0                 0
#define ADC_ATTEN_DB_2_5               1
#define ADC_ATTEN_DB_6                 2
#define ADC_ATTEN_DB_11                3

/* ────────────────────────────────────────────────────────────────────── */
/* Legacy Calibration Structure (ESP-IDF v5.x compatibility)              */
/* ────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t coeff_a;                  /* Linearity coefficient A */
    uint32_t coeff_b;                  /* Linearity coefficient B */
    uint32_t vref;                     /* Reference voltage (mV) */
    uint32_t vref_default;             /* eFuse-stored default vref */
    int      digi_grade;               /* Digital grade (EFUSE_ADC_CALIBRATION_DIG_GRADE) */
} esp_adc_cal_characteristics_t;

/* ────────────────────────────────────────────────────────────────────── */
/* Calibration Mode Enumeration                                           */
/* ────────────────────────────────────────────────────────────────────── */

typedef enum {
    ESP_ADC_CAL_VAL_DEFAULT_VREF  = 0,   /* Use default eFuse vref (1100mV) */
    ESP_ADC_CAL_VAL_EFUSE_VREF    = 1,   /* Read eFuse-burned vref */
    ESP_ADC_CAL_VAL_EFUSE_TP      = 2,   /* Two-point calibration (if burned) */
    ESP_ADC_CAL_VAL_EFUSE_LUT     = 3,   /* LUT-based (future, not yet supported) */
    ESP_ADC_CAL_VAL_MAX           = 4
} esp_adc_cal_value_t;

/* ────────────────────────────────────────────────────────────────────── */
/* Diagnostics Structure                                                  */
/* ────────────────────────────────────────────────────────────────────── */

typedef struct {
    bool       efuse_vref_valid;       /* eFuse vref is present and usable */
    bool       efuse_tp_valid;         /* Two-point calibration burned */
    uint32_t   vref_stored;            /* eFuse vref value (mV) */
    uint8_t    adc_calib_version;      /* eFuse calibration version */
    uint32_t   adc_calib_digits;       /* Number of calibration points stored */
} esp_adc_cal_diagnostics_t;

/* ────────────────────────────────────────────────────────────────────── */
/* Function Declarations: Legacy (v5.x) Compatibility                     */
/* ────────────────────────────────────────────────────────────────────── */

/* Characterize ADC channel for voltage conversion.
 * unit:          ADC_UNIT_1 or ADC_UNIT_2
 * atten:         ADC_ATTEN_DB_0..11 (input attenuation level)
 * bit_width:     ADC_WIDTH_BIT_9..13 (resolution)
 * default_vref:  reference voltage in mV (typically 1100 mV)
 * chars:         output struct to populate
 * 
 * ASSERTION REQUIREMENTS:
 * - chars != NULL
 * - bit_width >= 9 && bit_width <= 13
 * - atten >= 0 && atten < 4
 * - default_vref > 0 && default_vref <= 1300 mV
 */
void esp_adc_cal_characterize(int unit,
                               int atten,
                               int bit_width,
                               uint32_t default_vref,
                               esp_adc_cal_characteristics_t *chars);

/* Read raw ADC and convert to voltage using pre-computed characteristics.
 * raw:        raw ADC reading (0 to 2^bit_width - 1)
 * chars:      pre-computed calibration characteristics
 * voltage:    output voltage in mV
 *
 * ASSERTION REQUIREMENTS:
 * - chars != NULL
 * - voltage != NULL
 *
 * CONSTANT-TIME: Yes (no secret data branch)
 *
 * RETURN: voltage in mV (0 to ~1100 depending on atten)
 */
uint32_t esp_adc_cal_raw_to_voltage(uint32_t raw,
                                     const esp_adc_cal_characteristics_t *chars);

/* ────────────────────────────────────────────────────────────────────── */
/* Function Declarations: Production Features                             */
/* ────────────────────────────────────────────────────────────────────── */

/* Get the best available calibration mode on this chip.
 * Returns the highest-priority calibration method that is actually
 * available (eFuse two-point > eFuse vref > default).
 *
 * RETURN: esp_adc_cal_value_t indicating which calibration is active
 */
esp_adc_cal_value_t esp_adc_cal_get_best_available(void);

/* Read eFuse-stored reference voltage (if available).
 * vref_out:  output parameter for vref in mV
 *
 * ASSERTION REQUIREMENTS:
 * - vref_out != NULL
 *
 * RETURN: true if eFuse vref was successfully read and is valid,
 *         false if eFuse vref is not burned or is invalid
 */
bool esp_adc_cal_get_efuse_vref(uint32_t *vref_out);

/* Get detailed calibration diagnostics.
 * diag_out:  output diagnostics structure
 *
 * ASSERTION REQUIREMENTS:
 * - diag_out != NULL
 *
 * Populates diag_out with eFuse calibration status, valid calibration
 * methods, and stored reference values. Used for production testing and
 * firmware diagnostics.
 */
void esp_adc_cal_get_diagnostics(esp_adc_cal_diagnostics_t *diag_out);

/* ────────────────────────────────────────────────────────────────────── */
/* Inline Utilities                                                       */
/* ────────────────────────────────────────────────────────────────────── */

/* Validate bit width parameter.
 * bit_width: resolution in bits (9-13)
 * RETURN: true if valid, false otherwise
 */
static inline bool esp_adc_cal_is_valid_bitwidth(int bit_width) {
    return bit_width >= 9 && bit_width <= 13;
}

/* Validate attenuation parameter.
 * atten: attenuation level (0-3, representing 0/2.5/6/11 dB)
 * RETURN: true if valid, false otherwise
 */
static inline bool esp_adc_cal_is_valid_atten(int atten) {
    return atten >= 0 && atten < 4;
}

/* Get max expected ADC reading for a given bit width.
 * RETURN: 2^bit_width - 1
 */
static inline uint32_t esp_adc_cal_get_max_raw(int bit_width) {
    return (1U << (unsigned int)bit_width) - 1U;
}

#ifdef __cplusplus
}
#endif

#endif /* ESP_ADC_CAL_H */
