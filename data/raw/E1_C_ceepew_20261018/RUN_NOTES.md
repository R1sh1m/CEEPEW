# E1-C (CEE-PEW full stack) — run notes

## Topology actually used
- BOTH boards ran `build_cap_ceepew` — **Mode C**, the full CEE-PEW stack:
  `CONFIG_CEEPEW_CAPTURE=y`, `CONFIG_CEEPEW_CAPTURE_MODE_CEEPEW=y`,
  `CONFIG_CEEPEW_BENCH_58B=y` (verified from the compiled `sdkconfig.h` flags in
  `manifest.json`). `CEEPEW_CAPTURE_ESL_BYPASS` is **0** in this mode, so the
  ESL/Ascon + crypto_box + Ed25519 path runs in full — this is the
  security-relevant mode (and the one the bypass fixes were written not to touch).
- BOTH boards transmit the BENCH58 campaign AND capture the peer's frames.
- Ports: **COM3 + COM4**.
- `app_binary_sha256` = `d2390308…`. The flash logs (`flash_COM3.log`,
  `flash_COM4.log`, ~9.6 kB each) show the flash did **not** rebuild: unlike the
  E1-B flash, `build_cap_ceepew` was already a correct capture build, so the
  binary recorded in the manifest is the binary that ran.

## Observed behaviour — cleanest run of the set
- Pairing succeeded on the **FIRST cycle, no retry**:
  `IDLE -> DISCOVERY` t≈3.0 s, `-> COMMITMENT` t≈8.4 s, `-> GATT_IDENTITY`
  t≈8.7 s (A) / 9.6 s (B), `-> KEY_DERIVE` t≈11.5 s (B) / 13.4 s (A).
- Post-derive sync completed on the first cycle, with the full three-way
  exchange visible:
  - t=16.4 s `[POST_DERIVE_SYNC] Received sync byte 0xA5 (HELLO)`
  - t=16.8 s `[POST_DERIVE_SYNC] Sent ACK to initiator 1C:C3:AB:C2:52:AC`
  - t=17.8 s `[POST_DERIVE_SYNC] Received sync byte 0x5A (ACK)`
  `Post-derive sync timed out` = **0**; UI reaches `chat_menu`.
- BENCH58 campaign complete on BOTH boards: `[BENCH58_DONE]` x2,
  `sent_total: 160`, seq 0..79 with **zero gaps** each, 16 forged frames armed.

## Frame-size evidence (PRIMARY: intact short log lines)
From `tools/e1_frame_lengths.py`:

| source | n | length histogram (bytes) |
|---|---|---|
| `[BENCH58_TX]` | 160 | {58: 160} |
| `[ARQ_TX]` | 170 | {77: 2, 173: 8, 233: 94, 237: 66} |
| `RADIO-RX frame from ... len=` | 175 | {2:1, 5:1, 9:3, 78:2, 174:8, 234:94, 238:66} |
| `[SECURE_CHAT_TX] Initiating TX:` | 166 | {1: 7, 58: 159} |

ARQ TX sequence coverage: 0x00..0x55, 86 unique, **gaps = 0**.

**This is the headline mode contrast:** the Mode C application frame is
**233 B / 237 B** on the wire (`RADIO-RX` 234/238), versus **74 B / 78 B**
(`RADIO-RX` 75/79) for Modes A and B. Control/handshake frames are 173 B
(Mode C) vs 12 B (Modes A/B). Pc = 58 B of payload in every case.
Note `233` vs `237` is a 4 B split (94 vs 66 frames); `RADIO-RX` is consistently
ARQ_TX + 1.

## Ring-hex degradation (quantified — pcap NOT usable as primary evidence)
`tools/e1_frame_lengths.py` ring audit: **10,668 ring rows — 2,055 clean
(19.3 %), 8,613 corrupted (80.7 %)**, with the same length bias as E1-A/E1-B
(long frames preferentially destroyed). Same root cause: the on-node ring-dump
task and the ESP-IDF log task interleave on the shared 115200-baud UART. See
registry entry `CAP-RING-DEGRADE`. Mode C has the largest frames of any mode
(233/237 B + full ring slotting), which is exactly the regime worst affected.

## Forged-frame rejection (this run is the Mode C forgery evidence)
The BENCH58 forge path arms `session_send_bench_forge_next()` once; the next
transmit corrupts **one outer-ciphertext byte** (`main/session_send.c` ~170-177,
`stage_*`), exercising the over-the-air reject path. Peer response, measured by
time-correlated matching:

| metric | value |
|---|---|
| forged frames transmitted | 16 (8 per board: seq 9,19,29,39,49,59,69,79) |
| crypto-stage rejections total | 34 (16 `box`, 17 `pipeline`, 1 `ascon`) |
| **forged frames matched to a peer rejection** | **16 / 16** |
| match latency (on-device clocks) | median **+0.40 s**, max +0.50 s |
| rejections not explained by a forged frame | 2 (ordinary RF corruption / retries) |

Reject kind: `Discard: crypto_box decrypt FAILED (err=5, len=83)` and
`Discard: RX decrypt pipeline failed (err=5, len=209)`.
`err=5` = **`CEEPEW_ERR_CRYPTO`** (`hal_ui_types.h`). No forged frame reached the
application layer.

### METHODOLOGICAL WARNING — do not repeat this mistake
An app-level per-sequence accept/reject table is **invalid** here. Because the
forge is one-shot and ARQ retransmits, the corrupted frame is dropped and a
**clean retry of the same application sequence number is then accepted** — so
every forged `seq` eventually appears as `[BENCHRX] commit seq=N err=0`. Building
an accept/reject table from `commit seq=` therefore shows "8/8 forged accepted",
which is the OPPOSITE of the truth. The only valid metric is one-to-one
time-correlated rejection on the peer device (as tabulated above).

## Key counters (from capture_dual.log, 9,089,350 B)
| counter | value |
|---|---|
| `POST_DERIVE_SYNC` | 3 |
| `Post-derive sync timed out` | **0** |
| `BENCH58_DONE` | 2 (sent_total 160) |
| `BENCH58_TX` lines | 176 = 2 x (80 frames + 8 forge annotations) |
| forged | 16 |
| `phase=active` | 1511 |
| `chat_menu` | 1505 |
| `CRC & FEC verified` | 162 |
| `Message received` | 141 |
| `Inner CRC mismatch` | 0 |
| `Ascon-128 AEAD auth tag verification FAILED` | 1 |
| `ESL transport header...` rejects | 1 |
| `ARQ_TX` / `ARQ_RX` | 170 / 168 |
| `Discard` (all kinds) | 35 |

`Message received` (141) < 160 sent because the 16 rejected forgeries and a few
RF losses are not delivered; the survivors are the clean ARQ retries.

## Capture-integrity gap
Application-level accounting is complete: 160/160 BENCH58 frames with zero seq
gaps on both boards, ARQ TX covering 0x00..0x55 with zero gaps. The degraded
quantity is the *ring hex* (the pcap substrate), quantified above.
