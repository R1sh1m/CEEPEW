# Contributing to CEE-PEW

## Build Setup

See [README.md](README.md#getting-started) for environment setup. The project targets ESP32 with ESP-IDF v6.0.1.

## Development Mode

Build with `CONFIG_CEEPEW_DEVELOPMENT_MODE=y` (via `menuconfig`) to enable:

- On-device test suite (crypto, transport, HAL, memory)
- Integration tests (pairing E2E, UI, replay)
- Debug timeout trigger (3-second button hold on pairing screen)

Production builds should set this to `n`.

## Coding Standards

### Naming

- `snake_case` for all identifiers (functions, variables, types, macros, filenames)
- `CamelCase` only for FreeRTOS/ESP-IDF API calls
- Prefix public module functions with the module name: `session_*`, `transport_ble_*`, `hal_*`
- `CeePewErr_t` typedef for all error codes (`CEEPEW_OK`, `CEEPEW_ERR_*`)
- Constants and macros: `CEEPEW_UPPER_CASE`

### Safety

- All secret material (keys, session data) must be zeroed via `ceepew_secure_zero()` on teardown
- Constant-time comparison (`crypto_ct_equal`) for all tag/MAC/key comparisons — never `memcmp`
- No heap allocation — use the region allocator (`ceepew_region_alloc`)
- All function parameters validated with `CEEPEW_ASSERT`

### Pointers and Bounds

- Parameters returning allocated data use `_out` suffix; plaintext output uses `_out`
- `uint8_t*` for opaque byte buffers, typed pointers for structs
- `uint16_t` for lengths (max payload < 64 KB); checked with `CEEPEW_ASSERT` on entry
- Static analysis prefers `const` for read-only buffer parameters

### Headers

- Public headers in `components/hal/` (namespace `hal_*`)
- Internal headers in each component directory
- Header comment: one-line description, key API summary, security properties

### State Machines

- Session FSM enforces strict phase transitions with assertion guards
- Synchronisation barriers must use the double-ended rendezvous pattern (HELLO/ACK)
- No blocking waits in interrupt context; use FreeRTOS queues for IPC

## Testing

All tests run on-device. To run:

```bash
idf.py menuconfig  # set CONFIG_CEEPEW_DEVELOPMENT_MODE=y
idf.py build flash monitor
```

The diagnostic report is printed at boot. See `tests/README.md` for test coverage details.

## Pull Request Process

1. Ensure all tests pass in development mode
2. Verify the build compiles with `CONFIG_CEEPEW_DEVELOPMENT_MODE=n`
3. Update `SECURITY.md` if the threat model changes
4. Update `docs/ARCHITECTURAL_DEVIATIONS.md` for spec deviations

## Security Issues

See [SECURITY.md](SECURITY.md) for responsible disclosure. Do not file public GitHub issues for security vulnerabilities.
