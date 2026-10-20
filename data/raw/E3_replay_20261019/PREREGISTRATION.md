# E3 — Replay Rejection: pre-registration

**Written before any result was observed.** This file fixes the measurement protocol,
the success criterion, and the exclusion rule in advance, so that they cannot be
adjusted after the fact to suit the outcome.

| Field | Value |
| :--- | :--- |
| Run ID | `E3_replay_20261019` |
| Date | 2026-10-19 |
| Experiment | E3 — stale-frame replay rejection |
| Boards | COM3 (DEVICE_A), COM4 (DEVICE_B) |
| Duration | 480 s per board, 115200 baud |
| Build dir | `build_e3` |
| Firmware SHA-256 | *recorded at seal time (build was in flight when this file was written)* |

## 1. Firmware configuration, and one deliberate deviation

`build_e3` is compiled with:

- `CONFIG_CEEPEW_CAPTURE=y`
- `CONFIG_CEEPEW_CAPTURE_MODE_CEEPEW=y`
- `CONFIG_CEEPEW_BENCH_58B=y`
- `CONFIG_CEEPEW_DEVELOPMENT_MODE` **absent** (production code paths)

**Deviation from the E5 reference build (`build_cap_timing`): `CONFIG_CEEPEW_BENCH_TIMING` is OFF.**

Reason: with `BENCH_TIMING` enabled, the link step failed —
`ceepew.elf section '.dram0.bss' will not fit in region 'dram0_0_seg'`, overflowed by
**40 bytes**. `BENCH_TIMING` is E5's latency instrumentation; it consumes `.bss` and
contributes nothing to a replay-rejection measurement. It was disabled to fit, not to
change behaviour.

This is recorded here because it is a real difference between the two builds and it must
be visible in any write-up. It does **not** alter frame construction, the crypto path,
the ESL header, or the wire format — the replay seam under test (`stage_tx_esl_wrap`) is
untouched by the flag.

`build_e3` also carries the BLE `ble_ctx_lock` fixes (crash-path fix, advertising-watchdog
lock-hold fix, 26 hardened call sites) which `build_cap_timing` predates. Relevant because
1 of 4 E1-A capture cycles panicked in that code path; a pairing failure in this run must
be attributed correctly rather than assumed to be a protocol/RF failure.

## 2. What is injected

Hook: `session_send_bench_replay_next()` (declared `main/session_fsm.h`, implemented
`main/session_send.c`), called from the BENCH58 driver loop in `main/task_session.c`
whenever `(s_bench_sent % 10U) == 4U`.

At that point the stored bytes of an earlier frame — **including its original ESL header,
and therefore its original replay-window `msg_id`** — are re-submitted through
`stage_tx_esl_wrap`, with a fresh ARQ sequence number assigned downstream.

The seam matters: the ESL header is attached *last*. Injecting before it would carry a
fresh `msg_id`, the peer would accept it, and the experiment would appear to pass while
proving nothing. The injection point is the one that preserves the stale `msg_id`.

Expected injection count for an 80-frame campaign: seq 4, 14, 24, …, 74 → **8 per board**.

## 3. Success criterion

A stale-`msg_id` frame is **rejected** if the peer logs a rejection attributable to it.
The reject reason/layer is reported as observed — the claim is not weakened or
strengthened to suit the err code:

- `err=17` (`CEEPEW_ERR_REPLAY`) — rejected at the replay-commit stage. This is the
  strongest form of the claim.
- `err=3` — rejected at the ESL transport header/MAC/replay check, i.e. the frame did
  not survive the ESL layer. A rejection, but a *different* layer; must be labelled as
  such and never silently merged into the err=17 count.

Both are rejections. They are not equivalent evidence and will not be presented as if
they were.

## 4. Exclusion rule (pre-declared)

**Exclusions: none.** No injection, rejection, or interval will be trimmed, dropped, or
excluded on the basis of its value.

Consequences of that rule, stated in advance:

1. **Link loss is not rejection.** If an injected frame is never observed at the peer
   (peer never received it), it is reported as **unmatched**, exactly like E2a. It is
   never counted as a rejection, and it is never counted as an acceptance.
2. **Unmatched rejections are not E3 evidence.** A rejection that cannot be matched
   one-to-one to an injection belongs to some other event (a forged frame, or a natural
   discard). It is reported in its own bucket and is not credited to E3.
3. **App-level per-sequence accounting is invalid here** and will not be used. ARQ
   retransmission means a corrupt frame is dropped and a *clean retry of the same
   sequence number* is subsequently committed, so a naive per-seq table reports
   "accepted" for frames that were in fact rejected. Only one-to-one, time-correlated
   matching is admissible.
4. **No outlier trimming.** The sample is ~8 injections per board. Every event is
   reported; N is stated for every figure.

## 5. Analysis tool

`tools/e3_replay_reject.py` — matches injections to rejections one-to-one and classifies
the rejection layer. It emits a three-way verdict and treats "no injections found" as
"nothing to verify" rather than as a pass. `--self-test` covers err=17 matching, err=3
ESL-layer matching, out-of-window rejection, and the one-to-one negative case.

## 6. What is explicitly NOT claimed

- Mode A/B captures bypass application crypto; no forged or replay count from those
  modes is a security result. E3 runs in **Mode C** only.
- This tests replay of a stale `msg_id` at the application/ESL layer. It is not a test of
  ESP-NOW link-layer duplicate suppression, and `[ARQ_RX] REPLAY reject` lines are
  recorded separately as link-layer duplicate suppression, never as a crypto defence.

## 7. Amendment (made before any result was observed): toolchain change

**ESP-IDF v6.0.2 was deleted from this machine mid-session.** Its tree at
`C:\esp\v6.0.2\esp-idf` lost `components/`, `cmake/`, `docs/` and then the whole
directory, and it was not a git checkout, so there was no restore path. It has been
replaced by **ESP-IDF v6.0.3** at `C:\esp\v6.0.3\esp-idf`, and `tools/setup_esp_idf.ps1`
was repointed to it.

**Consequence, stated plainly:** E1, E2a, E2b and E5 — and the sealed
`build_cap_timing` image (`b335daf4…`) — were all built with **v6.0.2**. E3 is therefore
built on a **different ESP-IDF version from every other experiment in this paper.**

What is *not* different: the C compiler. Both toolchains use
`xtensa-esp-elf 15.2.0_20251204`, which is the version that produced the existing images.
The delta is the ESP-IDF framework sources, not the code generator.

Why this is recorded and not quietly absorbed:

- A reader reproducing E3 from the paper's stated prerequisites (v6.0.2) should know the
  E3 binary came from v6.0.3.
- It is a genuine deviation from the "single toolchain across all experiments" property.
  It must be disclosed in the paper's setup section, not omitted.
- It is *not* a change made to improve a number. The decision to keep E3 on v6.0.3 is
  forced by the loss of v6.0.2, and the alternative (re-cloning v6.0.2) would cost a
  multi-gigabyte download. If the toolchain uniformity matters more than the time, E3 can
  be rebuilt on a re-cloned v6.0.2 — say so and this run will be redone rather than
  reported.

Because `build_e3` was configured against the now-deleted v6.0.2 path, its CMake cache is
dangling; E3 is built into a fresh directory (`build_e3_idf603`) rather than mutating that
stale cache. `build_cap_timing` — the sealed E5 artefact — is not touched. Its recorded
binary hash remains `b335daf4c879b9c5ddbe202a3636e8324e049d5e389d95eae80360441aa76bb0`.

## 8. Amendment 2 (made before any result was observed): toolchain reverts to v6.0.2

**This section corrects a factual error in Section 7, and supersedes it.**

Section 7 stated that ESP-IDF v6.0.2 "was deleted from this machine mid-session", that its
tree "was not a git checkout", and that there was "no restore path". **All three statements
were wrong.** Re-examined on 2026-10-19, before any E3 data existed:

- `C:\esp\v6.0.2\esp-idf` is present and *is* a git checkout. `git describe --tags` reports
  **`v6.0.2`**, and `components/esp_common/include/esp_idf_version.h` defines
  `ESP_IDF_VERSION_MAJOR 6`, `ESP_IDF_VERSION_MINOR 0`, `ESP_IDF_VERSION_PATCH 2`.
- What had actually occurred was not deletion but **submodule gutting**. Three submodules
  that supply the prebuilt Wi-Fi/PHY/COEX archives had been de-initialized —
  `components/esp_wifi/lib`, `components/esp_phy/lib`, `components/esp_coex/lib` — so
  `libcore.a`, `libnet80211.a`, `libpp.a`, `libphy.a` and `libcoexist.a` were absent. The
  tree presented as destroyed; it was one submodule population away from usable, and the
  `.git` metadata holding the pinned commits was intact.
- Those three submodules were restored by reading each pinned gitlink from the superproject
  (`git ls-tree HEAD <path>`) and fetching exactly that commit, rather than by re-cloning
  ESP-IDF. Restored archives, sizes verified on disk as valid `!<arch>` objects:
  `libcore.a` 4,876 B; `libnet80211.a` 1,574,536 B; `libpp.a` 628,380 B;
  `libphy.a` 864,848 B; `libcoexist.a` 125,562 B.

**Consequence: the deviation recorded in Section 7 is withdrawn. E3 is built on ESP-IDF
v6.0.2 — the same version as E1, E2a, E2b and E5, and the same as the sealed
`build_cap_timing` image.** There is no longer a toolchain non-uniformity to disclose.

**Second correction.** Section 7 described `build_e3`'s CMake cache as "dangling" and moved
the build to `build_e3_idf603`. With the v6.0.2 libraries restored the cache is not
dangling, and E3 is built in **`build_e3`**, as the table at the top of this file originally
stated. `build_e3_idf603` is abandoned; its image never linked.

**Why the v6.0.3 detour happened**, recorded so the record is complete: on v6.0.3 this exact
capture configuration cannot be linked at all. The link failed with
`region 'dram0_0_seg' overflowed by 16 bytes`; Section 1 already records an earlier
`BENCH_TIMING`-enabled attempt overflowing `.dram0.bss` by 40 bytes. The v6.0.2 toolchain
links this configuration without difficulty: `build_e3` linked successfully on v6.0.2 at
12:18 today *with the replay hook compiled in* (that image contains the `BENCH58_REPLAY`
markers). It was superseded only because its `config/sdkconfig.h` was regenerated at 12:48,
leaving the image stale relative to its own configuration — which is why it is rebuilt here
rather than reused.

**No flag was weakened to make this run link.** `build_e3/config/sdkconfig.h` (sha256
`eaae8eaf…`) is **byte-identical** to `build_cap_ceepew/config/sdkconfig.h`, the Mode C
capture configuration used for E1-C, E2a and E2b. E3's configuration is therefore identical
to those runs, and differs from the E5 reference build only in `BENCH_TIMING`, as Section 1
already states.

Recorded values for this run:

| Field | Value |
| :--- | :--- |
| ESP-IDF | **v6.0.2** (`C:\esp\v6.0.2\esp-idf`, tag `v6.0.2`) |
| Compiler | `xtensa-esp-elf 15.2.0_20251204` (unchanged) |
| Build dir | `build_e3` |
| Input config | `build_e3/sdkconfig` — sha256 `98ccde1e…` |
| Generated config | `build_e3/config/sdkconfig.h` — sha256 `eaae8eaf…` |
| Firmware SHA-256 | *recorded at seal time* |

No result had been observed when this amendment was written. Its purpose is to remove an
inaccurate disclosure from the record, not to alter what is measured.

