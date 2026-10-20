# E2(b) — Forged-frame rejection (CEE-PEW full stack) — run notes

## Objective
Verify that CEE-PEW rejects **forged** frames over the air in Mode C (full stack),
under this run's own sealed manifest, as an independent replicate of the same
configuration captured in E1-C.

## Topology actually used
- BOTH boards ran `build_cap_ceepew` — Mode C, full CEE-PEW stack
  (`CONFIG_CEEPEW_CAPTURE=y`, `CONFIG_CEEPEW_CAPTURE_MODE_CEEPEW=y`,
  `CONFIG_CEEPEW_BENCH_58B=y`; `CEEPEW_CAPTURE_ESL_BYPASS=0`).
- `app_binary_sha256` = `d2390308…` — identical to E1-C's, and **no reflash was
  performed** (`--no-flash`): the boards still carried the E1-C image.
- Ports: **COM3 + COM4**. No `flash_*.log` files exist for this run by design.

## Forge mechanism (what is actually being tested)
`session_send_bench_forge_next()` (`main/session_send.c` ~49) arms a ONE-SHOT
flag; on the next transmit the code at ~170-177 corrupts exactly **one
outer-ciphertext byte** (`[BENCH58_FORGE] armed frame corrupted (1 box byte)`).
`main/task_session.c` (~2876) arms it every 10th BENCH58 frame and logs
`[BENCH58_TX] seq=N forged (tag byte will be corrupted)`. The peer must reject
that frame at its authenticator.

## RESULT — forged-frame rejection: 16 / 16
Measured with `tools/e2_forge_reject.py` (device boot-time clock, ±2.0 s window):

| metric | value |
|---|---|
| forged frames transmitted | 16 (8 per board; seq 9,19,29,39,49,59,69,79) |
| forge-armed confirmations | 16 (one per forged frame) |
| **forged matched to peer rejection** | **16 / 16** |
| match latency | median **400 ms**, range 100–1500 ms |
| rejection kind for all 16 | `box` — `Discard: crypto_box decrypt FAILED (err=5, …)` |
| total crypto rejections | 36 = 16 box + 17 pipeline + 1 ascon + 2 replay |
| rejections unexplained by a forged frame | 6 (2 replay, 1 ascon, 3 pipeline) — ordinary RF loss / other traffic |

`err=5` = **`CEEPEW_ERR_CRYPTO`** (`hal_ui_types.h`). **No forged frame reached
the application layer.**

This run and E1-C are the same configuration and agree exactly: both give
**16/16** forged frames rejected, at the same `box` stage, at the same ~400 ms
latency. Two independent captures, same result.

## METHODOLOGICAL WARNING — carried forward from E1-C
Do **not** build an accept/reject table from application sequence numbers.
Because the forge is one-shot and ARQ retransmits, the corrupted frame is
dropped and a **clean retry of the same application sequence number is then
accepted** — so every forged `seq` eventually logs
`[BENCHRX] commit seq=N err=0`. A naive per-seq table reports "8/8 forged
accepted", which is the exact opposite of the truth. Only one-to-one
time-correlated rejection on the peer device is valid.

## Observed behaviour
- Pairing succeeded on the **FIRST cycle — no retry**: `Post-derive sync timed
  out` = **0**; `POST_DERIVE_SYNC` = 3 with the full exchange visible:
  t=15.1 s `Received sync byte 0xA5 (HELLO)`, `Sent ACK to initiator`,
  t=16.4 s `Received sync byte 0x5A (ACK)`. UI reaches `chat_menu`.
- BENCH58 campaign complete on BOTH boards: `[BENCH58_DONE]` x2,
  `sent_total: 160`, seq 0..79 with **zero gaps** each, 16 forged armed.

## Key counters (from capture_dual.log, 9,117,997 B)
| counter | value |
|---|---|
| `POST_DERIVE_SYNC` | 3 |
| `Post-derive sync timed out` | **0** |
| `BENCH58_DONE` | 2 (sent_total 160) |
| `BENCH58_TX` lines | 176 = 2 x (80 frames + 8 forge annotations) |
| forged | 16 |
| `phase=active` | 1494 |
| `chat_menu` | 1488 |
| `CRC & FEC verified` | 162 |
| `Message received` | 141 |
| `Inner CRC mismatch` | 0 |
| `Ascon-128 AEAD auth tag verification FAILED` | 1 |
| `ESL transport header...` rejects | 2 |
| `ARQ_TX` / `ARQ_RX` | 167 / 167 |
| `Discard` (all kinds) | 36 |

`Message received` (141) < 160 sent because the 16 rejected forgeries (and a few
RF losses) are not delivered; delivery happens via the clean ARQ retry.

Split: DEVICE_A 4,506,621 B / DEVICE_B 4,513,745 B.

## Interrupted first attempt (kept as evidence, NOT part of this manifest)
The first attempt at this run was cancelled by an opencode server restart at
~02:02 (server restarts have now interrupted three runs). Its partial capture is
preserved at
`data/raw/_tooling/E2b_forged_rejection_20261018_partial_serverrestart_0202.log`
(4,914,666 B) and was removed from this run directory so the sealed capture is a
single clean 480 s session. It is retained for auditability.

## Capture-integrity gap
Application-level accounting is complete: 160/160 BENCH58 frames with zero seq
gaps on both boards. As with E1-A/B/C, the on-node **ring hex** is degraded by
UART interleaving (registry `CAP-RING-DEGRADE`); this run's evidence does not
depend on ring hex — the forgery result comes from intact short metadata lines.
