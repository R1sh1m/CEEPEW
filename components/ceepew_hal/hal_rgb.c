#include "hal_rgb.h"

#include "ceepew_config.h"
#include "hal_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include <limits.h>
#include <math.h>

typedef struct{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t duration_ms;
} RgbStep_t;

typedef struct {
    const RgbStep_t *steps;
    uint8_t step_count;
    bool loop;
} RgbPatternDef_t;

/* PWM pulse mode state */
typedef enum {
    RGB_MODE_PATTERN = 0U,  /* Discrete on/off blinking */
    RGB_MODE_PWM = 1U       /* Smooth LEDC PWM pulsing */
} RgbMode_t;

/* PWM context for smooth sine-wave pulsing */
typedef struct {
    uint8_t r_duty;         /* Current R duty cycle (0-255) */
    uint8_t g_duty;         /* Current G duty cycle (0-255) */
    uint8_t b_duty;         /* Current B duty cycle (0-255) */
    uint8_t step_index;     /* Current position in sine table (0-255) */
} RgbPwmCtx_t;

typedef struct{
    bool initialised;
    bool timer_running;
    RgbPattern_t active_pattern;
    uint8_t step_index;
    TimerHandle_t timer;
    SemaphoreHandle_t lock;
    RgbMode_t mode;         /* Pattern or PWM mode */
    RgbPwmCtx_t pwm;        /* PWM state */
} RgbState_t;

static StaticTimer_t s_timer_buf;
static StaticSemaphore_t s_lock_buf;
static RgbState_t s_state = { .initialised = false, .timer_running = false, .active_pattern = RGB_OFF, .step_index = 0U, .timer = NULL, .lock = NULL, .mode = RGB_MODE_PATTERN, .pwm = {0}};

/* Sine wave lookup table (256 entries) for smooth 1 Hz PWM pulsing
   Values computed as: 128 + 127*sin(2π*i/256) for i in [0, 255]
   This creates a full sine wave cycle used to modulate PWM duty cycle */
static const uint8_t s_sine_lut[256] = {
    128U, 131U, 134U, 137U, 140U, 143U, 146U, 149U,
    152U, 155U, 158U, 161U, 164U, 167U, 170U, 173U,
    176U, 179U, 182U, 185U, 188U, 190U, 193U, 196U,
    199U, 201U, 204U, 206U, 209U, 211U, 214U, 216U,
    219U, 221U, 223U, 225U, 227U, 229U, 231U, 233U,
    235U, 236U, 238U, 239U, 241U, 242U, 244U, 245U,
    246U, 247U, 248U, 249U, 250U, 251U, 251U, 252U,
    252U, 253U, 253U, 253U, 254U, 254U, 254U, 254U,
    254U, 254U, 254U, 254U, 253U, 253U, 253U, 252U,
    252U, 251U, 251U, 250U, 249U, 248U, 247U, 246U,
    245U, 244U, 242U, 241U, 239U, 238U, 236U, 235U,
    233U, 231U, 229U, 227U, 225U, 223U, 221U, 219U,
    216U, 214U, 211U, 209U, 206U, 204U, 201U, 199U,
    196U, 193U, 190U, 188U, 185U, 182U, 179U, 176U,
    173U, 170U, 167U, 164U, 161U, 158U, 155U, 152U,
    149U, 146U, 143U, 140U, 137U, 134U, 131U, 128U,
    125U, 122U, 119U, 116U, 113U, 110U, 107U, 104U,
    101U,  98U,  95U,  92U,  89U,  86U,  83U,  80U,
     77U,  74U,  71U,  68U,  65U,  62U,  59U,  56U,
     53U,  51U,  48U,  46U,  43U,  41U,  38U,  36U,
     33U,  31U,  29U,  27U,  25U,  23U,  21U,  19U,
     17U,  16U,  14U,  13U,  11U,  10U,   8U,   7U,
      6U,   5U,   4U,   3U,   3U,   2U,   2U,   1U,
      1U,   1U,   0U,   0U,   0U,   0U,   0U,   0U,
      0U,   0U,   1U,   1U,   1U,   2U,   2U,   3U,
      3U,   4U,   5U,   6U,   7U,   8U,  10U,  11U,
     13U,  14U,  16U,  17U,  19U,  21U,  23U,  25U,
     27U,  29U,  31U,  33U,  36U,  38U,  41U,  43U,
     46U,  48U,  51U,  53U,  56U,  59U,  62U,  65U,
     68U,  71U,  74U,  77U,  80U,  83U,  86U,  89U,
     92U,  95U,  98U, 101U, 104U, 107U, 110U, 113U,
    116U, 119U, 122U, 125U
};

static const RgbStep_t s_steps_off[] = {    {0U, 0U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_red[] = {    {1U, 0U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_green[] = {    {0U, 1U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_blue[] = {    {0U, 0U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_yellow[] = {    {1U, 1U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_cyan[] = {    {0U, 1U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_magenta[] = {    {1U, 0U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_white[] = {    {1U, 1U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_red_blink[] = {    {1U, 0U, 0U, 300U},    {0U, 0U, 0U, 300U}};
static const RgbStep_t s_steps_green_blink[] = {    {0U, 1U, 0U, 300U},    {0U, 0U, 0U, 300U}};
static const RgbStep_t s_steps_blue_blink[] = {    {0U, 0U, 1U, 300U},    {0U, 0U, 0U, 300U}};

/* PWM pulse patterns — marked with UINT32_MAX duration to signal PWM mode
   These are interpreted specially: the pattern defines which channels to pulse,
   and the duration is ignored (PWM runs continuously until switched).
   Pulse patterns: 1.0 Hz (960ms period) sine wave on active channels */
static const RgbStep_t s_steps_white_pulse[] = {    {1U, 1U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_blue_pulse[] = {    {0U, 0U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_green_pulse[] = {    {0U, 1U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_amber_pulse[] = {    {1U, 1U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_cyan_pulse[] = {    {0U, 1U, 1U, UINT32_MAX}};

static const RgbStep_t s_steps_rainbow_cycle[] = {    {1U, 0U, 0U, 250U},    {0U, 1U, 0U, 250U},    {0U, 0U, 1U, 250U}};
static const RgbStep_t s_steps_heartbeat[] = {    {1U, 0U, 0U, 140U},    {0U, 0U, 0U, 120U},    {1U, 0U, 0U, 140U},    {0U, 0U, 0U, 700U}};
/* Supervisor recovery indicator — alternating yellow then red, 250ms each, looping.
 * Distinct from RGB_RED_BLINK (pure red) and RGB_AMBER_PULSE (PWM yellow)
 * so the user can tell at a glance that the radio is being reset. */
static const RgbStep_t s_steps_yellow_red_blink[] = {    {1U, 1U, 0U, 250U},    {1U, 0U, 0U, 250U}};
static const RgbStep_t s_steps_cyan_blink[] = {         {0U, 1U, 1U, 250U},    {0U, 0U, 0U, 250U}};

static const RgbPatternDef_t s_patterns[RGB_PATTERN_COUNT] = {
    {s_steps_off, 1U, false},
    {s_steps_red, 1U, false},
    {s_steps_green, 1U, false},
    {s_steps_blue, 1U, false},
    {s_steps_yellow, 1U, false},
    {s_steps_cyan, 1U, false},
    {s_steps_magenta, 1U, false},
    {s_steps_white, 1U, false},
    {s_steps_red_blink, 2U, true},
    {s_steps_green_blink, 2U, true},
    {s_steps_blue_blink, 2U, true},
    {s_steps_white_pulse, 1U, false},   /* PWM mode: smooth white breathe */
    {s_steps_blue_pulse, 1U, false},    /* PWM mode: smooth blue breathe  */
    {s_steps_green_pulse, 1U, false},   /* PWM mode: smooth green breathe */
    {s_steps_amber_pulse, 1U, false},   /* PWM mode: smooth amber breathe */
    {s_steps_cyan_pulse, 1U, false},    /* PWM mode: smooth cyan breathe  */
    {s_steps_yellow_red_blink, 2U, true},  /* Supervisor recovery: yellow↔red blink */
    {s_steps_cyan_blink, 2U, true},        /* GATT identity exchange: cyan↔off blink */
    {s_steps_rainbow_cycle, 3U, true},
    {s_steps_heartbeat, 4U, true}
};

static TickType_t rgb_ms_to_ticks(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0U){ ticks = 1U;}
    return ticks;
}

static void rgb_apply_levels(uint8_t r, uint8_t g, uint8_t b){
    CEEPEW_ASSERT_VOID(r <= 1U && g <= 1U && b <= 1U);
    CEEPEW_ASSERT_VOID(s_state.initialised);
    gpio_set_level(CEEPEW_PIN_RGB_RED, (int)r);
    gpio_set_level(CEEPEW_PIN_RGB_GREEN, (int)g);
    gpio_set_level(CEEPEW_PIN_RGB_BLUE, (int)b);
}

static CeePewErr_t rgb_apply_step_locked(const RgbPatternDef_t *pattern) {
    CEEPEW_ASSERT(pattern != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pattern->steps != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pattern->step_count > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_state.step_index < pattern->step_count, CEEPEW_ERR_BOUNDS);
    const RgbStep_t *step = &pattern->steps[s_state.step_index];
    CEEPEW_ASSERT(step->duration_ms > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(step->r <= 1U && step->g <= 1U && step->b <= 1U, CEEPEW_ERR_PARAM);
    rgb_apply_levels(step->r, step->g, step->b);
    return CEEPEW_OK;
}

static CeePewErr_t rgb_arm_timer_locked(const RgbPatternDef_t *pattern) {
    CEEPEW_ASSERT(pattern != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pattern->steps != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(pattern->step_count > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_state.step_index < pattern->step_count, CEEPEW_ERR_BOUNDS);
    CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_INTERNAL);

    const RgbStep_t *step = &pattern->steps[s_state.step_index];
    TickType_t period = rgb_ms_to_ticks(step->duration_ms);
    (void)xTimerChangePeriod(s_state.timer, period, 0U);
    (void)xTimerStart(s_state.timer, 0U);
    s_state.timer_running = true;
    return CEEPEW_OK;
}

static CeePewErr_t rgb_ledc_init(void) {
    /* Initialize LEDC PWM for smooth sine-wave pulsing on RGB channels
       Uses LEDC_LOW_SPEED_MODE at 160 kHz frequency, 8-bit resolution (0-255).
       All 3 channels share one timer; individual channels have independent duty cycles. */
    CEEPEW_ASSERT(GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_RED) &&
                  GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_GREEN) &&
                  GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_BLUE), CEEPEW_ERR_PINS);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,  /* 0-255 range */
        .freq_hz = 160000,                     /* 160 kHz */
        .clk_cfg = LEDC_AUTO_CLK
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    /* Configure R channel (GPIO 2) on LEDC_CHANNEL_0 */
    ledc_channel_config_t ch_r = {
        .gpio_num = CEEPEW_PIN_RGB_RED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0U,
        .hpoint = 0U,
        .flags.output_invert = 0U
    };
    err = ledc_channel_config(&ch_r);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    /* Configure G channel (GPIO 18) on LEDC_CHANNEL_1 */
    ledc_channel_config_t ch_g = {
        .gpio_num = CEEPEW_PIN_RGB_GREEN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0U,
        .hpoint = 0U,
        .flags.output_invert = 0U
    };
    err = ledc_channel_config(&ch_g);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    /* Configure B channel (GPIO 23) on LEDC_CHANNEL_2 */
    ledc_channel_config_t ch_b = {
        .gpio_num = CEEPEW_PIN_RGB_BLUE,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0U,
        .hpoint = 0U,
        .flags.output_invert = 0U
    };
    err = ledc_channel_config(&ch_b);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    return CEEPEW_OK;
}

static CeePewErr_t rgb_apply_pwm_duty(uint8_t r_duty, uint8_t g_duty, uint8_t b_duty) {
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_PARAM);

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, r_duty);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, g_duty);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, b_duty);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    if (err != ESP_OK) { return CEEPEW_ERR_HW; }

    return CEEPEW_OK;
}

/* Unified timer callback — dispatches based on current mode (pattern or PWM) */
static void rgb_unified_timer_cb(TimerHandle_t timer) {
    CEEPEW_ASSERT_VOID(timer != NULL);
    CEEPEW_ASSERT_VOID(s_state.lock != NULL);
    CEEPEW_ASSERT_VOID(s_state.initialised);

    if (xSemaphoreTake(s_state.lock, 0U) != pdTRUE) {
        (void)xTimerChangePeriod(timer, rgb_ms_to_ticks(1U), 0U);
        return;
    }

    if (s_state.mode == RGB_MODE_PWM) {
        /* PWM sine-wave advance */
        s_state.pwm.step_index++;
        uint8_t sine_val = s_sine_lut[s_state.pwm.step_index];
        s_state.pwm.r_duty = (s_state.pwm.r_duty == 0U) ? 0U : sine_val;
        s_state.pwm.g_duty = (s_state.pwm.g_duty == 0U) ? 0U : sine_val;
        s_state.pwm.b_duty = (s_state.pwm.b_duty == 0U) ? 0U : sine_val;
        (void)rgb_apply_pwm_duty(s_state.pwm.r_duty, s_state.pwm.g_duty, s_state.pwm.b_duty);
    } else {
        /* Pattern step advance */
        if (s_state.active_pattern >= RGB_PATTERN_COUNT) {
            s_state.timer_running = false;
            (void)xSemaphoreGive(s_state.lock);
            return;
        }

        const RgbPatternDef_t *pattern = &s_patterns[s_state.active_pattern];
        if (pattern->step_count == 0U || pattern->steps == NULL) {
            (void)xSemaphoreGive(s_state.lock);
            return;
        }

        if (s_state.step_index + 1U < pattern->step_count) {
            s_state.step_index++;
        } else if (pattern->loop) {
            s_state.step_index = 0U;
        } else {
            s_state.timer_running = false;
            (void)xSemaphoreGive(s_state.lock);
            return;
        }
        (void)rgb_apply_step_locked(pattern);
        (void)rgb_arm_timer_locked(pattern);
    }

    (void)xSemaphoreGive(s_state.lock);
}


CeePewErr_t rgb_init(void){
    CEEPEW_ASSERT(GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_RED) && GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_GREEN) && GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_BLUE), CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_GREEN && CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_BLUE && CEEPEW_PIN_RGB_GREEN != CEEPEW_PIN_RGB_BLUE, CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(RGB_PATTERN_COUNT == 18U, CEEPEW_ERR_INTERNAL);

    if (s_state.initialised) {
        CEEPEW_ASSERT(s_state.lock != NULL && s_state.timer != NULL, CEEPEW_ERR_INTERNAL);
        return CEEPEW_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CEEPEW_PIN_RGB_RED) | (1ULL << CEEPEW_PIN_RGB_GREEN) | (1ULL << CEEPEW_PIN_RGB_BLUE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t gpio_err = gpio_config(&cfg);
    if (gpio_err != ESP_OK) { return CEEPEW_ERR_HW;}

    /* Initialize LEDC for PWM mode */
    CeePewErr_t ledc_err = rgb_ledc_init();
    if (ledc_err != CEEPEW_OK) { return ledc_err; }

    if (s_state.lock == NULL) {
        s_state.lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        CEEPEW_ASSERT(s_state.lock != NULL, CEEPEW_ERR_ALLOC);
    }

    if (s_state.timer == NULL) {
        s_state.timer = xTimerCreateStatic("rgb", rgb_ms_to_ticks(1U), pdFALSE, NULL, rgb_unified_timer_cb, &s_timer_buf);
        CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_ALLOC);
    }

    s_state.initialised = true;
    s_state.active_pattern = RGB_OFF;
    s_state.step_index = 0U;
    s_state.timer_running = false;
    s_state.mode = RGB_MODE_PATTERN;

    const RgbPatternDef_t *pattern = &s_patterns[RGB_OFF];
    CeePewErr_t err = rgb_apply_step_locked(pattern);
    if (err != CEEPEW_OK) {
        s_state.initialised = false;
        return err;
    }

    err = rgb_arm_timer_locked(pattern);
    if (err != CEEPEW_OK) {
        s_state.initialised = false;
        return err;
    }
    return CEEPEW_OK;
}

CeePewErr_t rgb_set_pattern(RgbPattern_t pattern)
{
    CEEPEW_ASSERT(pattern < RGB_PATTERN_COUNT, CEEPEW_ERR_BOUNDS);

    if (!s_state.initialised) {
        CeePewErr_t err = rgb_init();
        if (err != CEEPEW_OK) {
            return err;
        }
    }

    CEEPEW_ASSERT(s_state.lock != NULL && s_state.timer != NULL, CEEPEW_ERR_INTERNAL);

    if (xSemaphoreTake(s_state.lock, portMAX_DELAY) != pdTRUE) { return CEEPEW_ERR_TIMEOUT; }

    s_state.active_pattern = pattern;
    s_state.step_index = 0U;

    const RgbPatternDef_t *pattern_def = &s_patterns[s_state.active_pattern];

    /* Check if this is a PWM pulse pattern (step has UINT32_MAX duration) */
    bool is_pwm_pattern = (pattern_def->step_count == 1U && 
                           pattern_def->steps != NULL &&
                           pattern_def->steps[0].duration_ms == UINT32_MAX);

    CeePewErr_t err = CEEPEW_OK;

    if (is_pwm_pattern) {
        s_state.mode = RGB_MODE_PWM;
        s_state.pwm.r_duty = (pattern_def->steps[0].r == 1U) ? 255U : 0U;
        s_state.pwm.g_duty = (pattern_def->steps[0].g == 1U) ? 255U : 0U;
        s_state.pwm.b_duty = (pattern_def->steps[0].b == 1U) ? 255U : 0U;
        s_state.pwm.step_index = 0U;

        /* Apply initial PWM duty */
        err = rgb_apply_pwm_duty(s_state.pwm.r_duty, s_state.pwm.g_duty, s_state.pwm.b_duty);
        if (err == CEEPEW_OK && s_state.timer != NULL) {
            /* Repurpose the single static timer at 4 ms for PWM stepping */
            (void)xTimerStop(s_state.timer, 0U);
            (void)xTimerChangePeriod(s_state.timer, pdMS_TO_TICKS(4U), 0U);
            /* Switch timer to auto-reload for continuous PWM */
            vTimerSetReloadMode(s_state.timer, pdTRUE);
            (void)xTimerStart(s_state.timer, 0U);
            s_state.timer_running = true;
        }
    } else {
        s_state.mode = RGB_MODE_PATTERN;

        /* Restore one-shot mode for pattern stepping */
        (void)xTimerStop(s_state.timer, 0U);
        vTimerSetReloadMode(s_state.timer, pdFALSE);

        err = rgb_apply_step_locked(pattern_def);
        if (err == CEEPEW_OK) {
            err = rgb_arm_timer_locked(pattern_def);
        }
    }

    (void)xSemaphoreGive(s_state.lock);
    return err;
}

CeePewErr_t rgb_task(void) {
    if (!s_state.initialised) {
        CeePewErr_t err = rgb_init();
        if (err != CEEPEW_OK) {
            return err;
        }
    }
    CEEPEW_ASSERT(s_state.lock != NULL, CEEPEW_ERR_INTERNAL);
    return CEEPEW_OK;
}

CeePewErr_t rgb_set_pwm_mode(uint8_t r_intensity, uint8_t g_intensity, uint8_t b_intensity)
{
    /* Set LED to smooth PWM pulsing mode with specified color intensities (0-255).
       Intensity 0 = off, 255 = full brightness. Sine-wave modulation drives smooth 1 Hz pulse. */
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(s_state.lock != NULL && s_state.timer != NULL, CEEPEW_ERR_INTERNAL);

    if (xSemaphoreTake(s_state.lock, portMAX_DELAY) != pdTRUE) { 
        return CEEPEW_ERR_TIMEOUT; 
    }

    /* Switch to PWM mode */
    s_state.mode = RGB_MODE_PWM;
    s_state.pwm.r_duty = r_intensity;
    s_state.pwm.g_duty = g_intensity;
    s_state.pwm.b_duty = b_intensity;
    s_state.pwm.step_index = 0U;

    /* Apply initial PWM duty cycle */
    CeePewErr_t err = rgb_apply_pwm_duty(s_state.pwm.r_duty, s_state.pwm.g_duty, s_state.pwm.b_duty);
    if (err == CEEPEW_OK && s_state.timer != NULL) {
        /* Repurpose the single static timer at 4 ms for PWM stepping */
        (void)xTimerStop(s_state.timer, 0U);
        (void)xTimerChangePeriod(s_state.timer, pdMS_TO_TICKS(4U), 0U);
        /* Switch timer to auto-reload for continuous PWM */
        vTimerSetReloadMode(s_state.timer, pdTRUE);
        (void)xTimerStart(s_state.timer, 0U);
        s_state.timer_running = true;
    }

    (void)xSemaphoreGive(s_state.lock);
    return err;
}

CeePewErr_t rgb_pulse(uint8_t r_intensity, uint8_t g_intensity, uint8_t b_intensity)
{
    /* High-level API: smoothly pulse the LED with specified color at 1 Hz.
       Equivalent to: rgb_set_pwm_mode(r, g, b) + start PWM sine-wave animation. */
    return rgb_set_pwm_mode(r_intensity, g_intensity, b_intensity);
}
