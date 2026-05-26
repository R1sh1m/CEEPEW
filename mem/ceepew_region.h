/* mem/ceepew_region.h - Minimal region allocator header stub for build/tests */

#ifndef CEEPEW_REGION_H
#define CEEPEW_REGION_H

#include "../main/ceepew_config.h"
#include "../main/ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>

#define CEEPEW_REGION_POOL_BYTES (48U * 1024U)
#define CEEPEW_REGION_MAX_ALLOCS 256U

typedef struct {
    uint32_t offset;
    uint32_t size;
    bool in_use;
} RegionAlloc_t;

typedef struct {
    uint8_t  pool[CEEPEW_REGION_POOL_BYTES];
    uint32_t bump;
    uint32_t hwm;
    RegionAlloc_t allocs[CEEPEW_REGION_MAX_ALLOCS];
    uint16_t alloc_count;
    bool initialised;
} Region_t;

extern Region_t g_region;

void *region_alloc(Region_t *r, uint32_t size);
void *region_alloc_zeroed(Region_t *r, uint32_t size);
CeePewErr_t region_init(Region_t *r);
void region_reset(Region_t *r);

#endif /* CEEPEW_REGION_H */
