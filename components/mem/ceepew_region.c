/* components/mem/ceepew_region.c */

#include "ceepew_region.h"
#include "ceepew_assert.h"
#include "ceepew_config.h"
#include "ceepew_security_utils.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

Region_t g_region = { .bump = 0U, .hwm = 0U, .alloc_count = 0U, .initialised = false};

CeePewErr_t region_init(Region_t *r) {
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(CEEPEW_REGION_ALIGN > 0U && (CEEPEW_REGION_ALIGN & (CEEPEW_REGION_ALIGN - 1U)) == 0U, CEEPEW_ERR_PARAM);
    if (!r->initialised) { r->hwm = 0U; }
    r->initialised = true;
    region_reset(r);
    return CEEPEW_OK;
}

void region_reset(Region_t *r) {
    CEEPEW_ASSERT_VOID(r != NULL);
    CEEPEW_ASSERT_VOID(CEEPEW_REGION_POOL_BYTES > 0U);

    /* loop bound: CEEPEW_REGION_POOL_BYTES (compile-time constant) */
    ceepew_secure_zero((volatile void *)r->pool, CEEPEW_REGION_POOL_BYTES);

    r->bump = 0U;
    r->alloc_count = 0U;

    /* loop bound: CEEPEW_REGION_MAX_ALLOCS (compile-time constant) */
    for (uint16_t i = 0U; i < CEEPEW_REGION_MAX_ALLOCS; i++){
        r->allocs[i].offset = 0U;
        r->allocs[i].size = 0U;
        r->allocs[i].in_use = false;
    }

    /* Do not alter r->initialised here — region_init controls initialization state */
}

void *region_alloc(Region_t *r, uint32_t size){
    CEEPEW_ASSERT_PTR(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT_PTR(r->initialised, CEEPEW_ERR_BUSY);

    if (size == 0U || size > CEEPEW_REGION_POOL_BYTES) { return NULL; }

    if (r->alloc_count >= CEEPEW_REGION_MAX_ALLOCS) { return NULL; }

    uint32_t align_mask = CEEPEW_REGION_ALIGN - 1U;
    uint32_t aligned_bump = (r->bump + align_mask) & ~align_mask;
    if (aligned_bump > CEEPEW_REGION_POOL_BYTES) { return NULL; }
    if (size > (CEEPEW_REGION_POOL_BYTES - aligned_bump)) { return NULL; }

    uint16_t rec = r->alloc_count;
    r->allocs[rec].offset = aligned_bump;
    r->allocs[rec].size = size;
    r->allocs[rec].in_use = true;
    r->alloc_count++;

    r->bump = aligned_bump + size;
    if (r->bump > r->hwm) { r->hwm = r->bump; }

    return (void *)&r->pool[aligned_bump];
}

void *region_alloc_zeroed(Region_t *r, uint32_t size) {
    CEEPEW_ASSERT_PTR(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT_PTR(r->initialised, CEEPEW_ERR_BUSY);

    void *ptr = region_alloc(r, size);
    if (ptr == NULL) { return NULL; }

    volatile uint8_t *p = (volatile uint8_t *)ptr;
    /* loop bound: size <= CEEPEW_REGION_POOL_BYTES */
    for (uint32_t i = 0U; i < size; i++) { p[i] = 0U; }
    __asm__ __volatile__("" ::: "memory");

    return ptr;
}

uint32_t region_free_bytes(Region_t *r) {
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->initialised, CEEPEW_ERR_BUSY);

    if (r->bump >= CEEPEW_REGION_POOL_BYTES) { return 0U; }
    return (CEEPEW_REGION_POOL_BYTES - r->bump);
}

uint32_t region_used_bytes(Region_t *r) {
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(r->bump <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_INTERNAL);
    return r->bump;
}

uint8_t region_usage_pct(Region_t *r){
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(r->initialised, CEEPEW_ERR_BUSY);
    CEEPEW_ASSERT(CEEPEW_REGION_POOL_BYTES > 0U, CEEPEW_ERR_PARAM);

    uint32_t used = r->bump;
    if (used > CEEPEW_REGION_POOL_BYTES) {
        used = CEEPEW_REGION_POOL_BYTES;
    }

    uint32_t pct = (used * 100U) / CEEPEW_REGION_POOL_BYTES;
    if (pct > 100U) { pct = 100U;}
    return (uint8_t)pct;
}
