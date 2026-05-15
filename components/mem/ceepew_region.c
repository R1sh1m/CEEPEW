/* components/mem/ceepew_region.c */

#include "ceepew_region.h"
#include "../../main/ceepew_assert.h"
#include "../../main/ceepew_config.h"
#include <string.h>
#include <stdint.h>

/* Static instance of region allocator */
Region_t g_region = {
    .bump = 0U,
    .hwm = 0U,
    .alloc_count = 0U,
    .initialised = false
};

CeePewErr_t region_init(Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(CEEPEW_REGION_POOL_BYTES > 0U, CEEPEW_ERR_PARAM);

    /* Reset and mark initialised */
    region_reset(r);
    r->initialised = true;
    return CEEPEW_OK;
}

/* secure_zero: volatile-zero memory with a compiler barrier to prevent optimization
 * Use for wiping key material. */
static void secure_zero(void *ptr, size_t len)
{
    CEEPEW_ASSERT_VOID(ptr != NULL);
    CEEPEW_ASSERT_VOID(len > 0U);
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    /* Compile-time bounded loop: iterate up to CEEPEW_REGION_POOL_BYTES and stop at len */
    for (size_t i = 0U; i < CEEPEW_REGION_POOL_BYTES; i++) {
        if (i >= len) { break; }
        p[i] = 0U;
    }
    __asm__ volatile ("" ::: "memory");
}

void region_reset(Region_t *r)
{
    CEEPEW_ASSERT_VOID(r != NULL);
    CEEPEW_ASSERT_VOID(CEEPEW_REGION_MAX_ALLOCS >= 1U);

    /* Securely zero the pool to ensure key material is wiped */
    secure_zero(r->pool, CEEPEW_REGION_POOL_BYTES);

    r->bump = 0U;
    r->hwm = 0U;
    r->alloc_count = 0U;
    r->initialised = true;
}

void *region_alloc(Region_t *r, uint32_t size)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(size > 0U && size <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);

    /* Align bump to CEEPEW_REGION_ALIGN */
    uint32_t aligned = (r->bump + (CEEPEW_REGION_ALIGN - 1U)) & ~(CEEPEW_REGION_ALIGN - 1U);

    if (aligned + size > CEEPEW_REGION_POOL_BYTES) {
        return NULL; /* pool exhausted */
    }

    CEEPEW_ASSERT(r->alloc_count < CEEPEW_REGION_MAX_ALLOCS, CEEPEW_ERR_ALLOC);

    uint16_t idx = r->alloc_count++;
    r->allocs[idx].offset = aligned;
    r->allocs[idx].size = size;
    r->allocs[idx].in_use = true;

    r->bump = aligned + size;
    if (r->bump > r->hwm) { r->hwm = r->bump; }

    return (void *)&r->pool[aligned];
}

void *region_alloc_zeroed(Region_t *r, uint32_t size)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(size > 0U && size <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_BOUNDS);

    void *ptr = region_alloc(r, size);
    if (ptr != NULL) {
        memset(ptr, 0, (size_t)size);
    }
    return ptr;
}

uint32_t region_free_bytes(Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->bump <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_INTERNAL);

    return (uint32_t)(CEEPEW_REGION_POOL_BYTES - r->bump);
}

uint32_t region_used_bytes(Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->bump <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_INTERNAL);

    return r->bump;
}

uint8_t region_usage_pct(Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(CEEPEW_REGION_POOL_BYTES > 0U, CEEPEW_ERR_PARAM);

    uint32_t pct = (uint32_t)((r->bump * 100U) / CEEPEW_REGION_POOL_BYTES);
    if (pct > 100U) { pct = 100U; }
    return (uint8_t)pct;
}
