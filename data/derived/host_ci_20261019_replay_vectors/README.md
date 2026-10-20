# Host replay-vector campaign — 2026-10-19

Purpose: the traceable artefact for registry entry **`EXP-REPLAY-REJECT`**
("100/100 host-injected replay vectors rejected"). It replaces the previous
citation `tests/host/test_replay_window.c`, which contains 4 test functions /
20 assertion calls and **no 100-vector campaign** — the claim had no executed
artefact of that size until this run.

## What was run

- **Campaign:** `tests/host/test_replay_vectors.c` — pre-registered
  4 classes x 25 vectors = 100 replay vectors against the **real**
  `components/transport/transport_replay.c`, compiled natively (host stubs
  for FreeRTOS `portENTER_CRITICAL`, no firmware involved).
- **Pre-registration:** the class design was frozen in the test source
  header before first execution (2026-10-19, design v1); it is also echoed
  at the top of `replay_vectors_log.txt`.
- **Design summary** (window depth W = `CEEPEW_REPLAY_WINDOW_SIZE` = 64;
  production order = pre-decrypt read-only `check()`, then `commit()` only
  after Ascon tag verify, re-checked under the lock):
  - State S1 = 89 ascending commits `E-88..E` (E = 1,000,000), i.e. a live
    stream; afterwards all of `E-63..E` are in-window, `E-88..E-64` aged out.
  - **C1** recent duplicates, distance 0..24 (25) → rejected at `check` (bitmap).
  - **C2** mid-window duplicates, distance 25..49 (25) → rejected at `check` (bitmap).
  - **C3** aged-out duplicates, distance 64..88 (25, all *previously committed*
    frames now below the window) → rejected at `check` (too-old rule).
  - **C4** dup-then-confirm (25 fresh seqs): two frames that both pass
    `check`; first `commit` accepted, second `commit` → `CEEPEW_ERR_REPLAY`
    (the lock-protected re-check path).
  - Positive controls (must ACCEPT, else the matrix only proves a blanket
    rejector): P1 forward advance after the sweeps; P2 never-seen in-window
    value 1000 under a 1050 window edge (backward gap fill); P3 = the 25 C4
    first deliveries. Total 27.

## Result (as measured — no re-runs, no exclusions)

- **100/100 vectors rejected; 75/75 of C1–C3 independently re-rejected by
  `commit()`; 27/27 positive controls accepted; 0 failures; process exit 0.**
- Stage map: C1=CHECK(bitmap) · C2=CHECK(bitmap) · C3=CHECK(too-old) ·
  C4=COMMIT(re-check).
- Full host suite with the campaign added: **15/15 PASS**
  (`python tools/ceepew_pipeline.py --mode host`);
  `ctest`: **100% tests passed out of 20**, total 17.93 s.

## Build conditions (stated exactly)

- Native Windows build via CMake 4.4, generator **MinGW Makefiles**,
  compiler `C:/msys64/ucrt64/bin/gcc.exe`; target compile flags for this
  run are `-g` only.
- **Sanitizers are NOT applied in this environment**: `tests/host/CMakeLists.txt`
  enables `-fsanitize=address,undefined` only when the build is *not* MinGW
  (`if(NOT MINGW)`), and this build is MinGW. Do not describe this run as
  "ASan/UBSan-checked"; it is a plain native run. (The repo-level statement
  about sanitizers applies to non-MinGW GCC/Clang configurations.)
- No hardware, no firmware image, no timing measurement: this is a host
  unit-test campaign of the replay-window module.

## Files (bytes and SHA-256 pin the exact captured bytes)

| File | Bytes | SHA-256 |
|------|-------|---------|
| `replay_vectors_log.txt` | 15850 | `fc54a67ec4bcec2f7c9dbc9e4d606bb9bab267684e0702fbb824dfbe2febd1e5` |
| `host_test_log.txt` | 4788 | `588db71f977e29dbd0ec7252f5fe3e9f9e2b0487fe5e07c4b4177e1704ea4f82` |
| `ctest_full_log.txt` | 4562 | `2e796bdf52fb74658234059466a75f70046dffaa8f735eb2ee67924eadc093e0` |

- `replay_vectors_log.txt` — direct execution of
  `tests/host/build_host/test_replay_vectors.exe` (per-vector lines, summary,
  exit 0). Captured 2026-10-19 ~20:24 (+05:30).
- `host_test_log.txt` — `python tools/ceepew_pipeline.py --mode host`
  (starts 20:23:36; `[PASS] test_replay_vectors (0.64s)`, `Passed: 15 / 15`).
- `ctest_full_log.txt` — `ctest --test-dir tests/host/build_host`
  (`20/20`, `Total Test time (real) = 17.93 sec`).

## Reproduction

```powershell
cmake -B tests/host/build_host -S tests/host -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -G "MinGW Makefiles"
cmake --build tests/host/build_host --target test_replay_vectors
tests/host/build_host/test_replay_vectors.exe
python tools/ceepew_pipeline.py --mode host
```

## Provenance and honesty notes

- Captured 2026-10-19; copied verbatim into the repo as this directory.
  Contents were not modified after capture; the hashes above pin the bytes.
- The campaign was **added after** the paper text and README claim existed.
  The claim is now traceable to this executed artefact, and the registry
  entry `EXP-REPLAY-REJECT` was updated to cite
  `tests/host/test_replay_vectors.c` + this log. If `transport_replay.c` or
  the campaign test ever changes, this log must not be cited in its place
  without a re-run.
- Suite-composition caveat: `data/derived/host_ci_20261018/` reports
  pipeline 13/13 and ctest 19 — those are historical, narrower compositions
  and must not be conflated with the counts here (15/15 and 20/20 after the
  campaign was registered).
- The "95% CI failure bound ≈ 3.0%" figure used near this claim is a derived
  statistic of n=100 with 0 failures (rule of three, 3/100) — it is not an
  additional measurement.
