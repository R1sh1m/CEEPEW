# tests/ — CEE-PEW On-Device Test Suite

The `tests/` component is the project's diagnostic harness. It is a
gated build of 16 internal test files, compiled into the firmware only
when `CONFIG_CEEPEW_BUILD_TESTS` is set in `menuconfig` → "CEE-PEW
Build Options" → "Diagnostic Mode". With the option off, this component
contributes nothing to the final binary.

## How to run

1. `idf.py menuconfig`
2. `CEE-PEW Build Options` → `Diagnostic Mode` → `[*]`
3. Save, exit.
4. `idf.py build flash monitor -p COM5`
5. Watch the serial log; the auto-runner prints a structured
   `=== DIAGNOSTIC REPORT ===` block and then reboots after 5 s.

The companion script `diagnose.ps1` automates the menuconfig / build /
flash / monitor / parse cycle and exits with a non-zero status if any
subsystem reports `FAIL`.

## Subsystem coverage

| Subsystem           | File(s)                                           | What it checks                                                                                  |
|---------------------|---------------------------------------------------|-------------------------------------------------------------------------------------------------|
| Session FSM         | tests/main/integration_test_e2e.c                 | phase 1/2 transitions, key derivation, MAC lock, termination                                     |
| Nonce Enforcement   | tests/main/integration_test_e2e.c                 | 10-iteration `enforce_nonce_limit` call loop, counter increments correctly                       |
| Replay Window       | tests/main/test_replay.c, test_replay_comprehensive.c | 64-bit WG-style sliding window: accept in-order, reject duplicates, accept old-but-not-seen     |
| Pipeline/Memory     | tests/main/test_pipeline_memory.c                 | region allocator init/alloc/reset; pipeline buffer ownership                                    |
| HKDF/Key Derive     | tests/main/test_pipeline_memory.c                 | HKDF-SHA256 expand roundtrip; known-answer vectors                                              |
| Transport Hop       | tests/main/test_hop.c, test_transport_hop.c       | PRG permutation is deterministic; both peers see the same channel sequence for a given session  |
| Power/Wakeup        | tests/main/test_power.c                           | sleep/deep-sleep transitions; wake source decode                                                |
| Pairing UI          | tests/main/integration_test_pairing_ui.c          | digit entry, countdown, confirm; full state machine coverage                                     |
| Curve25519 (RFC)    | tests/components/crypto/curve25519_test.c         | RFC 7748 vector on the project's X25519 implementation                                          |
| UI Manager          | tests/components/ceepew_hal/ui_manager_test.c     | every state transition; STACKS page renders; bus_recover path                                   |
| UI Cryptogram       | tests/components/ceepew_hal/ui_cryptogram_test.c  | 6-digit cryptogram render; field/frame encode                                                    |
| Pairing Handoff     | tests/main/test_pairing_handoff.c                | regression: the handoff bug from Sprint 19 stays fixed                                          |
| Pairing Converge    | tests/main/test_pairing_convergence.c             | regression: two-device pairing converges to a shared key within 3 attempts                      |
| Temp Sensor         | tests/components/ceepew_hal/hal_temp_test.c       | on-die sensor returns plausible value; NULL guard; init idempotency                             |

## The 15th line item — `hal_oled`

| Subsystem   | File                                       | What it checks                                                 |
|-------------|--------------------------------------------|----------------------------------------------------------------|
| OLED Facade | tests/components/ceepew_hal/hal_oled_test.c | `hal_oled_*` symbols are linked; dimensions match the spec     |

The facade has no real "behaviour" to test (it's a one-line forwarder to
`ceepew_oled_*`); the test verifies the wiring.

## Code conventions

- Every test file MUST define its entry point as `void NAME_selftest_run(void)` — never as a constructor. The compiler-enforced
  ban on `__attribute__((constructor))` is documented in `.github/copilot-instructions.md`.
- Every test file MUST guard its body with `#ifdef CEEPEW_ENABLE_SELFTEST`. The macro is set in `tests/CMakeLists.txt` only when
  `CONFIG_CEEPEW_BUILD_TESTS=y`, so test code is invisible to the
  compiler when the option is off.
- Tests print PASS/FAIL with a leading `CEEPEW: ` prefix so `diagnose.ps1`
  can `Select-String` the serial log without ambiguity.
- Tests do not `malloc`; if you need a buffer, declare it `static`.

## Reading the DIAGNOSTIC REPORT

The on-boot `app_main` calls `integration_tests_run_all()` which
emits, in order:

```
=== DIAGNOSTIC REPORT ===
  [Session FSM      ] PASS
  [Nonce Enforcement] PASS
  [Replay Window    ] PASS
  [Pipeline/Memory  ] PASS
  [HKDF/Key Derive  ] PASS
  [Transport Hop    ] PASS
  [Power/Wakeup     ] PASS
  [Pairing UI       ] PASS
  [Curve25519 (RFC) ] PASS
  [UI Manager       ] PASS
  [UI Cryptogram    ] PASS
  [Pairing Handoff  ] PASS
  [Pairing Converge ] PASS
  [Temp Sensor      ] PASS
  Total: 14 / 14 subsystems passed
=========================
```

Each line is machine-grepable: starts with `"  ["`, ends with `"PASS"`
or `"FAIL"`. `diagnose.ps1` regex-matches every line and reports the
first failure.

## AI-agent workflow

When triaging a bug report, an AI agent can:

1. Run `idf.py menuconfig` to enable Diagnostic Mode.
2. Flash the binary.
3. Pipe `idf.py monitor -p COM5` through `diagnose.ps1` (or any
   `Select-String` over the `DIAGNOSTIC REPORT` block).
4. The first `FAIL` line tells you which subsystem to grep:

```
rg -l 'subsystem_keyword' main/ components/ tests/
```

See `.github/copilot-instructions.md` for the module dependency DAG
when narrowing down a fail.

## Production policy

`sdkconfig.production` is the production build configuration. It
contains:

```
# CONFIG_CEEPEW_BUILD_TESTS is not set
```

A production build that accidentally enables the option will fail its
`idf.py build` validation in CI (the `tests/` component will pull in
test code, and the binary size budget will be exceeded). Do not delete
this line.
