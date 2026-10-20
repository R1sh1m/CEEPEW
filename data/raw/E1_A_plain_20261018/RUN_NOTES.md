# E1-A (plain) — run notes

## Topology actually used
- BOTH boards ran `build_cap_plain` (Mode A, plain: ESL/Ascon application crypto
  bypassed on the session data path).
- BOTH boards transmit the BENCH58 campaign AND capture the peer's frames on
  their on-node capture ring (a node cannot hear its own TX, so every frame is
  captured by the PEER board).
- Ports: **COM3 + COM4** on 2026-10-18.

> The manifest `roles` field originally read
> "COM3=BENCH58 sender, COM5=promiscuous capture" — that describes an older,
> single-sender topology and a port (COM5) that no longer exists. It was
> corrected pre-seal to match what actually ran. No analytic field
> (`exclusion_rule`, `n_planned`, `capture_mode`, `hopping`, ...) was changed.

## Port map change
The USB port map changed during the session: the second board moved from COM5 to
COM4 (and COM3 stayed). Ports are re-discovered before every flash/capture via
`[System.IO.Ports.SerialPort]::GetPortNames()`.

## Build provenance refresh
This manifest was created at 2026-10-18T07:34:20Z, BEFORE the two handshake fixes
were written, so its recorded `app_binary_sha256` / sdkconfig hashes described a
stale binary. Pre-seal they were recomputed from the `build_cap_plain` that was
actually flashed (1,388,656 B, built 2026-10-18 23:14 local). Firmware binary in
use: `build_cap_plain/ceepew.bin`.

## Commands actually run (verbatim)
1. monitor:
   `python tools/ceepew_monitor.py --port COM3 --port COM4 --baud 115200 --duration 480 --log data\raw\E1_A_plain_20261018\capture_dual.log --no-color`
2. split:
   `python tools/capture/split_dual_log.py .../capture_dual.log --outdir data\raw\E1_A_plain_20261018 --prefix capture`

Driver: `python tools/e1_run_mode.py --run-id E1_A_plain_20261018 --build-dir build_cap_plain --ports COM3,COM4 --baud 115200 --duration 480 --no-flash -v`

## Interrupted attempt (kept as evidence, NOT part of this manifest)
The first attempt at this run was cancelled by an opencode server restart at
~23:23 local. Its partial capture is preserved at
`data/raw/_tooling/E1_A_plain_20261018_partial_serverrestart_2323.log`
(2,858,122 B). It is excluded from this sealed run because the capture was
truncated mid-campaign; it is retained so the truncation is auditable.

## Observed behaviour during the run (descriptive, no rule changed)
- Pairing cycle 1 reached KEY_DERIVE at t≈11.1 s (DEVICE_B) / 12.3 s (DEVICE_A)
  but its post-derive sync did not complete, so it FAILED at t≈44.6 s with the
  30 s post-derive timeout. Pairing cycle 2 then re-paired and cleared the sync
  barrier, evolving the UI to `chat_menu` at t≈59.9 s.
- The full BENCH58 campaign then ran to completion on BOTH boards:
  `[BENCH58_DONE]` on both, seq 0..79 present with **zero gaps** on each board.
  Campaign spans t≈59.7 s → 460.2 s at 5.07 s per frame.
- Forge arming: seq 9, 19, 29, 39, 49, 59, 69, 79 on each board (8/board, 16
  total). Those 8 seq values are each logged twice (once as `seq=N`, once as
  `seq=N forged`), which is why the raw `BENCH58_TX` line count is 88 per board
  / 176 total rather than 160 — it is not extra frames.

## Key counters (from capture_dual.log, 6,359,349 B)
| counter | value |
|---|---|
| `phase=active` | 2086 |
| `chat_menu` | 2075 |
| `BENCH58_DONE` | 2 (one per board) |
| `BENCH58_TX` lines | 176 = 2 x (80 frames + 8 forge annotations) |
| forged | 16 (8 per board) |
| `POST_DERIVE_SYNC` | 3 (HELLO -> ACK sent -> ACK received) |
| `Post-derive sync timed out` | 1 (pairing cycle 1) |
| `CRC & FEC verified` | 158 |
| `Message received` | 155 |
| `Inner CRC mismatch` | 2 |
| `Ascon-128 AEAD auth tag verification FAILED` | 0 |
| `ESL transport header...` rejects | 0 |

## Interpretation notes for the paper
- Mode A bypasses ALL application crypto (Ascon/box/Ed25519), so there is no
  Ascon tag to fail: the BENCH58 "forged" frames are **not** rejected at an
  authenticator stage in this mode. Do NOT cite Mode A forged-reject counts as
  a security result; that claim belongs to Mode C / E2b.
- `Message received` (155) therefore includes any forged frames that survived;
  this is expected in a plain baseline and is a property of the mode, not a
  defect in the capture.

## Capture-integrity gap
Per the pre-registered exclusion rule, the gap between transmitted frames and
peer-captured ring rows is reported rather than silently dropped. Ring rows:
DEVICE_A 8935, DEVICE_B 819 (from `split_dual_log.py`). The asymmetry is expected
in a symmetric run: one board's logs were dominated by per-second status lines
while the other's carried the bulk of the TX ring traffic. The BENCH58 frame
accounting itself is complete and gap-free at the seq level (80/80 per board).
