# Host CI verification logs — 2026-10-18

Purpose: durable, in-repo home for the host-side test logs backing registry entry
`TEST-HOST-CI`. They were previously referenced only by a `%TEMP%` path, which made the
claim unreproducible from a clean checkout.

## Provenance (disclosed, not paper evidence in itself)

- Captured 2026-10-18 23:50–23:51 during the session that applied the security-hardening
  change set. Copied here verbatim on 2026-10-19; **contents unmodified**.
- These are *captured logs*, not regenerated artifacts. To reproduce the runs:
  - `host_test_log.txt` — `python tools/ceepew_pipeline.py --mode host`
  - `ctest_full_log.txt` — `ctest` in `tests/host/build_host`
- Copied from a session-scoped temp directory, so no path under the repo can be cited as the
  original writer location. The hashes below pin the exact bytes that are cited.

## Files

| File | Bytes | SHA-256 |
|------|-------|---------|
| `host_test_log.txt` | 3976 | `61dd6ae91203f46be0e2f3e672821f0b93732a384ff4d9a3acabfb8c0709695b` |
| `ctest_full_log.txt` | 4342 | `f3e4cee8450a526e8a9c371c4da11b098b34d24553c94a67db57ebaee9bbdd6f` |

## What each log establishes (do not conflate the two counts)

- `host_test_log.txt` — `ceepew_pipeline --mode host`: **Passed: 13 / 13**, ending
  `PIPELINE EXECUTION: SUCCESS [ALL STAGES PASSED]`. Covers the core host unit tests
  (security-utils, Ascon, Hamming FEC fault injection, SHA-256, HKDF, EdDSA, Huffman,
  session pairing, replay window, UI navigator, ...).
- `ctest_full_log.txt` — `ctest` suite: **100% tests passed out of 19**,
  `Total Test time (real) = 12.29 sec`. Its final tests are `fuzz_smoke_*`
  (ascon, sha256, hkdf, hamming, arq), i.e. this is the wider 19-test ctest set.

`TEST-HOST-CI` reports **19/19**; that figure is the `ctest` count from
`ctest_full_log.txt`. The 13/13 in `host_test_log.txt` is a *different, narrower* suite count
and must not be presented as the same measurement.
