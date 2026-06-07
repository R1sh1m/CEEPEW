# components/hal — public HAL contract (headers only)

This directory is **headers only**. It contains the public HAL API that
all other components (including `ceepew_hal`) consume:

- `hal_pins.h`  — pin numbering and wiring (the single source of truth; includes `_Static_assert`s for conflicts and a `hal_pins_validate()` runtime check).
- `hal_gpio.h`  — GPIO init/control API.
- `hal_rng.h`   — RNG init/read API.

All `.c` implementations live in `components/ceepew_hal/`. That component's
`CMakeLists.txt` adds `../hal` to its include path so the public headers
resolve transparently from the implementation side.

## Rules

- Do NOT add `.c` files here. Implementations belong in `ceepew_hal`.
- Public APIs in this folder are stable; changes here are a contract break.
- Anything that needs to live on the radio (packed wire structs, channel-hop tables, etc.) is NOT a HAL concern and does not belong here.
