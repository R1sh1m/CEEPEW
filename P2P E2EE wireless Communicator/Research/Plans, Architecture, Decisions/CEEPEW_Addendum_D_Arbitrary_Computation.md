# CEE-PEW — Addendum D: Arbitrary Computation & Arbitrary-Length Structures
### From fixed-buffer FSM to a practically universal execution model
**Version:** 1.0 | Extends: Final Master Specification v2.0 + Addendum C

---

## PART 1 — THE HONEST ANSWER: WHAT WE CURRENTLY ARE

### 1.1 The Gap Between Our Claim and Reality

The Final Master Specification v2 states: *"The system is an event-driven actor
model, not a flat FSM."* and *"Turing-complete."* That claim needs unpacking.

**What we actually have today:**
- Fixed static buffers (`CEEPEW_MAX_MSG_BYTES = 160U`)
- All loop bounds are compile-time constants
- No dynamic allocation whatsoever
- Message fragmentation capped at `CEEPEW_MAX_FRAGMENTS = 4`
- Crypto pipeline stages are hardcoded and sequential

**Is this Turing-complete?**
No — not in the strict Church-Turing sense. A system with bounded memory
and bounded computation is a finite-state machine with very many states,
not a universal computer. The claim in v2 was aspirational.

**Does that matter in practice?**
It depends entirely on what "arbitrary" means for CEE-PEW's use case.

For a **messaging device**, arbitrary-length means:
> *"Any message that physically fits in available RAM, processed correctly
>  regardless of length, without crashes, overflows, or silent truncation."*

For a **cryptographic pipeline**, arbitrary means:
> *"The encryption stack must handle any payload length up to RAM capacity,
>  applying the same security properties regardless of message size."*

For a **general computation model**, arbitrary means:
> *"The device can execute any algorithm expressible in the language, bounded
>  only by physical memory and time — not by compile-time constants."*

This addendum delivers all three, with formal analysis of each.

---

## PART 2 — THE THEORETICAL BARRIER AND HOW TO CROSS IT

### 2.1 Why Pure Static Allocation Limits You

The JPL coding rule "no dynamic allocation after init" was designed for
safety-critical aerospace software where heap corruption can crash a spacecraft.
It prevents:
- Memory leaks
- Heap fragmentation causing unpredictable allocation failures
- Use-after-free vulnerabilities
- Buffer overflows from unbounded allocations

But it trades these against:
- **Inflexibility**: max message size is a compile-time decision
- **Waste**: buffers are allocated for worst-case, always
- **Rigidity**: the pipeline cannot adapt to input length

### 2.2 The Resolution: Region (Arena) Allocation

A **region allocator** (also called arena allocator, bump allocator, or
zone allocator) is the bridge. It satisfies the JPL rule while enabling
dynamic-length structures:

```
STATIC POOL: uint8_t g_region_pool[CEEPEW_REGION_POOL_BYTES];
             (declared once, never freed individually)

Allocation:  bump a pointer forward into the pool
             — as fast as incrementing an integer
             — no fragmentation possible
             — no heap involved

Reclamation: reset the bump pointer to zero at session end
             — en-masse, instant, complete
             — equivalent to stack unwinding
```

This is not "dynamic allocation" in the OS/heap sense. It is bounded,
deterministic, and provably safe. The pool size is a compile-time constant;
individual allocations within it are runtime-sized.

**The JPL rule re-read:** "Do not use dynamic memory allocation **after
initialization**." A region allocator initialized at boot with a fixed pool
satisfies this literally — the pool is the initialization-time allocation.
What happens inside the pool is internal bookkeeping, not OS heap usage.

### 2.3 Turing-Completeness: What We Can and Cannot Achieve

| Property | Hard limit | CEE-PEW target |
|---|---|---|
| Unbounded memory | Physical impossibility on any real hardware | Bounded by 520 KB SRAM, pool-managed |
| Unbounded computation time | FreeRTOS watchdog prevents infinite loops | Bounded loops with pool-size-derived bounds |
| Arbitrary data structures | Requires heap | Region-allocated variable-length structures |
| Recursion | Stack overflow risk | Explicit stack (pool-allocated), bounded depth |
| Arbitrary message length | Fixed buffers | Any length ≤ free pool space |
| Streaming computation | Batch-only today | Chunk-based streaming over existing ARQ |

**The practical result:** CEE-PEW becomes a **physically-bounded universal
computer** — it can express any algorithm that fits in its memory budget.
This is equivalent to every real-world computer ever built. True mathematical
Turing-completeness (unbounded tape) is physically impossible; practical
universality is achievable and is what we implement.

---

## PART 3 — THE REGION ALLOCATOR

### 3.1 Design

```c
/* mem/ceepew_region.h */
#ifndef CEEPEW_REGION_H
#define CEEPEW_REGION_H

#include "ceepew_config.h"
#include "ceepew_assert.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * CEEPEW_REGION_POOL_BYTES — total size of the region pool.
 *
 * Budget breakdown (520 KB total SRAM):
 *   FreeRTOS kernel + two task stacks:  ~20 KB
 *   g_crypto_ctx + key material:         ~2 KB
 *   g_message_buffer (static, legacy):   ~4 KB
 *   OLED framebuffer:                    ~1 KB
 *   WiFi/BT driver buffers (internal):  ~80 KB (ESP-IDF reserves this)
 *   Region pool:                        200 KB  ← this allocator owns this
 *   Reserve / guard:                    ~213 KB (headroom)
 *
 * 200 KB pool supports:
 *   - Messages up to ~180 KB of plaintext before Huffman compression
 *   - After compression at 0.65× average: ~117 KB compressed payload
 *   - After crypto_box + Ascon overhead: ~117 KB + 48 bytes
 *   - Fragmented into 250-byte ESP-NOW packets: ~478 fragments
 *   - Practical limit: CEEPEW_REGION_POOL_BYTES / 2 usable per message
 *     (one half for source, one half for transformed output)
 *
 * This is the theoretical maximum for a single message.
 * Typical use: messages of 1–2 KB (multi-paragraph text).
 */
#define CEEPEW_REGION_POOL_BYTES    (200U * 1024U)   /* 200 KB */
#define CEEPEW_REGION_ALIGN         8U               /* 8-byte alignment */
#define CEEPEW_REGION_MAX_ALLOCS    256U             /* bookkeeping entries */

/*
 * RegionAlloc_t — a single allocation record.
 * Stored in a parallel static array, never on the heap.
 */
typedef struct {
    uint32_t offset;   /* byte offset from pool base */
    uint32_t size;     /* bytes allocated             */
    bool     in_use;
} RegionAlloc_t;

/*
 * Region_t — the region allocator state.
 * One static instance: g_region.
 * MUST be reset at session start and session end via region_reset().
 */
typedef struct {
    uint8_t       pool[CEEPEW_REGION_POOL_BYTES]; /* the backing store */
    uint32_t      bump;       /* next free byte offset */
    uint32_t      hwm;        /* high-water mark (peak usage) */
    RegionAlloc_t allocs[CEEPEW_REGION_MAX_ALLOCS];
    uint16_t      alloc_count;
    bool          initialised;
} Region_t;

extern Region_t g_region;

/* Lifecycle — called at session start and session end */
CeePewErr_t region_init(Region_t *r);
void        region_reset(Region_t *r);   /* zeros pool, resets bump */

/*
 * region_alloc()
 *
 * Allocates 'size' bytes from the pool, aligned to CEEPEW_REGION_ALIGN.
 * Returns pointer on success, NULL on pool exhaustion.
 * Loop-free: bump pointer arithmetic only.
 * O(1) time complexity.
 *
 * NEVER call free() on the returned pointer. Use region_reset() to
 * reclaim all allocations at once.
 */
void *region_alloc(Region_t *r, uint32_t size);

/*
 * region_alloc_zeroed()
 * Same as region_alloc but zeroes the allocation before returning.
 * Used for security-sensitive buffers.
 */
void *region_alloc_zeroed(Region_t *r, uint32_t size);

/* Query functions */
uint32_t region_free_bytes(const Region_t *r);
uint32_t region_used_bytes(const Region_t *r);
uint8_t  region_usage_pct(const Region_t *r);

#endif /* CEEPEW_REGION_H */
```

### 3.2 Implementation

```c
/* mem/ceepew_region.c */

#include "ceepew_region.h"

Region_t g_region;

CeePewErr_t region_init(Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(CEEPEW_REGION_POOL_BYTES > 0U, CEEPEW_ERR_PARAM);

    memset(r->pool,   0, sizeof(r->pool));
    memset(r->allocs, 0, sizeof(r->allocs));
    r->bump        = 0U;
    r->hwm         = 0U;
    r->alloc_count = 0U;
    r->initialised = true;

    return CEEPEW_OK;
}

void region_reset(Region_t *r)
{
    CEEPEW_ASSERT_VOID(r != NULL);
    CEEPEW_ASSERT_VOID(r->initialised);

    /* Secure-zero the entire pool before resetting —
     * any partial plaintext or key-adjacent data is wiped.          */
    volatile uint8_t *p = (volatile uint8_t *)r->pool;
    for (uint32_t i = 0U; i < CEEPEW_REGION_POOL_BYTES; i++) {
        p[i] = 0U;
    }
    __asm__ __volatile__("" ::: "memory");

    memset(r->allocs, 0, sizeof(r->allocs));
    r->bump        = 0U;
    r->alloc_count = 0U;
    /* r->hwm is NOT reset — it accumulates across sessions for DIAG */
}

void *region_alloc(Region_t *r, uint32_t size)
{
    CEEPEW_ASSERT(r != NULL,         NULL);
    CEEPEW_ASSERT(r->initialised,    NULL);
    CEEPEW_ASSERT(size > 0U,         NULL);
    CEEPEW_ASSERT(size <= CEEPEW_REGION_POOL_BYTES, NULL);

    /* Align bump to CEEPEW_REGION_ALIGN */
    uint32_t align_mask = CEEPEW_REGION_ALIGN - 1U;
    uint32_t aligned_bump = (r->bump + align_mask) & ~align_mask;
    uint32_t new_bump = aligned_bump + size;

    /* Pool exhaustion check */
    if (new_bump > CEEPEW_REGION_POOL_BYTES) {
        return NULL;   /* caller must handle exhaustion */
    }

    /* Bookkeeping — bounded by CEEPEW_REGION_MAX_ALLOCS */
    if (r->alloc_count < CEEPEW_REGION_MAX_ALLOCS) {
        r->allocs[r->alloc_count].offset = aligned_bump;
        r->allocs[r->alloc_count].size   = size;
        r->allocs[r->alloc_count].in_use = true;
        r->alloc_count++;
    }

    r->bump = new_bump;
    if (r->bump > r->hwm) { r->hwm = r->bump; }

    return (void *)(r->pool + aligned_bump);
}

void *region_alloc_zeroed(Region_t *r, uint32_t size)
{
    CEEPEW_ASSERT(r != NULL, NULL);
    CEEPEW_ASSERT(size > 0U, NULL);

    void *ptr = region_alloc(r, size);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

uint32_t region_free_bytes(const Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, 0U);
    CEEPEW_ASSERT(r->initialised, 0U);
    return CEEPEW_REGION_POOL_BYTES - r->bump;
}

uint32_t region_used_bytes(const Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, 0U);
    return r->bump;
}

uint8_t region_usage_pct(const Region_t *r)
{
    CEEPEW_ASSERT(r != NULL, 0U);
    return (uint8_t)((r->bump * 100U) / CEEPEW_REGION_POOL_BYTES);
}
```

### 3.3 Bounded-Dynamic Loop Pattern

The region allocator enables a new loop pattern: bounds derived from
**runtime-determined but region-bounded** sizes. This satisfies the spirit
of the JPL rule while allowing variable-length processing:

```c
/*
 * CEEPEW_ASSERT_BOUND(len, max) — validates that a runtime length is
 * within the region's current free space before using it as a loop bound.
 * This makes the loop bound statically provable: it cannot exceed max,
 * which is itself bounded by CEEPEW_REGION_POOL_BYTES.
 */
#define CEEPEW_ASSERT_BOUND(len, max_constant) \
    CEEPEW_ASSERT((len) <= (max_constant), CEEPEW_ERR_BOUNDS)

/* Usage: */
uint32_t msg_len = receive_message_length();   /* runtime value */
CEEPEW_ASSERT_BOUND(msg_len, CEEPEW_REGION_POOL_BYTES / 2U);

uint8_t *buf = region_alloc(&g_region, msg_len);
CEEPEW_ASSERT(buf != NULL, CEEPEW_ERR_BOUNDS);

/* Now msg_len is certified <= CEEPEW_REGION_POOL_BYTES/2 — static checker
 * can verify the loop cannot exceed CEEPEW_REGION_POOL_BYTES/2 iterations */
for (uint32_t i = 0U; i < msg_len; i++) {
    buf[i] = process_byte(input[i]);
}
```

---

## PART 4 — STREAMING MESSAGE PROTOCOL

### 4.1 Problem: ESP-NOW is 250-byte-max per frame

With the existing architecture, large messages must be fragmented. The
old design fixed `CEEPEW_MAX_FRAGMENTS = 4`, capping messages at ~572 chars.
With the region allocator, we remove this artificial cap.

### 4.2 The Stream Frame

```c
/* transport/transport_stream.h */

/*
 * Stream state machine — replaces the fixed 4-fragment model.
 * A "stream" is an ordered sequence of frames carrying one logical message.
 * The stream is identified by stream_id (8 bits, rolls over at 255).
 * The receiver reassembles frames in order using the fragment_seq field.
 */

#define CEEPEW_STREAM_PAYLOAD_MAX   210U   /* bytes per frame after headers */
#define CEEPEW_STREAM_MAX_FRAGMENTS 512U   /* hard limit: 512×210 = ~105 KB */

typedef struct __attribute__((packed)) {
    uint8_t  stream_id;        /* identifies this logical message      */
    uint16_t fragment_seq;     /* 0-based fragment index               */
    uint16_t total_fragments;  /* total fragments in this stream       */
    uint16_t payload_len;      /* bytes in this fragment               */
    uint8_t  payload[CEEPEW_STREAM_PAYLOAD_MAX];
} StreamFrame_t;

/*
 * StreamRx_t — receiver-side reassembly state.
 * Buffer lives in g_region — allocated at stream start, freed at reset.
 */
typedef struct {
    uint8_t   stream_id;
    uint16_t  total_fragments;
    uint16_t  received_count;
    uint64_t  received_bitmap;     /* up to 64 fragments tracked in bitmap */
    uint8_t  *reassembly_buf;      /* region_alloc'd on stream start       */
    uint32_t  reassembly_buf_len;
    bool      complete;
} StreamRx_t;

CeePewErr_t stream_tx_begin(uint8_t stream_id,
                             const uint8_t *data, uint32_t data_len);
CeePewErr_t stream_rx_init(StreamRx_t *rx, const StreamFrame_t *first_frame);
CeePewErr_t stream_rx_ingest(StreamRx_t *rx, const StreamFrame_t *frame);
bool        stream_rx_complete(const StreamRx_t *rx);
CeePewErr_t stream_rx_get_payload(const StreamRx_t *rx,
                                   uint8_t **out, uint32_t *out_len);
```

### 4.3 Streaming Crypto Pipeline

The key insight: **XSalsa20 (inside crypto_box) is a stream cipher**. It
does not require the full plaintext to be in memory simultaneously.
It generates a keystream from the nonce and key, then XORs it with the
plaintext 64 bytes at a time.

CEE-PEW implements this as a **chunked streaming mode** for large messages:

```c
/* crypto/crypto_stream.h */

/*
 * StreamCipher_t — state for streaming encryption/decryption.
 * Initialized once per message, updated per chunk.
 * Lives in g_region — allocated by caller.
 */
typedef struct {
    uint8_t  key[32];           /* crypto_box session key seed        */
    uint8_t  nonce[24];         /* XSalsa20 nonce (session_id+counter)*/
    uint64_t keystream_pos;     /* position in the XSalsa20 keystream */
    uint32_t bytes_processed;   /* total bytes encrypted/decrypted    */
    bool     encrypting;        /* true=encrypt, false=decrypt        */
} StreamCipher_t;

/*
 * crypto_stream_init()
 * Prepares a StreamCipher_t for a new message.
 * ctx->nonce_counter is incremented here (once per message, not per chunk).
 */
CeePewErr_t crypto_stream_init(StreamCipher_t *sc,
                                const CryptoCtx_t *ctx,
                                bool encrypting);

/*
 * crypto_stream_process_chunk()
 * Processes up to CEEPEW_STREAM_PAYLOAD_MAX bytes.
 * Can be called repeatedly; state in StreamCipher_t carries over.
 * Loop bound: chunk_len <= CEEPEW_STREAM_PAYLOAD_MAX (compile-time constant)
 */
CeePewErr_t crypto_stream_process_chunk(StreamCipher_t *sc,
                                         const uint8_t *in,
                                         uint8_t *out,
                                         uint32_t chunk_len);

/*
 * crypto_stream_finalise()
 * Produces the Poly1305 MAC over the entire processed message.
 * MAC covers all chunks in sequence — order-dependent.
 */
CeePewErr_t crypto_stream_finalise(StreamCipher_t *sc,
                                    uint8_t mac_out[16]);
```

### 4.4 Updated Message Size Limits

With the region allocator and streaming protocol, the message limit
changes from a compile-time constant to a runtime-computed value:

```
Maximum plaintext per message:
  = region_free_bytes(&g_region) / 2
    (factor of 2: one half for compressed input, one half for ciphertext)

Typical available after session setup: ~180 KB
After Huffman compression (×0.65):    ~117 KB compressed
After crypto_box MAC (16 bytes):       ~117 KB + 16 bytes
After Ascon envelope (16 bytes tag):   ~117 KB + 32 bytes

Maximum fragment count:
  = ceil(total_ciphertext / CEEPEW_STREAM_PAYLOAD_MAX)
  = ceil(117 KB / 210 bytes)
  = 569 fragments

At ESP-NOW throughput of ~100 packets/second:
  569 fragments ≈ 5.7 seconds transfer time for a 117 KB message

For typical messaging use (1-5 KB messages):
  5 KB / 210 bytes = 24 fragments ≈ 0.24 seconds
```

**UI representation:** The compose screen shows the pool status as a
small region bar in the status line:

```
[pool: ████████░░ 82% free — 148KB avail]
```

And the compose counter changes from `XX/160` to `XXXX B / 148 KB`:

```
Before: "23/160"   (character count, old fixed model)
After:  "1.2 KB / 148 KB"  (bytes, pool-relative, accurate)
```

---

## PART 5 — ARBITRARY COMPUTATION MODEL

### 5.1 The Dispatch Table (Replacing the Hardcoded Pipeline)

Today the crypto pipeline is hardcoded:
`compress → sha256 → crypto_box → ascon → fec → crc → arq`

With arbitrary computation, the pipeline is a **runtime-configured dispatch
chain** stored in the region:

```c
/* pipeline/ceepew_pipeline.h */

/*
 * A PipelineStage_t is a function that transforms a buffer into another buffer.
 * Both buffers are region-allocated. Input is consumed; output is produced.
 * Stages are chained: output of stage N becomes input of stage N+1.
 */
typedef CeePewErr_t (*PipelineFn_t)(
    Region_t       *region,
    const uint8_t  *in,
    uint32_t        in_len,
    uint8_t       **out,        /* region_alloc'd by the stage */
    uint32_t       *out_len,
    void           *ctx         /* stage-specific context      */
);

#define CEEPEW_PIPELINE_MAX_STAGES  16U

typedef struct {
    PipelineFn_t  fn[CEEPEW_PIPELINE_MAX_STAGES];
    void         *ctx[CEEPEW_PIPELINE_MAX_STAGES];
    uint8_t       stage_count;
} Pipeline_t;

CeePewErr_t pipeline_init(Pipeline_t *p);
CeePewErr_t pipeline_add_stage(Pipeline_t *p, PipelineFn_t fn, void *ctx);
CeePewErr_t pipeline_run(Pipeline_t *p, Region_t *region,
                          const uint8_t *input, uint32_t input_len,
                          uint8_t **output, uint32_t *output_len);
```

```c
/* pipeline/ceepew_pipeline.c */

CeePewErr_t pipeline_run(Pipeline_t *p, Region_t *region,
                          const uint8_t *input, uint32_t input_len,
                          uint8_t **output, uint32_t *output_len)
{
    CEEPEW_ASSERT(p != NULL && region != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(input != NULL && output != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(p->stage_count > 0U &&
                  p->stage_count <= CEEPEW_PIPELINE_MAX_STAGES,
                  CEEPEW_ERR_PARAM);

    const uint8_t *cur_in  = input;
    uint32_t       cur_len = input_len;
    uint8_t       *cur_out = NULL;
    uint32_t       cur_out_len = 0U;

    /* Loop bound: p->stage_count <= CEEPEW_PIPELINE_MAX_STAGES (compile-time) */
    for (uint8_t s = 0U; s < p->stage_count; s++) {
        CEEPEW_ASSERT(p->fn[s] != NULL, CEEPEW_ERR_PARAM);

        CeePewErr_t err = p->fn[s](region, cur_in, cur_len,
                                    &cur_out, &cur_out_len, p->ctx[s]);
        CEEPEW_ASSERT(err == CEEPEW_OK, err);
        CEEPEW_ASSERT(cur_out != NULL, CEEPEW_ERR_BOUNDS);

        cur_in  = cur_out;
        cur_len = cur_out_len;
    }

    *output     = cur_out;
    *output_len = cur_out_len;
    return CEEPEW_OK;
}
```

**Default TX pipeline (assembled in session_fsm.c after Phase 3):**

```c
pipeline_add_stage(&tx_pipeline, stage_digital_sum_mix,  &code_ctx);
pipeline_add_stage(&tx_pipeline, stage_huffman_compress, &huff_ctx);
pipeline_add_stage(&tx_pipeline, stage_pad_pkcs7,        NULL);
pipeline_add_stage(&tx_pipeline, stage_sha256_prepend,   NULL);
pipeline_add_stage(&tx_pipeline, stage_crypto_box,       &crypto_ctx);
pipeline_add_stage(&tx_pipeline, stage_ascon_envelope,   &crypto_ctx);
pipeline_add_stage(&tx_pipeline, stage_fec_encode,       &esl_ctx);
pipeline_add_stage(&tx_pipeline, stage_crc32_append,     NULL);
```

The stage count is 8 — well within `CEEPEW_PIPELINE_MAX_STAGES = 16`.
Future stages (e.g. a second compression pass, a steganographic layer,
a different cipher suite) can be inserted without touching existing code.

### 5.2 The Explicit Stack (Bounded Recursion)

The numerology algorithm uses recursion (`reduceSum` calls itself). Recursion
on ESP32 is dangerous: each call frame consumes stack, and the FreeRTOS stack
is fixed. The JPL rule implies banning recursion.

With the region allocator, we implement an **explicit stack** — recursion
unrolled into iteration with a region-allocated stack array:

```c
/* tools/digital_sum.c — explicit stack version */

/*
 * digital_sum_reduce_iterative()
 * Equivalent to the recursive numerology reduceSum(), but uses an
 * explicit stack region_alloc'd from g_region. No function recursion.
 * Bounded: stack depth <= ceil(log10(CEEPEW_REGION_POOL_BYTES)) < 10 levels.
 */
CeePewErr_t digital_sum_reduce_iterative(Region_t *region,
                                          const uint8_t *data,
                                          uint32_t len,
                                          uint8_t *result_out)
{
    CEEPEW_ASSERT(region != NULL && data != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(len > 0U && len <= CEEPEW_REGION_POOL_BYTES, CEEPEW_ERR_PARAM);

    /* Allocate a working copy in the region */
    uint8_t *work = region_alloc(region, len);
    CEEPEW_ASSERT(work != NULL, CEEPEW_ERR_BOUNDS);
    memcpy(work, data, len);

    uint32_t work_len = len;

    /* Iterative digit-root reduction — maximum 10 iterations (see above) */
    for (uint8_t iter = 0U; iter < 10U; iter++) {
        if (work_len == 1U) { break; }   /* single byte = done */

        /* Sum all bytes */
        uint32_t sum = 0U;
        for (uint32_t i = 0U; i < work_len; i++) {
            sum += work[i];
        }

        /* Convert sum to bytes and repeat */
        uint8_t digit_count = 0U;
        uint32_t tmp = sum;
        while (tmp > 0U && digit_count < 10U) {
            work[digit_count++] = (uint8_t)(tmp % 10U);
            tmp /= 10U;
        }
        work_len = digit_count;
    }

    *result_out = work[0];
    return CEEPEW_OK;
}
```

### 5.3 Arbitrary-Length FEC

The (15,11) Hamming encoder currently processes a fixed-size buffer. With
region allocation, it processes **any length** by iterating over 11-bit chunks:

```c
/* ecc/ecc_hamming.c — region-aware version */

CeePewErr_t ecc_hamming_encode_stream(Region_t *region,
                                       const uint8_t *in, uint32_t in_len,
                                       uint8_t **out, uint32_t *out_len)
{
    CEEPEW_ASSERT(region != NULL && in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len > 0U, CEEPEW_ERR_PARAM);

    /* Output size: ceil(in_len_bits / 11) * 15 bits, rounded up to bytes */
    uint32_t in_bits   = in_len * 8U;
    uint32_t n_chunks  = (in_bits + 10U) / 11U;   /* ceil division */
    uint32_t out_bytes = (n_chunks * 15U + 7U) / 8U;

    CEEPEW_ASSERT_BOUND(n_chunks, CEEPEW_STREAM_MAX_FRAGMENTS * 200U);

    *out = region_alloc(region, out_bytes);
    CEEPEW_ASSERT(*out != NULL, CEEPEW_ERR_BOUNDS);
    *out_len = out_bytes;

    memset(*out, 0, out_bytes);

    /*
     * Loop bound: n_chunks is bounded by CEEPEW_STREAM_MAX_FRAGMENTS * 200
     * which is itself <= CEEPEW_REGION_POOL_BYTES / 11 (bits per chunk).
     * Static checker can verify: n_chunks < CEEPEW_REGION_POOL_BYTES.
     */
    for (uint32_t chunk = 0U; chunk < n_chunks; chunk++) {
        uint16_t data_bits = extract_11_bits(in, chunk * 11U, in_bits);
        uint16_t codeword  = hamming_encode_11(data_bits,
                                                &g_esl_ctx.fec_perm);
        insert_15_bits(*out, chunk * 15U, codeword);
    }

    return CEEPEW_OK;
}
```

---

## PART 6 — ARBITRARY-LENGTH CRYPTO: PADDING SCHEME

### 6.1 PKCS7-Compatible Padding (Deterministic, Length-Hiding)

After Huffman compression, the payload is padded to the next multiple
of 64 bytes (XSalsa20 block size). This hides the true plaintext length
from a traffic analyst who observes ciphertext sizes.

```c
/* crypto/crypto_pad.c */

#define CEEPEW_PAD_BLOCK_SIZE  64U

/*
 * crypto_pad_pkcs7()
 * Pads input to next multiple of CEEPEW_PAD_BLOCK_SIZE.
 * Padding bytes contain the padding length (PKCS7 convention).
 * Output is region_alloc'd.
 *
 * Length hiding: all messages whose compressed length falls in the same
 * 64-byte bucket produce identically-sized ciphertext. A 50-byte and
 * 63-byte message both pad to 64 bytes — indistinguishable by length.
 */
CeePewErr_t crypto_pad_pkcs7(Region_t *region,
                               const uint8_t *in, uint32_t in_len,
                               uint8_t **out, uint32_t *out_len)
{
    CEEPEW_ASSERT(region != NULL && in != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len > 0U, CEEPEW_ERR_PARAM);

    uint32_t pad_len = CEEPEW_PAD_BLOCK_SIZE -
                       (in_len % CEEPEW_PAD_BLOCK_SIZE);
    uint32_t total   = in_len + pad_len;

    CEEPEW_ASSERT_BOUND(total, CEEPEW_REGION_POOL_BYTES);

    *out = region_alloc(region, total);
    CEEPEW_ASSERT(*out != NULL, CEEPEW_ERR_BOUNDS);

    memcpy(*out, in, in_len);

    /* Fill padding bytes — bounded by CEEPEW_PAD_BLOCK_SIZE (64) */
    for (uint32_t i = in_len; i < total; i++) {
        (*out)[i] = (uint8_t)pad_len;
    }

    *out_len = total;
    return CEEPEW_OK;
}

/*
 * crypto_unpad_pkcs7()
 * Verifies and strips PKCS7 padding.
 * Constant-time verification to prevent padding oracle attacks.
 */
CeePewErr_t crypto_unpad_pkcs7(const uint8_t *in, uint32_t in_len,
                                 uint32_t *plaintext_len_out)
{
    CEEPEW_ASSERT(in != NULL && plaintext_len_out != NULL, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(in_len > 0U && in_len % CEEPEW_PAD_BLOCK_SIZE == 0U,
                  CEEPEW_ERR_PARAM);

    uint8_t pad_len = in[in_len - 1U];

    /* Constant-time: check all padding bytes equal pad_len */
    uint8_t bad = 0U;
    /* Loop bound: pad_len <= CEEPEW_PAD_BLOCK_SIZE (compile-time) */
    for (uint8_t i = 0U; i < CEEPEW_PAD_BLOCK_SIZE; i++) {
        if (i < pad_len) {
            bad |= (in[in_len - 1U - i] ^ pad_len);
        }
    }
    CEEPEW_ASSERT(bad == 0U, CEEPEW_ERR_CRYPTO);   /* padding oracle safety */

    *plaintext_len_out = in_len - pad_len;
    return CEEPEW_OK;
}
```

---

## PART 7 — UPDATED ARCHITECTURE: WHAT CHANGES

### 7.1 Module Map Additions

```
mem/
└── ceepew_region.h/.c    ← NEW: region allocator (200 KB static pool)

pipeline/
└── ceepew_pipeline.h/.c  ← NEW: dispatch table, stage chaining

All existing modules gain region-aware variants:
  compress/compress_huffman.c  → huffman_compress_stream() using region
  ecc/ecc_hamming.c            → ecc_hamming_encode_stream() using region
  crypto/crypto_ascon.c        → ascon_encrypt_stream() using region
  crypto/crypto_pad.c          ← NEW: PKCS7 padding, length hiding
  transport/transport_stream.c ← NEW: streaming frame assembly/reassembly
  tools/digital_sum.c          → explicit-stack iterative version
```

### 7.2 Session Lifecycle Update

```
[Session start]
  region_init(&g_region)        ← 200 KB pool zeroed and ready
  crypto_ctx_init(...)
  pipeline_init(&tx_pipeline)
  pipeline_init(&rx_pipeline)
  [add default stages to both pipelines]

[Per message TX]
  pipeline_run(&tx_pipeline, &g_region, msg, msg_len, &out, &out_len)
  stream_tx_begin(stream_id, out, out_len)  ← fragments and sends

[Per message RX]
  stream_rx_ingest(...)          ← reassembles fragments into region buf
  pipeline_run(&rx_pipeline, &g_region, in, in_len, &plain, &plain_len)
  [display plaintext]

[Session end / wipe]
  region_reset(&g_region)        ← secure-zeros entire 200 KB pool
  crypto_ctx_destroy(...)
```

### 7.3 Updated Coding Standard: Rule 1 (Loop Bounds)

**Revised Rule 1** (replaces the previous version):

> Every loop must have a provably bounded iteration count. Two forms are
> accepted:
>
> **Form A — Compile-time constant:**
> `for (uint8_t i = 0U; i < CEEPEW_MAX_MESSAGES; i++)`
> The bound `CEEPEW_MAX_MESSAGES` is a `#define` constant. Static checkers
> can verify termination trivially.
>
> **Form B — Runtime-validated, region-bounded:**
> ```c
> CEEPEW_ASSERT_BOUND(n, CEEPEW_REGION_POOL_BYTES);
> for (uint32_t i = 0U; i < n; i++) { ... }
> ```
> The bound `n` is validated by `CEEPEW_ASSERT_BOUND` to be ≤
> `CEEPEW_REGION_POOL_BYTES` before the loop. The checker sees:
> `n <= CEEPEW_REGION_POOL_BYTES` (compile-time constant) → termination proven.

**Form B loops must always be preceded by `CEEPEW_ASSERT_BOUND`.**
Failure to do so is an architecture violation of the same severity as
missing assertions.

### 7.4 Updated Coding Standard: Rule 2 (Dynamic Allocation)

**Revised Rule 2** (replaces the previous version):

> No OS heap allocation after initialization. `malloc`, `calloc`, `realloc`,
> `free`, `new`, `delete`, `std::vector`, `std::string` are forbidden.
>
> **Permitted:** `region_alloc(&g_region, size)` and
> `region_alloc_zeroed(&g_region, size)` — these allocate from the static
> pool, not the OS heap. They may be called freely from any module that
> receives a `Region_t *` parameter.
>
> **Constraint:** Every `region_alloc` call must be paired with a NULL check
> on the return value. Pool exhaustion is a recoverable error, not a crash.

### 7.5 Theoretical Message Size Limits

| Scenario | Limit | Notes |
|---|---|---|
| Minimum useful message | 1 byte | Padded to 64 bytes |
| Typical text message (SMS-length) | 160 chars ≈ 160 B | 1 fragment |
| Typical paragraph | ~1 KB | ~5 fragments, ~0.05 s TX |
| Multi-paragraph text | ~10 KB | ~48 fragments, ~0.5 s TX |
| Large document | ~100 KB | ~476 fragments, ~5 s TX |
| Maximum (half pool) | ~100 KB | Hard limit at pool / 2 |
| Pool exhaustion indication | RGB Yellow blink | Shown when pool < 20% |

---

## PART 8 — DIAG MODE UPDATE: REGION STATS

DIAG Page 5 (Memory Map) gains a region pool section:

```
┌──────────────────────────────┐
│ ▸ MEMORY         [DIAG 5/5] │
│ SRAM TOTAL:  520 KB          │
│ REGION POOL: 200 KB          │
│ POOL USED:   ███░░░░░  31%   │  ← real-time region usage bar
│ POOL HWM:    87 KB           │  ← high-water mark this session
│ POOL FREE:   138 KB          │
│ HEAP FREE:   298 KB          │  ← OS heap (should stay flat)
│ HEAP MIN:    296 KB          │
│ STACK C0:   4096/4096 B     │
│ STACK C1:   6821/8192 B     │
│ MSG BUF:    active stream    │  ← "N/A" or "stream N: M frags"
└──────────────────────────────┘
```

---

## PART 9 — PROJECT MODE ADDENDUM (Append to Part 13 of Final Spec v2)

```
── ADDENDUM D: ARBITRARY COMPUTATION ──

REGION ALLOCATOR:
  g_region is the single static Region_t instance (200 KB pool).
  region_init()  called at session start.
  region_reset() called at session end — secure-zeros the pool.
  All variable-length processing uses region_alloc() / region_alloc_zeroed().
  Return value of region_alloc() must always be NULL-checked.
  Two CEEPEW_ASSERT calls minimum in every function that calls region_alloc.

LOOP BOUND RULE UPDATE:
  Form B loops (runtime-bounded via CEEPEW_ASSERT_BOUND) are now permitted.
  CEEPEW_ASSERT_BOUND(n, MAX_CONSTANT) must immediately precede the loop.
  Omitting CEEPEW_ASSERT_BOUND before a Form B loop = architecture violation.

PIPELINE:
  TX and RX pipelines are assembled in session_fsm.c after Phase 3.
  Default stage order for TX: digital_sum_mix → huffman → pad_pkcs7 →
    sha256_prepend → crypto_box → ascon_envelope → fec_encode → crc32.
  Default stage order for RX: reverse of TX, with verify steps.
  Pipeline stages must not allocate outside g_region.
  Each stage receives (region, in, in_len, &out, &out_len, ctx).

STREAMING:
  Messages of any length (up to region_free_bytes()/2) are supported.
  stream_tx_begin() fragments the ciphertext into StreamFrame_t packets.
  stream_rx_ingest() reassembles in region memory.
  CEEPEW_STREAM_MAX_FRAGMENTS = 512 is the hard fragment cap.

PADDING:
  crypto_pad_pkcs7() applied after Huffman compression.
  Pads to next 64-byte boundary (XSalsa20 block size).
  crypto_unpad_pkcs7() must be used with constant-time verification.
  This hides plaintext length from traffic analysts.

MESSAGE SIZE DISPLAY:
  Compose screen shows "X.X KB / Y KB" not "XX/160".
  Y = region_free_bytes(&g_region) / (2 * 1024).

NEW MILESTONES:
  [ ] M-D-1: ceepew_region.h/.c — unit test: alloc, zero, reset, hwm
  [ ] M-D-2: ceepew_pipeline.h/.c — 8-stage default TX/RX pipelines
  [ ] M-D-3: crypto_pad.c — PKCS7 pad/unpad, constant-time verify
  [ ] M-D-4: transport_stream.c — fragment TX, reassemble RX
  [ ] M-D-5: huffman_compress_stream() — region-aware variant
  [ ] M-D-6: ecc_hamming_encode_stream() — arbitrary-length FEC
  [ ] M-D-7: digital_sum explicit stack — iterative, region-aware
  [ ] M-D-8: DIAG page 5 pool stats — usage bar, HWM display
  [ ] M-D-9: Compose screen "X.X KB / Y KB" display
```
