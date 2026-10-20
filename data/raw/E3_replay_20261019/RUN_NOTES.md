# E3 — Stale-frame replay rejection — run notes

## Purpose

E3 tests **on-device stale-replay rejection**. Replay injection is performed via the
BENCH58 replay hook (`session_send_bench_replay_next()`), which re-submits a previously
captured ESL-wrapped blob (including its original ESL header and replay-window `msg_id`)
with a fresh ARQ sequence number. The injection point (`stage_tx_esl_wrap`) preserves the
stale `msg_id` so the peer's replay defence can be exercised.

## Build and conditions

- **Build dir:** `build_e3`
- **ESP-IDF:** **v6.0.2** (`C:\esp\v6.0.2\esp-idf`, tag `v6.0.2`)
- **Binary:** `build_e3/ceepew.bin`, **1,403,456 B**
- **SHA-256:** `6e392147ae80aed5068ebd55e27236c42a43c4ab20dc0885033266490e6ee83e`
- **Compiled flags verified** (from `build_e3/config/sdkconfig.h`):
  `CONFIG_CEEPEW_BENCH_58B=1`, `CONFIG_CEEPEW_CAPTURE=1`,
  `CONFIG_CEEPEW_CAPTURE_MODE_CEEPEW=1`.
- **Gate checks:**
  - Gate 1 (compiled flags): **PASS**
  - Gate 2 (binary newer than `config/sdkconfig.h`): **PASS**
  - Gate 3 (boards COM3+COM4, COM6 excluded): **PASS**
- **Ports:** COM3 (DEVICE_A), COM4 (DEVICE_B)
- **Channel:** 1, **hopping OFF**
- **Distance:** 20 cm, same USB hub
- **Duration:** 480 s, **exit code 0**

## Key counters (from `capture_dual.log`, 8,519,751 B)

| counter | value |
|---|---|
| `[BENCH58_REPLAY] armed` | **16** |
| `[BENCH58_REPLAY] replayed` | **16** |
| `replay commit rejected` (err=17) | **0** |
| `ESL transport header/MAC/replay check failed` (err=3) | **18** |
| `[ARQ_RX] REPLAY reject` (link-layer) | **1** |
| `POST_DERIVE_SYNC` | **3** |
| `PAIRING_FAILED` | **2** |
| `BENCH58_TX` | **176** |

Counts verified by `Select-String` against `capture_dual.log` before writing.

## RESULT (headline verdict)

**16/16 replayed frames were time-correlation-matched to peer rejections, but every one
of the 16 matched a rejection at the ESL header/MAC layer (`err=3`, 18 occurrences); ZERO
reached the crypto replay-window commit path (`replay commit rejected`, err=17 → 0
occurrences).**

The honest headline is: **"16/16 stale replays rejected, by the ESL header/MAC layer
(err=3)."**

Rejection kinds are reported **separately**:
- **err=3** (`CEEPEW_ERR_PARAM`): ESL transport header/MAC/replay check — **18 occurrences**
- **err=17** (`CEEPEW_ERR_REPLAY`): replay-window commit — **0 occurrences**

These are **never merged**, and a bare `err=17` count is **never cited as replay evidence**
for this run.

## Analysis tool

`tools/e3_replay_reject.py` (self-test passes; covers err=17 matching, err=3 ESL-layer
matching, out-of-window rejection, and the one-to-one negative case).

Outputs:
- `data/derived/E3_replay_20261019/replay_reject.json` (12,360 B)
- `data/derived/E3_replay_20261019/replay_reject.md` (3,468 B)

## Deviations and caveats

1. **PAIRING_FAILED = 2** during the run. Two pairing attempts failed before the session
   entered ACTIVE state.
2. **Injection count = 16**, vs **8 originally anticipated** in `PREREGISTRATION.md` §2
   (seq 4, 14, 24, …, 74 → 8 per board). The actual run injected 16 total (8 per board,
   but the pre-registration expected ~8 total). This is recorded as a deviation from the
   pre-registered expectation.
3. **Ring-hex degradation caveat:** The on-node 115200-baud ring dump interleaves with the
   IDF log task (~80% row corruption), so **no usable pcap is claimed from this run**.
   Evidence is from the serial log markers only.
4. **PREREGISTRATION.md §8 amendment:** Toolchain returned to IDF v6.0.2. An earlier draft
   claimed v6.0.2 was deleted and unrecoverable; that assertion was wrong. The v6.0.2
   tree was restored by repopulating three gutted submodules (`esp_wifi/lib`,
   `esp_phy/lib`, `esp_coex/lib`), and E3 is built on the **same ESP-IDF version as E1,
   E2a, E2b, and E5**.

## Reproduce

```
python tools/ceepew_pipeline.py --mode pairing  # with build_e3
python tools/e3_replay_reject.py --run-id E3_replay_20261019
```
