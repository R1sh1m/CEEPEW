# E1-B (CCMP) — run notes

## Topology actually used
- BOTH boards ran `build_cap_ccmp` — **Mode B**: `CONFIG_CEEPEW_CAPTURE=y`,
  `CONFIG_CEEPEW_CAPTURE_MODE_CCMP=y`, `CONFIG_CEEPEW_BENCH_58B=y` (verified from
  the compiled `sdkconfig.h` flags recorded in `manifest.json`).
- Mode B derives the LMK (hardware CCMP active) but has
  `CEEPEW_CAPTURE_ESL_BYPASS=1`, i.e. the ESL/Ascon application crypto is
  bypassed on the session data path.
- BOTH boards transmit the BENCH58 campaign AND capture the peer's frames on
  their on-node capture ring (a node cannot hear its own TX, so every frame is
  captured by the PEER board).
- Ports: **COM3 + COM4** on 2026-10-18/19.

## Flash provenance — read this before comparing against E1-A
Flashing used `idf.py -B build_cap_ccmp -DSDKCONFIG=build_cap_ccmp/sdkconfig -p <port> flash`
for each board; both reported `[ OK ] flashed ... (hash verified)`. Raw flash logs
are preserved in this directory (`flash_COM3.log`, `flash_COM4.log`).

The flash path performed a **full CMake reconfigure + rebuild (1393 objects)**.
The pre-flash `build_cap_ccmp/ceepew.bin` (1,403,056 B) turned out to have been a
**development-mode** binary: its compiled flags carried
`CONFIG_CEEPEW_DEVELOPMENT_MODE` and were MISSING `CONFIG_CEEPEW_CAPTURE` and
`CONFIG_CEEPEW_CAPTURE_MODE_CCMP`. The reconfigure produced the correct capture
binary (1,389,168 B) that actually ran this experiment.

Consequences, stated plainly:
- The `app_binary_sha256`, `configured_sdkconfig_sha256`,
  `generated_sdkconfig_sha256` and `compiled_flags` fields in `manifest.json`
  were re-read **after** the flash, so they describe the binary that actually
  produced this capture. An earlier manifest revision (written pre-flash)
  recorded the development-mode hash and has been superseded before sealing.
- No source file changed between the 23:21 build and the 00:03 rebuild
  (verified by mtime scan of `main/` + `components/`); the difference is
  configuration, not code.

## Observed behaviour during the run
- Pairing succeeded on the **FIRST cycle — no retry**:
  `IDLE -> DISCOVERY` t≈3.0 s, `-> COMMITMENT` t≈8.2 s,
  `-> GATT_IDENTITY` t≈8.4 s, `-> KEY_DERIVE` t≈11.5 s (B) / 13.6 s (A).
  `Post-derive sync timed out` = **0**; `POST_DERIVE_SYNC` = 3
  (HELLO 0xA5 -> ACK sent -> ACK 0x5A received); UI reaches `chat_menu`.
  This is the cleanest pairing observed across all runs today (E1-A needed a
  second cycle; pre-fix builds never cleared the barrier at all).
- BENCH58 campaign ran to completion on BOTH boards: `[BENCH58_DONE]` x2,
  `sent_total: 160`, seq 0..79 with **zero gaps** on each board, 16 forged
  frames armed (8 per board).

## Frame-size evidence (PRIMARY source: intact short log lines)
Intact, gap-free, from `tools/e1_frame_lengths.py`:

| source | n | length histogram (bytes) |
|---|---|---|
| `[BENCH58_TX]` | 160 | {58: 160} |
| `[ARQ_TX]` | 173 | {12: 13, 74: 92, 77: 2, 78: 66} |
| `RADIO-RX frame from ... len=` | 180 | {2:1, 5:1, 9:3, 13:13, 75:94, 78:2, 79:66} |
| `[SECURE_CHAT_TX] Initiating TX:` | 170 | {1: 13, 58: 157} |

ARQ TX sequence coverage: 0x00..0x57, 88 unique, **gaps = 0**.

Per-device: DEVICE_A BENCH58_TX {58:80}, DEVICE_B BENCH58_TX {58:80}.

## Ring-hex degradation (quantified — DO NOT use the pcap as primary evidence)
The on-node ring dump prints `ts,rssi,ch,len,<HEX>` lines. Under this log volume
the ESP32's two UART writers (ring-dump task and the ESP-IDF log task) interleave
and/or bytes are dropped, cutting the hex. Measured by
`tools/e1_frame_lengths.py`:

- ring rows 9,596 — **clean 1,404 (14.6 %), corrupted 8,192 (85.4 %)**
- clean lengths:      {28:95, 32:136, 114:92, 116:127, 213:91, 242:59, 246:96, 300:280}
- corrupted lengths:  {26:1, 213:598, 235:466, 237:233, 239:501, 242:879, 246:938, 261:963, 300:2315}

The corruption is strongly **length-biased** — long frames are preferentially
destroyed (`300` dominates the corrupted set at 2,315 of 9,596 rows). A pcap
built from these rows therefore yields a frame-length histogram biased toward
short frames, and any airtime figure derived from it would be wrong.

**Key consequence for Mode B specifically:** Mode A and Mode B share the same
application-level wire format (`CEEPEW_CAPTURE_ESL_BYPASS=1` applies to BOTH
PLAIN and CCMP), and indeed the intact short-line histograms are near-identical
between E1-A and E1-B ({74:94,78:66} vs {74:92,78:66}). The only physical
difference between the two modes is at the **802.11/CCMP layer** (CCMP adds an
8 B header + 8 B MIC = +16 B on air). That difference is exactly what the
corrupted long-frame ring rows can no longer resolve. So E1-B can evidence
application-level frame sizes and campaign completeness, but it CANNOT by itself
evidence the CCMP on-air length delta from this capture. The 802.11-layer
claim needs either a clean re-capture or the analytic airtime budget
(`tools/wireshark/airtime_calc.py`, evidence class C).

## Key counters (from capture_dual.log, 9,741,175 B)
| counter | value |
|---|---|
| `POST_DERIVE_SYNC` | 3 |
| `Post-derive sync timed out` | **0** |
| `BENCH58_DONE` | 2 (sent_total 160) |
| `BENCH58_TX` lines | 176 = 2 x (80 frames + 8 forge annotations) |
| forged | 16 (8 per board) |
| `phase=active` | 1672 |
| `chat_menu` | 1666 |
| `CRC & FEC verified` | 167 |
| `Message received` | 155 |
| `Inner CRC mismatch` | 2 |
| `Ascon-128 AEAD auth tag verification FAILED` | 0 |
| `ESL transport header...` rejects | 0 |
| `ARQ_RX` | 174 |

Split: DEVICE_A 22,046 lines / 8,057 ring rows; DEVICE_B 22,256 lines / 8,198 ring rows.

## Interpretation notes for the paper
- Mode B bypasses the ESL/Ascon application crypto, so there is no Ascon tag to
  fail: the BENCH58 forged frames are NOT rejected at an authenticator stage in
  this mode. Do NOT cite a Mode B forged-reject count as a security result; that
  claim belongs to Mode C / E2b.
- The 16 forged frames were transmitted and counted; their rejection behaviour is
  a Mode C measurement.

## Capture-integrity gap
Per the pre-registered rule, the gap between transmitted and peer-captured frames
is reported rather than dropped. At the application level the accounting is
complete: 160/160 BENCH58 frames present with zero seq gaps on both boards, and
ARQ TX covers 0x00..0x57 with zero gaps. The degraded quantity is the *ring hex*
(the pcap substrate), not the campaign accounting — quantified above.
