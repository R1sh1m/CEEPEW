/* tests/host/test_ui_navigator.c
 * Host unit tests for ui_navigator: hysteresis, velocity ramp, edge dwell, clamp-on-shrink.
 */

#include "ui_navigator.h"
#include <stdio.h>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            return 1; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(val, expected, msg) \
    do { \
        if ((val) != (expected)) { \
            printf("FAIL: %s (got %u, expected %u)\n", msg, (unsigned)(val), (unsigned)(expected)); \
            return 1; \
        } \
    } while (0)

static int test_init_and_get(void)
{
    NavCtx_t nav;
    CeePewErr_t err = ui_nav_init(&nav, 66U, 5U);
    TEST_ASSERT(err == CEEPEW_OK, "init failed");
    TEST_ASSERT_EQ(nav.cursor, 5U, "cursor init");
    TEST_ASSERT_EQ(nav.count, 66U, "count init");
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 5U, "get_index");
    printf("test_init_and_get: OK\n");
    return 0;
}

static int test_init_invalid_count(void)
{
    NavCtx_t nav;
    CeePewErr_t err = ui_nav_init(&nav, 0U, 0U);
    TEST_ASSERT(err == CEEPEW_ERR_PARAM, "init should reject count=0");
    printf("test_init_invalid_count: OK\n");
    return 0;
}

static int test_reset_reanchors(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 10U);
    (void)ui_nav_reset(&nav, 128U);
    TEST_ASSERT_EQ(nav.anchor, 128U, "anchor reset");
    TEST_ASSERT_EQ(nav.cursor, 10U, "cursor unchanged");
    TEST_ASSERT_EQ(nav.travel_accum, 0, "travel_accum reset");
    TEST_ASSERT_EQ(nav.edge_repeat_start_ms, 0U, "edge timer reset");
    printf("test_reset_reanchors: OK\n");
    return 0;
}

static int test_hysteresis_no_drift(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 0U);
    (void)ui_nav_reset(&nav, 128U);

    uint32_t now = 1000U;
    /* First call seeds anchor at 130; subsequent ±2 jitter stays under THRESH. */
    (void)ui_nav_update(&nav, 130U, now);
    for (int i = 0; i < 20; i++) {
        uint8_t pot = (i % 2 == 0) ? 131U : 129U;  /* ±1 jitter around 130 */
        (void)ui_nav_update(&nav, pot, now + (i + 1) * 16U);
    }
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 0U, "no drift below threshold");
    printf("test_hysteresis_no_drift: OK\n");
    return 0;
}

static int test_static_input_no_flip(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 10U);
    (void)ui_nav_reset(&nav, 200U);

    uint32_t now = 1000U;
    /* Seed anchor. */
    (void)ui_nav_update(&nav, 200U, now);
    /* Feed 60 identical frames — cursor must stay frozen. */
    for (int i = 1; i <= 60; i++) {
        (void)ui_nav_update(&nav, 200U, now + i * 16U);
    }
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 10U, "static input never moves cursor");
    printf("test_static_input_no_flip: OK\n");
    return 0;
}

static int test_slow_drift_two_units(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 0U);
    (void)ui_nav_reset(&nav, 100U);

    uint32_t now = 1000U;
    (void)ui_nav_update(&nav, 100U, now);
    /* Two units per frame (|dv|<=2) activates slow-speed precision threshold (THRESH*2 = 8).
     * 10 frames of 2 units = 20 total travel → 20 / 8 = 2 steps (fine precision). */
    for (int i = 0; i < 10; i++) {
        (void)ui_nav_update(&nav, (uint8_t)(102U + i * 2U), now + (i + 1) * 16U);
    }
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 2U, "slow 2/frame drift = 1 step per 4 frames (fine precision)");
    printf("test_slow_drift_two_units: OK\n");
    return 0;
}

static int test_one_to_one_stepping(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 0U);
    (void)ui_nav_reset(&nav, 128U);

    uint32_t now = 1000U;
    /* First call seeds anchor; subsequent calls step. */
    (void)ui_nav_update(&nav, 132U, now);
    for (int i = 0; i < 10; i++) {
        (void)ui_nav_update(&nav, (uint8_t)(136U + i * 4U), now + (i + 1) * 16U);
    }
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 10U, "1:1 stepping");
    printf("test_one_to_one_stepping: OK\n");
    return 0;
}

static int test_velocity_ramp(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 0U);
    (void)ui_nav_reset(&nav, 100U);

    uint32_t now = 1000U;
    for (int i = 0; i < 5; i++) {
        (void)ui_nav_update(&nav, (uint8_t)(100U + (i + 1) * 24U), now + i * 16U);
    }
    uint8_t idx = ui_nav_get_index(&nav);
    TEST_ASSERT_EQ(idx, 65U, "velocity ramp hits upper bound");
    printf("test_velocity_ramp: OK (idx=%u)\n", (unsigned)idx);
    return 0;
}

static int test_turbo_velocity(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 0U);
    (void)ui_nav_reset(&nav, 50U);

    uint32_t now = 1000U;
    for (int i = 0; i < 3; i++) {
        (void)ui_nav_update(&nav, (uint8_t)(50U + (i + 1) * 60U), now + i * 16U);
    }
    uint8_t idx = ui_nav_get_index(&nav);
    TEST_ASSERT_EQ(idx, 65U, "turbo velocity hits upper bound");
    printf("test_turbo_velocity: OK (idx=%u)\n", (unsigned)idx);
    return 0;
}

static int test_edge_dwell_auto_repeat(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 0U);
    (void)ui_nav_reset(&nav, 2U);

    uint32_t now = 1000U;
    for (int i = 0; i < 40; i++) {
        (void)ui_nav_update(&nav, 2U, now + i * 16U);
    }
    uint8_t idx = ui_nav_get_index(&nav);
    TEST_ASSERT_EQ(idx, 0U, "left edge dwell stays at 0");
    printf("test_edge_dwell_auto_repeat: OK (idx=%u)\n", (unsigned)idx);
    return 0;
}

static int test_edge_dwell_right(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 0U);
    (void)ui_nav_reset(&nav, 253U);

    uint32_t now = 1000U;
    for (int i = 0; i < 40; i++) {
        (void)ui_nav_update(&nav, 253U, now + i * 16U);
    }
    uint8_t idx = ui_nav_get_index(&nav);
    TEST_ASSERT_EQ(idx, 65U, "right edge dwell hits upper bound");
    printf("test_edge_dwell_right: OK (idx=%u)\n", (unsigned)idx);
    return 0;
}

static int test_set_count_clamps(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 50U);
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 50U, "initial cursor");

    (void)ui_nav_set_count(&nav, 10U);
    TEST_ASSERT_EQ(nav.count, 10U, "count updated");
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 9U, "cursor clamped to 9");

    (void)ui_nav_set_count(&nav, 66U);
    TEST_ASSERT_EQ(nav.count, 66U, "count restored");
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 9U, "cursor stays at 9");
    printf("test_set_count_clamps: OK\n");
    return 0;
}

static int test_reverse_no_bounce(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 10U);
    (void)ui_nav_reset(&nav, 128U);

    uint32_t now = 1000U;
    /* Seed anchor first. */
    (void)ui_nav_update(&nav, 132U, now);
    /* Now step right. */
    (void)ui_nav_update(&nav, 136U, now + 16U);
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 11U, "first step right");

    /* Small reverse (below threshold) should NOT step back. */
    (void)ui_nav_update(&nav, 134U, now + 32U);  /* dv = -2 from anchor=136 */
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 11U, "tiny reverse no bounce");

    /* Large reverse sweep from 134 to 124: 10 units. Prior travel=-2 → total -12 = 3 crossings * vel_mult(10/6=2) = 6 steps back. */
    (void)ui_nav_update(&nav, 124U, now + 48U);
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 5U, "large reverse sweeps back 6");

    printf("test_reverse_no_bounce: OK\n");
    return 0;
}

static int test_post_selection_hold(void)
{
    NavCtx_t nav;
    (void)ui_nav_init(&nav, 66U, 15U); /* start at 'P' (idx 15) */
    (void)ui_nav_reset(&nav, 100U);

    uint32_t now = 1000U;
    /* User clicks character at t=1000ms, triggering 800ms hold */
    CeePewErr_t err = ui_nav_hold(&nav, 100U, 800U, now);
    TEST_ASSERT(err == CEEPEW_OK, "ui_nav_hold failed");

    /* During the 800ms hold (t=1000..1800), mechanical jitter/pot changes must NOT move cursor */
    for (int i = 1; i <= 20; i++) {
        uint8_t jitter_pot = (uint8_t)(100U + (i % 5) * 4U);
        (void)ui_nav_update(&nav, jitter_pot, now + i * 30U);
        TEST_ASSERT_EQ(ui_nav_get_index(&nav), 15U, "cursor held during selection hold window");
    }

    /* Once hold expires (t >= 1800ms), stepping resumes normally from current anchor without jump */
    (void)ui_nav_update(&nav, 120U, now + 850U); /* t=1850ms, seeds anchor at 120 */
    (void)ui_nav_update(&nav, 124U, now + 880U); /* +4 from anchor -> step 1 */
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 16U, "navigation resumes cleanly after hold");

    printf("test_post_selection_hold: OK\n");
    return 0;
}

static int test_button_press_freeze_and_reanchor(void)
{
    NavCtx_t nav;
    /* User navigates to 'E' (idx 4) with pot at 100 */
    (void)ui_nav_init(&nav, 66U, 4U);
    (void)ui_nav_reset(&nav, 100U);
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 4U, "initial position at 'E'");

    uint32_t now = 1000U;
    /* User pushes button down on stacked hardware: button_pressed becomes true */
    /* While button is held (t=1000..1150ms), mechanical downward pressure shifts pot from 100 to 125 */
    for (int i = 1; i <= 5; i++) {
        uint8_t shifted_pot = (uint8_t)(100U + i * 5U); /* drifts up to 125 */
        /* UI loop freezes cursor and re-anchors to shifted_pot */
        (void)ui_nav_reset(&nav, shifted_pot);
        TEST_ASSERT_EQ(ui_nav_get_index(&nav), 4U, "cursor remains solidly frozen at 'E' during press");
    }

    /* User releases button at t=1150ms: committed character is 4 ('E') */
    uint32_t release_ms = 1150U;
    (void)ui_nav_hold(&nav, 125U, 800U, release_ms);

    /* Post-selection hold absorbs release recoil (pot 125 -> 130) */
    for (int i = 1; i <= 10; i++) {
        (void)ui_nav_update(&nav, (uint8_t)(125U + (i % 3) * 2U), release_ms + i * 30U);
        TEST_ASSERT_EQ(ui_nav_get_index(&nav), 4U, "cursor held post-release");
    }

    /* Once hold window expires (t=2000ms), navigation resumes from resting pot without skipping */
    (void)ui_nav_update(&nav, 130U, release_ms + 850U); /* seeds anchor at 130 */
    (void)ui_nav_update(&nav, 134U, release_ms + 880U); /* +4 from anchor -> step 1 */
    TEST_ASSERT_EQ(ui_nav_get_index(&nav), 5U, "stepping resumes smoothly after hold to next item 'F'");

    printf("test_button_press_freeze_and_reanchor: OK\n");
    return 0;
}

int main(void)
{
    int failed = 0;

    failed += test_init_and_get();
    failed += test_init_invalid_count();
    failed += test_reset_reanchors();
    failed += test_hysteresis_no_drift();
    failed += test_static_input_no_flip();
    failed += test_slow_drift_two_units();
    failed += test_one_to_one_stepping();
    failed += test_velocity_ramp();
    failed += test_turbo_velocity();
    failed += test_edge_dwell_auto_repeat();
    failed += test_edge_dwell_right();
    failed += test_set_count_clamps();
    failed += test_reverse_no_bounce();
    failed += test_post_selection_hold();
    failed += test_button_press_freeze_and_reanchor();

    if (failed == 0) {
        printf("\n=== ALL ui_navigator TESTS PASSED ===\n");
        return 0;
    } else {
        printf("\n=== %d TEST(S) FAILED ===\n", failed);
        return 1;
    }
}