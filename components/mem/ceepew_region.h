/* components/mem/ceepew_region.h */

#ifndef CEEPEW_REGION_H
#define CEEPEW_REGION_H

#include "../../main/ceepew_config.h"
#include "../../main/ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>

/* Region allocator configuration: values defined in main/ceepew_config.h */
#define CEEPEW_REGION_ALIGN 8U
#define CEEPEW_REGION_MAX_ALLOCS 256U

typedef struct
{
    uint32_t offset; /* byte offset from pool base */
    uint32_t size;   /* allocation size in bytes */
    bool in_use;     /* bookkeeping */
} RegionAlloc_t;

typedef struct
{
    uint8_t pool[CEEPEW_REGION_POOL_BYTES];
    uint32_t bump; /* next free offset */
    uint32_t hwm;  /* high-water mark */
    RegionAlloc_t allocs[CEEPEW_REGION_MAX_ALLOCS];
    uint16_t alloc_count; /* number of active allocs recorded */
    bool initialised;
} Region_t;

extern Region_t g_region;

CeePewErr_t region_init(Region_t *r);
void region_reset(Region_t *r);
void *region_alloc(Region_t *r, uint32_t size);
void *region_alloc_zeroed(Region_t *r, uint32_t size);
uint32_t region_free_bytes(Region_t *r);
uint32_t region_used_bytes(Region_t *r);
uint8_t region_usage_pct(Region_t *r);

#endif /* CEEPEW_REGION_H */
