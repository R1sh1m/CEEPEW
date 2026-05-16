/* components/mem/ceepew_pipeline.c */

#include "ceepew_pipeline.h"
#include "ceepew_region.h"
#include "../../main/ceepew_config.h"
#include "../../main/ceepew_assert.h"
#include <string.h>

static bool s_initialised = false;

CeePewErr_t ceepew_pipeline_init(void)
{
    CEEPEW_ASSERT(!s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(region_usage_pct(&g_region) < 100U, CEEPEW_ERR_ALLOC);

    s_initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t ceepew_pipeline_fragment(const uint8_t *in, uint16_t in_len,
                                     uint8_t *out, uint16_t *out_len, uint16_t max_out_len)
{
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* Simple single-fragment pass-through implementation.
     * This avoids dynamic fragmentation and is safe for initial integration.
     */
    CEEPEW_ASSERT(in_len <= max_out_len, CEEPEW_ERR_BOUNDS);

    memcpy(out, in, in_len);
    *out_len = in_len;
    return CEEPEW_OK;
}

CeePewErr_t ceepew_pipeline_reassemble(const uint8_t *in, uint16_t in_len,
                                        uint8_t *out, uint16_t *out_len, uint16_t max_out_len)
{
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    /* Single-fragment passthrough: copy if space allows */
    CEEPEW_ASSERT(in_len <= max_out_len, CEEPEW_ERR_BOUNDS);

    memcpy(out, in, in_len);
    *out_len = in_len;
    return CEEPEW_OK;
}
