#include "hal_rgb.h"

#include "../../main/ceepew_config.h"
#include "hal_pins.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include <limits.h>

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t duration_ms;
} RgbStep_t;

typedef struct
{
    const RgbStep_t *steps;
    uint8_t step_count;
    bool loop;
} RgbPatternDef_t;

typedef struct
{
    bool initialised;
    bool timer_running;
    RgbPattern_t active_pattern;
    uint8_t step_index;
    TimerHandle_t timer;
    SemaphoreHandle_t lock;
} RgbState_t;

static StaticTimer_t s_timer_buf;
static StaticSemaphore_t s_lock_buf;
static RgbState_t s_state = {
    .initialised = false,
    .timer_running = false,
    .active_pattern = RGB_OFF,
    .step_index = 0U,
    .timer = NULL,
    .lock = NULL};

static const RgbStep_t s_steps_off[] = {
    {0U, 0U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_red[] = {
    {1U, 0U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_green[] = {
    {0U, 1U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_blue[] = {
    {0U, 0U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_yellow[] = {
    {1U, 1U, 0U, UINT32_MAX}};
static const RgbStep_t s_steps_cyan[] = {
    {0U, 1U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_magenta[] = {
    {1U, 0U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_white[] = {
    {1U, 1U, 1U, UINT32_MAX}};
static const RgbStep_t s_steps_red_blink[] = {
    {1U, 0U, 0U, 300U},
    {0U, 0U, 0U, 300U}};
static const RgbStep_t s_steps_green_blink[] = {
    {0U, 1U, 0U, 300U},
    {0U, 0U, 0U, 300U}};
static const RgbStep_t s_steps_blue_blink[] = {
    {0U, 0U, 1U, 300U},
    {0U, 0U, 0U, 300U}};
static const RgbStep_t s_steps_amber_pulse[] = {
    {1U, 1U, 0U, 500U},
    {0U, 0U, 0U, 500U}};
static const RgbStep_t s_steps_cyan_pulse[] = {
    {0U, 1U, 1U, 500U},
    {0U, 0U, 0U, 500U}};
static const RgbStep_t s_steps_rainbow_cycle[] = {
    {1U, 0U, 0U, 250U},
    {0U, 1U, 0U, 250U},
    {0U, 0U, 1U, 250U}};
static const RgbStep_t s_steps_heartbeat[] = {
    {1U, 0U, 0U, 140U},
    {0U, 0U, 0U, 120U},
    {1U, 0U, 0U, 140U},
    {0U, 0U, 0U, 700U}};

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
    {s_steps_amber_pulse, 2U, true},
    {s_steps_cyan_pulse, 2U, true},
    {s_steps_rainbow_cycle, 3U, true},
    {s_steps_heartbeat, 4U, true}};

static TickType_t rgb_ms_to_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0U)
    {
        ticks = 1U;
    }
    return ticks;
}

static void rgb_apply_levels(uint8_t r, uint8_t g, uint8_t b)
{
    CEEPEW_ASSERT_VOID(r <= 1U && g <= 1U && b <= 1U);
    CEEPEW_ASSERT_VOID(s_state.initialised);

    gpio_set_level(CEEPEW_PIN_RGB_RED, (int)r);
    gpio_set_level(CEEPEW_PIN_RGB_GREEN, (int)g);
    gpio_set_level(CEEPEW_PIN_RGB_BLUE, (int)b);
}

static CeePewErr_t rgb_apply_step_locked(const RgbPatternDef_t *pattern)
{
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

static CeePewErr_t rgb_arm_timer_locked(const RgbPatternDef_t *pattern)
{
    CEEPEW_ASSERT(pattern != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(s_state.step_index < pattern->step_count, CEEPEW_ERR_BOUNDS);

    const RgbStep_t *step = &pattern->steps[s_state.step_index];
    CEEPEW_ASSERT(step->duration_ms > 0U, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(step->duration_ms == UINT32_MAX || step->duration_ms <= 60000U, CEEPEW_ERR_BOUNDS);

    if (step->duration_ms == UINT32_MAX)
    {
        (void)xTimerStop(s_state.timer, 0U);
        s_state.timer_running = false;
        return CEEPEW_OK;
    }

    BaseType_t changed = xTimerChangePeriod(s_state.timer, rgb_ms_to_ticks(step->duration_ms), 0U);
    CEEPEW_ASSERT(changed == pdPASS, CEEPEW_ERR_HW);
    s_state.timer_running = true;
    return CEEPEW_OK;
}

static void rgb_timer_cb(TimerHandle_t timer)
{
    CEEPEW_ASSERT_VOID(timer != NULL);
    CEEPEW_ASSERT_VOID(s_state.lock != NULL);
    CEEPEW_ASSERT_VOID(s_state.initialised);

    if (xSemaphoreTake(s_state.lock, 0U) != pdTRUE)
    {
        (void)xTimerChangePeriod(timer, rgb_ms_to_ticks(1U), 0U);
        return;
    }

    if (s_state.active_pattern >= RGB_PATTERN_COUNT)
    {
        s_state.timer_running = false;
        (void)xSemaphoreGive(s_state.lock);
        return;
    }

    const RgbPatternDef_t *pattern = &s_patterns[s_state.active_pattern];
    if (pattern->step_count == 0U || pattern->steps == NULL)
    {
        (void)xSemaphoreGive(s_state.lock);
        return;
    }

    if (s_state.step_index + 1U < pattern->step_count)
    {
        s_state.step_index++;
    }
    else if (pattern->loop)
    {
        s_state.step_index = 0U;
    }
    else
    {
        s_state.timer_running = false;
        (void)xSemaphoreGive(s_state.lock);
        return;
    }

    (void)rgb_apply_step_locked(pattern);
    (void)rgb_arm_timer_locked(pattern);
    (void)xSemaphoreGive(s_state.lock);
}

CeePewErr_t rgb_init(void)
{
    CEEPEW_ASSERT(GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_RED) &&
                      GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_GREEN) &&
                      GPIO_IS_VALID_OUTPUT_GPIO(CEEPEW_PIN_RGB_BLUE),
                  CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_GREEN &&
                      CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_BLUE &&
                      CEEPEW_PIN_RGB_GREEN != CEEPEW_PIN_RGB_BLUE,
                  CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(!s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(RGB_PATTERN_COUNT == 15U, CEEPEW_ERR_INTERNAL);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CEEPEW_PIN_RGB_RED) |
                        (1ULL << CEEPEW_PIN_RGB_GREEN) |
                        (1ULL << CEEPEW_PIN_RGB_BLUE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    esp_err_t gpio_err = gpio_config(&cfg);
    if (gpio_err != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }

    s_state.lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    CEEPEW_ASSERT(s_state.lock != NULL, CEEPEW_ERR_ALLOC);

    s_state.timer = xTimerCreateStatic("rgb",
                                       rgb_ms_to_ticks(1U),
                                       pdFALSE,
                                       NULL,
                                       rgb_timer_cb,
                                       &s_timer_buf);
    CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_ALLOC);

    s_state.initialised = true;
    s_state.active_pattern = RGB_OFF;
    s_state.step_index = 0U;
    s_state.timer_running = false;

    const RgbPatternDef_t *pattern = &s_patterns[RGB_OFF];
    CeePewErr_t err = rgb_apply_step_locked(pattern);
    if (err != CEEPEW_OK)
    {
        return err;
    }
    return rgb_arm_timer_locked(pattern);
}

CeePewErr_t rgb_set_pattern(RgbPattern_t pattern)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_state.lock != NULL && s_state.timer != NULL, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(pattern < RGB_PATTERN_COUNT, CEEPEW_ERR_BOUNDS);

    if (xSemaphoreTake(s_state.lock, portMAX_DELAY) != pdTRUE)
    {
        return CEEPEW_ERR_TIMEOUT;
    }

    s_state.active_pattern = pattern;
    s_state.step_index = 0U;

    const RgbPatternDef_t *pattern_def = &s_patterns[s_state.active_pattern];
    CeePewErr_t err = rgb_apply_step_locked(pattern_def);
    if (err == CEEPEW_OK)
    {
        err = rgb_arm_timer_locked(pattern_def);
    }

    (void)xSemaphoreGive(s_state.lock);
    return err;
}

CeePewErr_t rgb_task(void)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_state.lock != NULL, CEEPEW_ERR_INTERNAL);
    return CEEPEW_OK;
}
