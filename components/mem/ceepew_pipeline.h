/* components/mem/ceepew_pipeline.h */
#ifndef CEEPEW_PIPELINE_H
#define CEEPEW_PIPELINE_H

#include "../mem/ceepew_region.h"
#include "ceepew_assert.h"
#include <stdint.h>
#include <stddef.h>

/* Maximum number of stages in a pipeline chain */
#define CEEPEW_PIPELINE_MAX_STAGES 16U

/* Stage function signature: processes input through a transformation, 
 * allocates output from region, and returns output buffer + length. */
typedef CeePewErr_t (*PipelineStage_fn)(
    Region_t *region,
    const uint8_t *in,
    uint16_t in_len,
    uint8_t **out,
    uint16_t *out_len,
    void *stage_ctx
);

/* Pipeline dispatch table — holds a sequence of stages to execute in order */
typedef struct {
    PipelineStage_fn fn[CEEPEW_PIPELINE_MAX_STAGES];
    void            *ctx[CEEPEW_PIPELINE_MAX_STAGES];
    uint8_t          stage_count;
} Pipeline_t;

/* Initialize pipeline subsystem. Must be called once after region_init. */
CeePewErr_t ceepew_pipeline_init(void);
CeePewErr_t ceepew_pipeline_deinit(void);

/* Execute a complete pipeline: chains all registered stages, each processing
 * the output of the previous stage as its input. All intermediate buffers
 * are region_alloc'd by the stages that produce them. */
CeePewErr_t pipeline_run(Pipeline_t *p, Region_t *region,
                         const uint8_t *input, uint16_t input_len,
                         uint8_t **output, uint16_t *output_len);

/* Register a stage function to execute in the pipeline. Stages execute in
 * registration order. Returns error if stage_count >= CEEPEW_PIPELINE_MAX_STAGES. */
CeePewErr_t pipeline_add_stage(Pipeline_t *p, PipelineStage_fn fn, void *stage_ctx);

/* Reset pipeline to empty state (clear all stages) */
CeePewErr_t pipeline_reset(Pipeline_t *p);

/* Legacy pass-through API for simple encode/decode without full pipeline */
CeePewErr_t ceepew_pipeline_fragment(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, uint16_t max_out_len);
CeePewErr_t ceepew_pipeline_reassemble(const uint8_t *in, uint16_t in_len, uint8_t *out, uint16_t *out_len, uint16_t max_out_len);

#endif /* CEEPEW_PIPELINE_H */
