#include "ceepew_pipeline.h"
#include "ceepew_region.h"
#include "ceepew_assert.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "PIPELINE-MEM-TEST";

CeePewErr_t stage_fragment(Region_t *region, const uint8_t *in, uint16_t in_len, uint8_t **out, uint16_t *out_len, void *ctx) {
    CEEPEW_ASSERT(region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    uint16_t needed = (uint16_t)(in_len + 2U);
    uint8_t *buf = (uint8_t *)region_alloc(region, needed);
    if (buf == NULL) { return CEEPEW_ERR_ALLOC; }

    buf[0] = (uint8_t)(in_len & 0xFFU);
    buf[1] = (uint8_t)(in_len >> 8U);
    for (uint16_t i = 0U; i < in_len; i++) { buf[2U + i] = in[i]; }

    *out = buf;
    *out_len = needed;

    CEEPEW_ASSERT(*out != NULL, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(*out_len == needed, CEEPEW_ERR_INTERNAL);
    return CEEPEW_OK;
}

CeePewErr_t stage_identity(Region_t *region, const uint8_t *in, uint16_t in_len, uint8_t **out, uint16_t *out_len, void *ctx) {
    CEEPEW_ASSERT(region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    uint8_t *buf = NULL;
    if (in_len > 0U) {
        buf = (uint8_t *)region_alloc(region, in_len);
        if (buf == NULL) { return CEEPEW_ERR_ALLOC; }
        for (uint16_t i = 0U; i < in_len; i++) { buf[i] = in[i]; }
    }

    *out = buf;
    *out_len = in_len;

    CEEPEW_ASSERT(in_len == 0U || *out != NULL, CEEPEW_ERR_INTERNAL);
    CEEPEW_ASSERT(*out_len == in_len, CEEPEW_ERR_INTERNAL);
    return CEEPEW_OK;
}

void test_pipeline_memory(void) {
    ESP_LOGI(TAG, "=== pipeline memory test ===");

    Region_t r;
    CeePewErr_t err = region_init(&r);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "region_init failed %d", (int)err); return; }

    err = ceepew_pipeline_init();
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "pipeline_init failed %d", (int)err); return; }

    Pipeline_t p;
    err = pipeline_reset(&p);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "pipeline_reset failed %d", (int)err); return; }

    err = pipeline_add_stage(&p, stage_fragment, NULL);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "add stage_fragment failed %d", (int)err); return; }

    err = pipeline_add_stage(&p, stage_identity, NULL);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "add stage_identity failed %d", (int)err); return; }

    const uint8_t input[] = { 'A', 'B', 'C' };
    uint8_t *out = NULL;
    uint16_t out_len = 0U;

    err = pipeline_run(&p, &r, input, (uint16_t)sizeof(input), &out, &out_len);
    if (err != CEEPEW_OK) { ESP_LOGE(TAG, "pipeline_run failed %d", (int)err); return; }

    uint8_t expected[] = { 3U, 0U, 'A', 'B', 'C' };
    if (out_len != sizeof(expected)) {
        ESP_LOGE(TAG, "unexpected out_len %u (expected %u)", out_len, (unsigned)sizeof(expected));
        return;
    }

    if (memcmp(out, expected, sizeof(expected)) != 0) {
        ESP_LOGE(TAG, "output mismatch");
        return;
    }

    ESP_LOGI(TAG, "pipeline memory test PASSED");
}
