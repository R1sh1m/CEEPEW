/* components/hal/hal_adc.c */
#include "hal_adc.h"
#include "../../main/ceepew_config.h"
#include "esp_adc_cal.h"
#include "driver/adc.h"

/* Provide sensible defaults if the project didn't define them */
#ifndef CEEPEW_ADC_SAMPLES_PER_READ
#define CEEPEW_ADC_SAMPLES_PER_READ 8U
#endif

#ifndef CEEPEW_ADC_VREF_MV
#define CEEPEW_ADC_VREF_MV 1100U
#endif

static bool s_initialised = false;
static esp_adc_cal_characteristics_t s_adc_chars;

CeePewErr_t hal_adc_init(void)
{
    CEEPEW_ASSERT(!s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_ADC_SAMPLES_PER_READ > 0U, CEEPEW_ERR_PARAM);
    /* Configure ADC1 width and channel */
    esp_err_t rc = adc1_config_width(ADC_WIDTH_BIT_12);
    if (rc != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }
    rc = adc1_config_channel_atten(CEEPEW_ADC_CHANNEL_POT, CEEPEW_ADC_ATTEN);
    if (rc != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }

    esp_adc_cal_characterize(ADC_UNIT_1, CEEPEW_ADC_ATTEN, ADC_WIDTH_BIT_12,
                             CEEPEW_ADC_VREF_MV, &s_adc_chars);

    s_initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t hal_adc_read_raw(uint16_t *out_raw)
{
    CEEPEW_ASSERT(out_raw != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_initialised, CEEPEW_ERR_BUSY);

    uint32_t total = 0U;
    for (uint32_t i = 0U; i < CEEPEW_ADC_SAMPLES_PER_READ; i++)
    {
        int raw = adc1_get_raw(CEEPEW_ADC_CHANNEL_POT);
        if (raw < 0)
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
    *out_raw = avg;
    return CEEPEW_OK;
}
