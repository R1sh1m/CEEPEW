#include "hal_rgb.h"

#include "../../main/ceepew_config.h"
#include "../../components/hal/hal_pins.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

typedef struct __attribute__((packed))
{
    uint16_t r_on_ms;
    uint16_t g_on_ms;
    uint16_t b_on_ms;
    uint16_t period_ms;
    uint8_t repeat_count;
} RgbFrame_t;

typedef struct
{
    bool initialised;
    bool timer_running;
    RgbPattern_t active_pattern;
    uint8_t frame_index;
    uint8_t repeats_left;
    uint16_t elapsed_ms;
    uint16_t scheduled_ms;
    TimerHandle_t timer;
} RgbState_t;

static StaticTimer_t s_timer_buffer;
static RgbState_t s_state = {
    .initialised = false,
    .timer_running = false,
    .active_pattern = RGB_OFF,
    .frame_index = 0U,
    .repeats_left = 0U,
    .elapsed_ms = 0U,
    .scheduled_ms = 0U,
    .timer = NULL
};

static const RgbFrame_t s_rgb_off[] = {
    {0U, 0U, 0U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_red[] = {
    {1000U, 0U, 0U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_green[] = {
    {0U, 1000U, 0U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_blue[] = {
    {0U, 0U, 1000U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_yellow[] = {
    {1000U, 1000U, 0U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_cyan[] = {
    {0U, 1000U, 1000U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_magenta[] = {
    {1000U, 0U, 1000U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_white[] = {
    {1000U, 1000U, 1000U, 1000U, 0U}
};

static const RgbFrame_t s_rgb_red_blink[] = {
    {100U, 0U, 0U, 300U, 2U},
    {0U, 0U, 0U, 300U, 2U}
};

static const RgbFrame_t s_rgb_green_blink[] = {
    {0U, 100U, 0U, 300U, 2U},
    {0U, 0U, 0U, 300U, 2U}
};

static const RgbFrame_t s_rgb_blue_blink[] = {
    {0U, 0U, 100U, 300U, 2U},
    {0U, 0U, 0U, 300U, 2U}
};

static const RgbFrame_t s_rgb_amber_pulse[] = {
    {180U, 70U, 0U, 400U, 3U},
    {0U, 0U, 0U, 400U, 1U}
};

static const RgbFrame_t s_rgb_cyan_pulse[] = {
    {0U, 180U, 180U, 400U, 3U},
    {0U, 0U, 0U, 400U, 1U}
};

static const RgbFrame_t s_rgb_rainbow_cycle[] = {
    {800U, 0U, 0U, 250U, 1U},
    {0U, 800U, 0U, 250U, 1U},
    {0U, 0U, 800U, 250U, 1U}
};

static const RgbFrame_t s_rgb_heartbeat[] = {
    {160U, 0U, 0U, 240U, 1U},
    {0U, 0U, 0U, 120U, 1U},
    {160U, 0U, 0U, 240U, 1U},
    {0U, 0U, 0U, 700U, 1U}
};

typedef struct
{
    const RgbFrame_t *frames;
    uint8_t frame_count;
} RgbPatternDef_t;

static const RgbPatternDef_t s_pattern_defs[RGB_PATTERN_COUNT] = {
    {s_rgb_off, 1U},
    {s_rgb_red, 1U},
    {s_rgb_green, 1U},
    {s_rgb_blue, 1U},
    {s_rgb_yellow, 1U},
    {s_rgb_cyan, 1U},
    {s_rgb_magenta, 1U},
    {s_rgb_white, 1U},
    {s_rgb_red_blink, 2U},
    {s_rgb_green_blink, 2U},
    {s_rgb_blue_blink, 2U},
    {s_rgb_amber_pulse, 2U},
    {s_rgb_cyan_pulse, 2U},
    {s_rgb_rainbow_cycle, 3U},
    {s_rgb_heartbeat, 4U}
};

static void rgb_apply_levels(bool red_on, bool green_on, bool blue_on)
{
    gpio_set_level(CEEPEW_PIN_RGB_RED, red_on ? 1 : 0);
    gpio_set_level(CEEPEW_PIN_RGB_GREEN, green_on ? 1 : 0);
    gpio_set_level(CEEPEW_PIN_RGB_BLUE, blue_on ? 1 : 0);
}

static TickType_t rgb_ms_to_ticks(uint16_t delay_ms)
{
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)delay_ms);
    return (ticks == 0U) ? 1U : ticks;
}

static const RgbFrame_t *rgb_current_frame(void)
{
    const RgbPatternDef_t *pattern = &s_pattern_defs[s_state.active_pattern];
    return &pattern->frames[s_state.frame_index];
}

static void rgb_stop_timer(void)
{
    if (s_state.timer != NULL)
    {
        (void)xTimerStop(s_state.timer, 0U);
    }
    s_state.timer_running = false;
    s_state.scheduled_ms = 0U;
}

static CeePewErr_t rgb_arm_timer(const RgbFrame_t *frame, uint16_t elapsed_ms)
{
    CEEPEW_ASSERT(frame != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_INTERNAL);

    if (frame->repeat_count == 0U)
    {
        rgb_stop_timer();
        return CEEPEW_OK;
    }

    uint16_t next_ms = frame->period_ms;
    if (elapsed_ms < frame->r_on_ms)
    {
        uint16_t delta = (uint16_t)(frame->r_on_ms - elapsed_ms);
        if (delta < next_ms)
        {
            next_ms = delta;
        }
    }
    if (elapsed_ms < frame->g_on_ms)
    {
        uint16_t delta = (uint16_t)(frame->g_on_ms - elapsed_ms);
        if (delta < next_ms)
        {
            next_ms = delta;
        }
    }
    if (elapsed_ms < frame->b_on_ms)
    {
        uint16_t delta = (uint16_t)(frame->b_on_ms - elapsed_ms);
        if (delta < next_ms)
        {
            next_ms = delta;
        }
    }
    if (next_ms == 0U)
    {
        next_ms = 1U;
    }

    s_state.scheduled_ms = next_ms;
    s_state.timer_running = true;
    (void)xTimerChangePeriod(s_state.timer, rgb_ms_to_ticks(next_ms), 0U);
    return CEEPEW_OK;
}

static CeePewErr_t rgb_apply_active_frame(void)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_INTERNAL);

    const RgbFrame_t *frame = rgb_current_frame();
    bool red_on = (0U < frame->r_on_ms);
    bool green_on = (0U < frame->g_on_ms);
    bool blue_on = (0U < frame->b_on_ms);
    rgb_apply_levels(red_on, green_on, blue_on);

    s_state.elapsed_ms = 0U;
    s_state.repeats_left = frame->repeat_count;
    return rgb_arm_timer(frame, 0U);
}

static void rgb_advance_frame(void)
{
    const RgbPatternDef_t *pattern = &s_pattern_defs[s_state.active_pattern];
    if (s_state.frame_index + 1U < pattern->frame_count)
    {
        s_state.frame_index++;
    }
    else
    {
        s_state.frame_index = 0U;
    }
}

static void rgb_timer_cb(TimerHandle_t timer)
{
    CEEPEW_ASSERT_VOID(timer != NULL);
    CEEPEW_ASSERT_VOID(s_state.initialised);

    const RgbFrame_t *frame = rgb_current_frame();
    if (s_state.scheduled_ms > 0U)
    {
        s_state.elapsed_ms = (uint16_t)(s_state.elapsed_ms + s_state.scheduled_ms);
    }

    if (frame->repeat_count == 0U)
    {
        rgb_apply_levels(frame->r_on_ms > 0U, frame->g_on_ms > 0U, frame->b_on_ms > 0U);
        rgb_stop_timer();
        return;
    }

    if (s_state.elapsed_ms >= frame->period_ms)
    {
        if (s_state.repeats_left > 1U)
        {
            s_state.repeats_left--;
            s_state.elapsed_ms = 0U;
        }
        else
        {
            rgb_advance_frame();
            frame = rgb_current_frame();
            s_state.repeats_left = frame->repeat_count;
            s_state.elapsed_ms = 0U;
        }
    }

    rgb_apply_levels(s_state.elapsed_ms < frame->r_on_ms,
                     s_state.elapsed_ms < frame->g_on_ms,
                     s_state.elapsed_ms < frame->b_on_ms);
    (void)rgb_arm_timer(frame, s_state.elapsed_ms);
}

CeePewErr_t rgb_init(void)
{
    CEEPEW_ASSERT(gpio_is_valid_output_gpio(CEEPEW_PIN_RGB_RED) &&
                  gpio_is_valid_output_gpio(CEEPEW_PIN_RGB_GREEN) &&
                  gpio_is_valid_output_gpio(CEEPEW_PIN_RGB_BLUE), CEEPEW_ERR_PINS);
    CEEPEW_ASSERT(CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_GREEN &&
                  CEEPEW_PIN_RGB_RED != CEEPEW_PIN_RGB_BLUE &&
                  CEEPEW_PIN_RGB_GREEN != CEEPEW_PIN_RGB_BLUE, CEEPEW_ERR_PINS);

    CEEPEW_ASSERT(!s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(RGB_PATTERN_COUNT == 15U, CEEPEW_ERR_INTERNAL);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CEEPEW_PIN_RGB_RED) |
                        (1ULL << CEEPEW_PIN_RGB_GREEN) |
                        (1ULL << CEEPEW_PIN_RGB_BLUE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t rc = gpio_config(&cfg);
    if (rc != ESP_OK)
    {
        return CEEPEW_ERR_HW;
    }

    rgb_apply_levels(false, false, false);

    s_state.timer = xTimerCreateStatic("rgb",
                                       rgb_ms_to_ticks(1U),
                                       pdFALSE,
                                       NULL,
                                       rgb_timer_cb,
                                       &s_timer_buffer);
    if (s_state.timer == NULL)
    {
        return CEEPEW_ERR_ALLOC;
    }

    s_state.initialised = true;
    s_state.active_pattern = RGB_OFF;
    s_state.frame_index = 0U;
    s_state.repeats_left = 0U;
    s_state.elapsed_ms = 0U;
    s_state.scheduled_ms = 0U;
    return rgb_apply_active_frame();
}

CeePewErr_t rgb_set_pattern(RgbPattern_t pattern)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(pattern < RGB_PATTERN_COUNT, CEEPEW_ERR_BOUNDS);

    s_state.active_pattern = pattern;
    s_state.frame_index = 0U;
    return rgb_apply_active_frame();
}

CeePewErr_t rgb_task(void)
{
    CEEPEW_ASSERT(s_state.initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(s_state.timer != NULL, CEEPEW_ERR_INTERNAL);

    if (!s_state.timer_running)
    {
        const RgbFrame_t *frame = rgb_current_frame();
        if (frame->repeat_count != 0U)
        {
            return rgb_apply_active_frame();
        }
    }

    return CEEPEW_OK;
}
