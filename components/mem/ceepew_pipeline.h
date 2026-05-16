/* components/mem/ceepew_pipeline.h */
#ifndef CEEPEW_PIPELINE_H
#define CEEPEW_PIPELINE_H

#include "../mem/ceepew_region.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>

/* Initialize pipeline subsystem. Must be called once after region_init. */
CeePewErr_t ceepew_pipeline_init(void);

/* Fragment a message into one or more transport-ready fragments.
 * This implementation currently provides a single-fragment pass-through
 * for simplicity. The caller must provide an output buffer and its max size.
 * On success, out_len is set to the number of bytes written.
 */
CeePewErr_t ceepew_pipeline_fragment(const uint8_t *in, uint16_t in_len,
                                     uint8_t *out, uint16_t *out_len, uint16_t max_out_len);

/* Reassemble fragments into a single message. This simple implementation
 * assumes a single-fragment passthrough.
 */
CeePewErr_t ceepew_pipeline_reassemble(const uint8_t *in, uint16_t in_len,
                                        uint8_t *out, uint16_t *out_len, uint16_t max_out_len);

#endif /* CEEPEW_PIPELINE_H */
