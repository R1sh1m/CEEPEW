# mem — Region Allocator & Pipeline

Zero-heap memory management for CEE-PEW. All components (except `ceepew_oled`) must use this allocator.

## Files

| File | Purpose |
|------|---------|
| `ceepew_region.h/c` | Region allocator — 48KB static pool, bump-pointer allocation, bulk free via `region_reset()` |
| `ceepew_pipeline.h/c` | Typed pipeline slots over the region pool for zero-copy message passing between tasks |

## Region Pool (`ceepew_region`)
- **Pool size**: 48 KB (`CEEPEW_REGION_POOL_BYTES` in `main/ceepew_config.h`)
- **Allocation**: `region_alloc(size)` — returns aligned pointer or NULL on OOM
- **Free**: No per-object free. Use `region_reset()` to reclaim entire pool (called on session reset)
- **Stats**: `region_stats()` returns used/remaining bytes
- **Alignment**: 8-byte alignment for all allocations

## Pipeline (`ceepew_pipeline`)
- **Slots**: Fixed array of typed slots over the region pool
- **Types**: `PIPELINE_SLOT_PLAINTEXT`, `PIPELINE_SLOT_CIPHERTEXT`, `PIPELINE_SLOT_ACK`, `PIPELINE_SLOT_CONTROL`
- **Max occupancy**: `CEEPEW_PIPELINE_MAX_SLOTS` (typically 8)
- **Zero-copy**: Slots reference region memory directly; no memcpy between stages
- **Ownership**: Single-writer, single-reader per slot type (enforced by convention)

## Why No malloc
- Deterministic memory usage — no fragmentation, no allocation failures at runtime
- No heap overhead — 48KB pool is statically allocated in `.bss`
- Security — no metadata to leak, no use-after-free possible
- Real-time — allocation is O(1) bump pointer, no lock contention

## Usage
```c
// In session task
uint8_t *buf = region_alloc(64);
CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_MEM);
// ... use buf ...
// On session reset:
region_reset(&g_region);
```

## Tunables (`main/ceepew_config.h`)
- `CEEPEW_REGION_POOL_BYTES` = 48 * 1024 (increase if OOM, never use malloc)
- `CEEPEW_PIPELINE_MAX_SLOTS` = 8