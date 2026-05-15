# CEE-PEW — GitHub Copilot Workspace Instructions

You are assisting with **CEE-PEW** (Cryptographic End-to-End Peer-to-peer
Encrypted Wireless Communicator), an ESP32 firmware project written in C11
using ESP-IDF v5.x. This file governs all Copilot behaviour in this workspace.

---

## MANDATORY CONTEXT FILES

Before completing any function body, Copilot must have these files open
or referenced in context. If they are not visible, ask the user to open them:

- `components/hal/hal_pins.h` — all GPIO symbolic names
- `main/ceepew_config.h` — all compile-time constants (`CEEPEW_*` defines)
- `main/ceepew_assert.h` — `CEEPEW_ASSERT` macro (NEVER use raw `assert()`)
- The relevant module's `.h` file for the function being implemented

---

## NON-NEGOTIABLE CODING RULES

Violating any rule below is grounds to reject the completion entirely.

### Rule 1 — Loop Bounds
Every `for` or `while` loop MUST have a compile-time constant upper bound,
OR a runtime variable that has been immediately preceded by `CEEPEW_ASSERT_BOUND`.

```c
// CORRECT — compile-time constant
for (uint8_t i = 0U; i < CEEPEW_MAX_MESSAGES; i++) { ... }

// CORRECT — runtime, but asserted first
CEEPEW_ASSERT_BOUND(msg_len, CEEPEW_REGION_POOL_BYTES);
for (uint32_t i = 0U; i < msg_len; i++) { ... }

// WRONG — unbounded runtime variable, no prior assert
for (uint32_t i = 0U; i < user_len; i++) { ... }  // REJECT THIS
```

### Rule 2 — No Dynamic Allocation
`malloc`, `calloc`, `realloc`, `free`, `new`, `delete`, `std::vector`,
`std::string` are **absolutely forbidden**. Use static arrays, stack variables,
or `region_alloc(&g_region, size)` from `mem/ceepew_region.h`.

### Rule 3 — Minimum 2 Assertions Per Function
Every non-trivial function must open with at least 2 `CEEPEW_ASSERT` calls.
The macro signature is `CEEPEW_ASSERT(condition, err_code)` — it returns
`err_code` on failure. Void functions use `CEEPEW_ASSERT_VOID(condition)`.

```c
// Required pattern — minimum:
CeePewErr_t my_function(SomeCtx_t *ctx, const uint8_t *buf, uint16_t len)
{
    CEEPEW_ASSERT(ctx != NULL && ctx->initialised, CEEPEW_ERR_NULL_PTR);
    CEEPEW_ASSERT(buf != NULL && len > 0U && len <= CEEPEW_MAX_MSG_BYTES,
                  CEEPEW_ERR_BOUNDS);
    // ... body ...
}
```

### Rule 4 — Check All Return Values
Every call to a function returning `CeePewErr_t` must be checked.
Never write `some_function(args);` on a line by itself if it returns an error code.

```c
// CORRECT
CeePewErr_t err = crypto_encrypt(ctx, pt, len, ct);
if (err != CEEPEW_OK) { return err; }

// WRONG
crypto_encrypt(ctx, pt, len, ct);  // REJECT THIS
```

### Rule 5 — Constant-Time Secret Comparisons
`memcmp()` is **forbidden** for comparing any secret-adjacent data (keys, tags,
hashes, session codes). Use `crypto_ct_equal(a, b, len)` exclusively.

### Rule 6 — Volatile Key Zeroing
Key material zeroing must use `secure_zero(ptr, len)` with a memory barrier,
never bare `memset()`.

### Rule 7 — Packed Wire Structs
All network/wire-format structs require `__attribute__((packed))`.

### Rule 8 — No Raw GPIO Numbers
GPIO numbers are only permitted in `hal/hal_pins.h`. Everywhere else,
use the `CEEPEW_PIN_*` symbolic names.

### Rule 9 — C11 Only
No C++. No Arduino APIs (`String`, `delay()`, `millis()`, `analogRead()`).
Use ESP-IDF v5 C APIs: `esp_timer_get_time()`, `vTaskDelay()`, `adc1_get_raw()`.

### Rule 10 — Function Naming
`module_verb_noun` convention: `crypto_encrypt_message`, `diag_render_page`,
`input_consume_click`. Constants: `CEEPEW_MODULE_CONSTANT_NAME`. Types:
`PascalCase_t` (e.g. `CryptoCtx_t`, `DiagCtx_t`).

---

## INSTANT REJECT CHECKLIST

Before accepting any Copilot completion, mentally scan for:

| Red flag | Action |
|---|---|
| `malloc()` / `free()` / `new` / `delete` | Reject entirely |
| `memcmp()` on anything key-adjacent | Replace with `crypto_ct_equal()` |
| Raw `GPIO_NUM_34` outside `hal_pins.h` | Reject, use `CEEPEW_PIN_POT` |
| `assert()` instead of `CEEPEW_ASSERT()` | Reject |
| `String`, `std::vector`, `std::string` | Reject |
| `delay()`, `millis()`, `analogRead()` | Reject |
| Loop with no bound assertion | Reject |
| Function with 0 or 1 assertions | Reject |
| Unchecked `CeePewErr_t` return | Reject |
| `memset()` on key buffer without `secure_zero()` | Reject |

---

## COMPLETION PATTERN (Use This as Copilot Primer)

Always write this much before triggering Copilot completion:

```c
/* components/[module]/[file].c */

#include "[module].h"
#include "ceepew_config.h"
#include "ceepew_assert.h"

CeePewErr_t module_verb_noun(ModuleCtx_t *ctx, const uint8_t *param, uint16_t len)
{
    /* Assertion 1: context pointer and state invariant */
    CEEPEW_ASSERT(ctx != NULL && ctx->initialised, CEEPEW_ERR_NULL_PTR);

    /* Assertion 2: input parameter bounds */
    CEEPEW_ASSERT(param != NULL && len > 0U && len <= CEEPEW_MAX_MSG_BYTES,
                  CEEPEW_ERR_BOUNDS);

    /* Implementation: */
    // ← trigger Copilot here
```

Copilot will maintain the pattern established above this point.

---

## MODULE DEPENDENCY MAP (No Upward Imports)

```
ui          → hal, session (via queues)
session     → crypto, transport, compress, session_memory, tools
crypto      → hal_rng, tweetnacl
ecc         → (none)
transport   → hal_radio, ecc
compress    → (none)
tools       → (none)
mem         → (none)
pipeline    → mem, crypto, compress, ecc
hal         → ESP-IDF only
```

A `#include` that violates this hierarchy is an architecture error.

---

## SECURITY NOTES FOR COPILOT

When implementing anything touching: key generation, nonce handling, message
authentication, session establishment, or transport framing — Copilot must
add an inline comment identifying which threat is addressed:

```c
/* SECURITY: nonce counter checked here — prevents nonce reuse (threat: key reuse) */
CEEPEW_ASSERT(ctx->nonce_counter < CEEPEW_NONCE_HARD_LIMIT, CEEPEW_ERR_NONCE_EXHAUSTED);
ctx->nonce_counter++;
```

---

*This file is read automatically by GitHub Copilot Chat for every prompt
in this workspace. Do not delete or move it.*
