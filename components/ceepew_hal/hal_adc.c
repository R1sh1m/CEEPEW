/* components/hal/hal_adc.c */
#include "hal_adc.h"
#include "../../main/ceepew_config.h"
#include "hal_pins.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* Provide sensible defaults if the project didn't define them */
#ifndef CEEPEW_ADC_SAMPLES_PER_READ
#define CEEPEW_ADC_SAMPLES_PER_READ 8U
#endif

static bool s_initialised = false;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_adc_calibrated = false;

CeePewErr_t hal_adc_init(void)
{
    CEEPEW_ASSERT(!s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_ADC_SAMPLES_PER_READ > 0U, CEEPEW_ERR_PARAM);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t rc = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (rc != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = CEEPEW_ADC_ATTEN,
        .bitwidth = CEEPEW_ADC_WIDTH,
    };
    rc = adc_oneshot_config_channel(s_adc_handle, CEEPEW_ADC_CHANNEL_POT, &chan_cfg);
    if (rc != ESP_OK)
    {
        (void)adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return CEEPEW_ERR_HW;
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = CEEPEW_ADC_CHANNEL_POT,
        .atten = CEEPEW_ADC_ATTEN,
        .bitwidth = CEEPEW_ADC_WIDTH,
    };
    s_adc_calibrated = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle) == ESP_OK);

    s_initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t hal_adc_read_raw(uint16_t *out_raw)
{
    CEEPEW_ASSERT(out_raw != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_adc_handle != NULL, CEEPEW_ERR_INTERNAL);

    uint32_t total = 0U;
    for (uint32_t i = 0U; i < CEEPEW_ADC_SAMPLES_PER_READ; i++)
    {
        int raw = 0;
        esp_err_t rc = adc_oneshot_read(s_adc_handle, CEEPEW_ADC_CHANNEL_POT, &raw);
        if (rc != ESP_OK)
        {
            return CEEPEW_ERR_HW;
        }
        total += (uint32_t)raw;
    }

    uint16_t avg = (uint16_t)(total / CEEPEW_ADC_SAMPLES_PER_READ);
    if (avg > CEEPEW_ADC_MAX_RAW)
    {
        avg = (uint16_t)CEEPEW_ADC_MAX_RAW;
    }

    if (s_adc_calibrated)
    {
        int mv = 0;
        (void)adc_cali_raw_to_voltage(s_adc_cali_handle, (int)avg, &mv);
    }

    *out_raw = avg;
    return CEEPEW_OK;
}
