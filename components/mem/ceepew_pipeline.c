/* components/mem/ceepew_pipeline.c
 *
 * Dispatch-table pipeline engine: chains a sequence of transformations
 * (compress, encrypt, FEC, CRC) into a single data flow.
 * Each stage allocates its output from the region allocator.
 * Supports configurable TX and RX chains.
 */

#include "ceepew_pipeline.h"
#include "ceepew_region.h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

/* Design note: The pipeline is a general-purpose dispatcher that executes
   a sequence of function pointers in order, threading the output of each
   stage as the input to the next. This decouples the encoding/crypto/FEC
   layers from the specific order and composition of operations, allowing
   runtime assembly of different processing chains (e.g., TX compress→encrypt→FEC
   vs RX CRC→FEC decode→decrypt→decompress). Each stage is responsible for
   allocating its own output from the region. */

static bool s_initialised = false;

CeePewErr_t ceepew_pipeline_init(void)
{
    CEEPEW_ASSERT(!s_initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(region_usage_pct(&g_region) <= 100U, CEEPEW_ERR_ALLOC);
    s_initialised = true;
    return CEEPEW_OK;
}

CeePewErr_t ceepew_pipeline_deinit(void)
{
    CEEPEW_ASSERT(region_usage_pct(&g_region) <= 100U, CEEPEW_ERR_ALLOC);
    s_initialised = false;
    return CEEPEW_OK;
}

CeePewErr_t pipeline_reset(Pipeline_t *p)
{
    CEEPEW_ASSERT(p != NULL, CEEPEW_ERR_NULL_PTR);
    
    /* loop bound: CEEPEW_PIPELINE_MAX_STAGES = 16U (compile-time constant) */
    for (uint8_t i = 0U; i < CEEPEW_PIPELINE_MAX_STAGES; i++) {
        p->fn[i] = NULL;
        p->ctx[i] = NULL;
    }
    p->stage_count = 0U;
    return CEEPEW_OK;
}

CeePewErr_t pipeline_add_stage(Pipeline_t *p, PipelineStage_fn fn, void *stage_ctx)
{
    CEEPEW_ASSERT(p != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(fn != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(p->stage_count < CEEPEW_PIPELINE_MAX_STAGES, CEEPEW_ERR_BOUNDS);
    
    p->fn[p->stage_count] = fn;
    p->ctx[p->stage_count] = stage_ctx;
    p->stage_count++;
    return CEEPEW_OK;
}

CeePewErr_t pipeline_run(Pipeline_t *p, Region_t *region,
                         const uint8_t *input, uint16_t input_len,
                         uint8_t **output, uint16_t *output_len)
{
    CEEPEW_ASSERT(p != NULL && region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(input != NULL || input_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(output != NULL && output_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(p->stage_count > 0U &&
                  p->stage_count <= CEEPEW_PIPELINE_MAX_STAGES,
                  CEEPEW_ERR_PARAM);

    const uint8_t *cur_in  = input;
    uint16_t       cur_len = input_len;
    uint8_t       *cur_out = NULL;
    uint16_t       cur_out_len = 0U;

    /* loop bound: p->stage_count <= CEEPEW_PIPELINE_MAX_STAGES = 16U */
    for (uint8_t s = 0U; s < p->stage_count; s++) {
        CEEPEW_ASSERT(p->fn[s] != NULL, CEEPEW_ERR_PARAM);

        CeePewErr_t err = p->fn[s](region, cur_in, cur_len,
                                    &cur_out, &cur_out_len, p->ctx[s]);
        if (err != CEEPEW_OK) { return err; }
        CEEPEW_ASSERT(cur_out != NULL, CEEPEW_ERR_BOUNDS);
        CEEPEW_ASSERT(cur_out_len > 0U, CEEPEW_ERR_BOUNDS);

        cur_in  = cur_out;
        cur_len = cur_out_len;
    }

    *output     = cur_out;
    *output_len = cur_out_len;
    return CEEPEW_OK;
}

/* Legacy pass-through functions for simple encode/decode */
CeePewErr_t ceepew_pipeline_fragment(const uint8_t *in, uint16_t in_len,
                                      uint8_t *out, uint16_t *out_len,
                                      uint16_t max_out_len)
{
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT((uint32_t)in_len + 2U <= max_out_len, CEEPEW_ERR_BOUNDS);

    out[0] = (uint8_t)(in_len & 0xFFU);
    out[1] = (uint8_t)(in_len >> 8U);
    /* loop bound: in_len <= CEEPEW_MAX_MSG_BYTES (validated by caller) */
    for (uint16_t i = 0U; i < in_len; i++) { out[2U + i] = in[i]; }
    *out_len = (uint16_t)(in_len + 2U);
    return CEEPEW_OK;
}

CeePewErr_t ceepew_pipeline_reassemble(const uint8_t *in, uint16_t in_len,
                                        uint8_t *out, uint16_t *out_len,
                                        uint16_t max_out_len)
{
    CEEPEW_ASSERT(in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len >= 2U, CEEPEW_ERR_PARAM);

    uint16_t payload_len = (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8U));
    CEEPEW_ASSERT((uint32_t)payload_len + 2U == in_len, CEEPEW_ERR_PARAM);
    CEEPEW_ASSERT(payload_len <= max_out_len, CEEPEW_ERR_BOUNDS);

    /* loop bound: payload_len <= CEEPEW_MAX_MSG_BYTES (validated above) */
    for (uint16_t i = 0U; i < payload_len; i++) { out[i] = in[2U + i]; }
    *out_len = payload_len;
    return CEEPEW_OK;
}
