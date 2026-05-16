/* components/compress/compress_huffman.c */

#include <stdint.h>
#include <string.h>
#include "../../main/ceepew_assert.h"

/* Simple pass-through compression API for initial integration.
 * Replace with full static Huffman implementation later.
 */

CeePewErr_t compress_huffman_compress(const uint8_t *in, uint16_t in_len,
                                      uint8_t *out, uint16_t *out_len, uint16_t max_out_len)
{
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(max_out_len > 0U, CEEPEW_ERR_PARAM);

    /* No real compression yet: passthrough if space allows */
    if ((uint16_t)in_len > max_out_len)
    {
        return CEEPEW_ERR_BOUNDS;
    }

    memcpy(out, in, in_len);
    *out_len = in_len;
    return CEEPEW_ERR_UNSUPPORTED;
}

CeePewErr_t compress_huffman_decompress(const uint8_t *in, uint16_t in_len,
                                        uint8_t *out, uint16_t *out_len, uint16_t max_out_len)
{
    CEEPEW_ASSERT(in != NULL || in_len == 0U, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(out != NULL && out_len != NULL, CEEPEW_ERR_NULL_PTR);

    if ((uint16_t)in_len > max_out_len)
    {
        return CEEPEW_ERR_BOUNDS;
    }

    memcpy(out, in, in_len);
    *out_len = in_len;
    return CEEPEW_ERR_UNSUPPORTED;
}
