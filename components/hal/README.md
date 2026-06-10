# components/hal — public HAL contract (headers only)

This directory contains the public HAL API that all other components
(including `ceepew_hal`) consume:

- `hal_pins.h`  — pin numbering and wiring (the single source of truth; includes `_Static_assert`s for conflicts and a `hal_pins_validate()` runtime check).
- `hal_gpio.h`  — GPIO init/control API.
- `hal_rng.h`   — RNG init/read API.

This directory is **NOT** a registered ESP-IDF component (no
`CMakeLists.txt`). The headers are exposed via `components/ceepew_hal/`'s
`INCLUDE_DIRS "../hal"` (which makes `components/hal/` a transitive
public include path of `ceepew_hal`).

All `.c` implementations live in `components/ceepew_hal/`. That component
adds `../hal` to its include path so the public headers resolve
transparently from the implementation side, and from anyone who
`REQUIRES ceepew_hal` (the public include path is inherited).

## Rules

- Do NOT add `.c` files here. Implementations belong in `ceepew_hal`.
- Do NOT add a `CMakeLists.txt` here. The name `hal` collides with
  ESP-IDF's own `components/hal/` (the platform-port HAL), so this
  directory must stay a plain headers directory.
- Public APIs in this folder are stable; changes here are a contract break.
- Anything that needs to live on the radio (packed wire structs, channel-hop tables, etc.) is NOT a HAL concern and does not belong here.
