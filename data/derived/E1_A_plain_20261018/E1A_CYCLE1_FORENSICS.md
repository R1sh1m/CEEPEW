# E1-A first-cycle post-derive-sync timeout — forensics (RESOLVED)

**Verdict: the E1-A cycle-1 failure was caused by a firmware PANIC on the
initiator board (DEVICE_A), not by an RF, timing, or protocol issue.**

This anomaly was previously documented as unexplained. It is now resolved with a
concrete causal chain and named defect.

## Evidence

### 1. The panic (DEVICE_A, host ~14.4-14.5 s)
```
E (14233) transport_ble: ble_ctx_lock DEADLOCK: held by pair_supp for >2000 ms — aborting
E (14233) transport_ble: enter_phase(IDLE): ble_ctx_lock timed out
E (14248) transport_ble: ble_ctx_lock DEADLOCK: held by pair_supp for >2000 ms — aborting
I (14248) transport_ble: Commitment beacon now active in scan response
assert failed: xTaskPriorityDisinherit tasks.c:5156 (pxTCB == pxCurrentTCBs[ xPortGetCoreID() ])
Backtrace: 0x4009122d:0x3ffed1e0 0x400911f9:0x3ffed200 0x400907e5:0x3ffed220 0x401bfa35:...
ELF file SHA256: 1c62e3d89
Rebooting...
rst:0xc (SW_CPU_RESET)
```
The board then ran a full second-stage boot (partition table, esp_image segments,
`app_init: Application information:`), i.e. **a genuine reboot mid-run**.

The assertion is the classic FreeRTOS mutex priority-inheritance violation
(`xTaskPriorityDisinherit`: disinheriting a mutex that the current task does not
hold). It follows two `ble_ctx_lock` deadlock-detector trips where the lock was
held by the `pair_supp` task for >2000 ms. The failure is in the BLE context
mutex handling in `transport_ble`, reached during the IDLE-phase entry that
immediately precedes the post-derive-sync barrier.

### 2. It happened exactly at barrier entry
| event | DEVICE_A boot ms | host s |
|---|---|---|
| `enter_phase(IDLE)` / deadlock detector trips | 14233-14248 | 14.4 |
| FreeRTOS assert, backtrace, reboot | ~14.5 s (host) | 14.5 |

Both boards had completed `GATT_IDENTITY` at boot ~8.5-9.4 s and were in
agreement to within ~150 ms, so FSM progress and crypto were healthy right up to
the crash.

### 3. The consequence matches the observed 45 s HELLO delay
On DEVICE_B (which did NOT crash):
```
I (44170) SESSION: Post-derive sync timed out after 30000 ms — failing pairing
I (59558) SESSION: [POST_DERIVE_SYNC] Received sync byte 0xA5 (HELLO)
I (59606) SESSION: [POST_DERIVE_SYNC] Sent ACK to initiator 1C:C3:AB:C2:52:AC
```
and on DEVICE_A: `[POST_DERIVE_SYNC] Received sync byte 0x5A (ACK)` at host 60.1 s.

Note the HELLO was **late, not lost**: it arrived **15.4 s after DEVICE_B's own
30 s timeout had already expired**. Cross-board timing is consistent only when
the two independent boot clocks are treated as unsynchronised.

The chain is self-consistent: A crashed at host 14.5 s → A rebooted and dropped
to IDLE, needing BLE rediscovery → B was meanwhile stuck in its barrier (its BLE
side was no longer advertising in a way A could latch) until B's own timeout
fired at host 44.4 s → both re-entered discovery → re-paired → HELLO delivered at
host 59.6 s → ACK returned at host 60.1 s → **cycle 2 succeeded**.

## Why this is a one-off (and intermittent, not deterministic)
Boot/panic markers per captured run, counted across both devices:

| run | `app_init` per board | backtrace | first-cycle outcome |
|---|---|---|---|
| `E1_A_plain_20261018` | **DEVICE_A 2x**, DEVICE_B 1x | **yes (DEVICE_A)** | **cycle 1 FAILED (timeout)** |
| `E1_B_ccmp_20261018` | 1x / 1x | no | cleared |
| `E1_C_ceepew_20261018` | 1x / 1x | no | cleared |
| `E2b_forged_rejection_20261018` | 1x / 1x | no | cleared |

So the crash occurred in 1 of these 4 captures. It is a **race**, which is
consistent with a lock held >2000 ms by `pair_supp` only under some interleaving.

## Implications — the important part
1. **The pairing fix is NOT implicated.** The barrier logic did what it was
   designed to do: it timed out, failed the cycle, and recovered on retry. The
   only defective component is the BLE context lock path.
2. **This is a latent, intermittent firmware defect worth fixing.** One pairing
   cycle in four captured runs was destroyed by a hard reboot. In the field this
   presents as an inexplicable ~45 s extra pairing delay followed by success —
   easy to misattribute to RF.
3. **Reporting rule for the paper:** the E1-A cycle-1 timeout must be reported as
   *crash-induced*, with this evidence, and must NOT be counted as a protocol or
   RF pairing failure. E1-A's frame-length and campaign data remain valid: the
   run completed its full BENCH58 campaign after the successful cycle 2.
4. The crash site is in `components/transport/transport_ble.c` around the
   `ble_ctx_lock` / `pair_supp` interaction and the IDLE-phase entry path. The
   `transport_ble.c` file is among the modified-but-uncommitted files in the
   working tree, so **the author must confirm whether the current tree still
   contains this defect** — this forensic note does not establish that the bug is
   fixed, only what went wrong on the E1-A binary (`9495ee6c…`, ELF SHA256
   prefix `1c62e3d89`).

## Method
Derived entirely from the sealed `capture_dual.log` by filtering on host
timestamps and device labels; no files in the sealed run directory were
modified. Device boot clocks were used for the ordering claims, and are treated
as **unsynchronised across boards** (they differ by several seconds; the
firmware's own `Epoch sync: offset=… peer_epoch=…` mechanism exists precisely
because of this).
