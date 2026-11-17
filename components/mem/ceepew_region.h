/**
 * @file ceepew_region.h
 * @brief Zero-Heap Static Region Bump Allocator for CEE-PEW.
 *
 * Implements a deterministic, bounded memory management pool that replaces
 * dynamic heap allocation (malloc/free/calloc) across the CEE-PEW firmware.
 * All temporary allocations within a processing frame or cryptographic session
 * are allocated in O(1) sequential bump order with 8-byte alignment guarantees.
 * Reclaiming memory is achieved via total region reset (`region_reset`),
 * completely eliminating heap fragmentation and use-after-free vulnerabilities.
 */

#ifndef CEEPEW_REGION_H
#define CEEPEW_REGION_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>

/** Byte alignment boundary for all allocated blocks (8-byte aligned for Xtensa LX6). */
#define CEEPEW_REGION_ALIGN 8U

/** Maximum distinct allocation tracking slots for diagnostic inspection. */
#define CEEPEW_REGION_MAX_ALLOCS 256U

/**
 * @brief Bookkeeping entry for a single region allocation.
 */
typedef struct {
    uint32_t offset; /**< Byte offset from pool base buffer */
    uint32_t size;   /**< Requested allocation size in bytes */
    bool in_use;     /**< Allocation active indicator */
} RegionAlloc_t;

/**
 * @brief Primary region pool descriptor and state tracking structure.
 */
typedef struct {
    uint8_t pool[CEEPEW_REGION_POOL_BYTES];        /**< Static contiguous memory backing store */
    uint32_t bump;                                 /**< Offset to the next free aligned byte */
    uint32_t hwm;                                  /**< High-water mark of pool usage */
    RegionAlloc_t allocs[CEEPEW_REGION_MAX_ALLOCS]; /**< Allocation tracking metadata table */
    uint16_t alloc_count;                          /**< Count of active allocations recorded */
    bool initialised;                              /**< Pool initialization verification flag */
} Region_t;

/** Global system region allocator instance. */
extern Region_t g_region;

/**
 * @brief Initialize a region allocator instance.
 *
 * Zeroes the memory backing store, resets the bump pointer and high-water mark,
 * and marks the pool as initialized.
 *
 * @param[in,out] r Pointer to target Region_t descriptor.
 * @return CEEPEW_OK on success, or CEEPEW_ERR_NULL_PTR if pointer is NULL.
 */
CeePewErr_t region_init(Region_t *r);

/**
 * @brief Reset region to empty state without individual object freeing.
 *
 * Resets the bump pointer to zero and invalidates metadata entries.
 * Note: Does not clear the high-water mark, preserving diagnostic peak metrics.
 *
 * @param[in,out] r Pointer to target Region_t descriptor.
 */
void region_reset(Region_t *r);

/**
 * @brief Allocate aligned memory block from the region pool.
 *
 * Advances the bump pointer by the aligned requested size in O(1) time.
 * Updates the high-water mark if the new bump offset exceeds prior peaks.
 *
 * @param[in,out] r Pointer to target Region_t descriptor.
 * @param[in] size Number of bytes requested.
 * @return Pointer to 8-byte aligned memory block, or NULL if pool is exhausted.
 */
void *region_alloc(Region_t *r, uint32_t size);

/**
 * @brief Allocate zeroed memory block from the region pool.
 *
 * Equivalent to region_alloc(), with the returned memory block guaranteed
 * to be initialized to zero.
 *
 * @param[in,out] r Pointer to target Region_t descriptor.
 * @param[in] size Number of bytes requested.
 * @return Pointer to zeroed aligned memory block, or NULL on pool exhaustion.
 */
void *region_alloc_zeroed(Region_t *r, uint32_t size);

/**
 * @brief Calculate remaining unallocated capacity in the region pool.
 *
 * @param[in] r Pointer to target Region_t descriptor.
 * @return Number of free bytes remaining before pool exhaustion.
 */
uint32_t region_free_bytes(Region_t *r);

/**
 * @brief Calculate current allocated memory in the region pool.
 *
 * @param[in] r Pointer to target Region_t descriptor.
 * @return Number of bytes currently consumed by active allocations.
 */
uint32_t region_used_bytes(Region_t *r);

/**
 * @brief Calculate current pool consumption as a percentage.
 *
 * @param[in] r Pointer to target Region_t descriptor.
 * @return Percentage of pool utilized (integer 0 to 100).
 */
uint8_t region_usage_pct(Region_t *r);

#endif /* CEEPEW_REGION_H */
