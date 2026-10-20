# Capture-session runbook — OTA / promiscuous logging for CEE-PEW

Scope: how to run RF capture sessions that produce court-admissible pcaps for
Fig 3b/3c recompute and the security-vector checks. Complements (does not
replace) `tools/capture/ota_sniffer_plan.md`, which covers the 3rd-ESP32 sniffer
firmware plan. This file covers the **on-node promiscuous + serial ring-dump**
path converted with `tools/capture/serial_to_pcap.py`.

## 0. Golden rule: capture sessions are SEPARATE from timing sessions

Serial logging perturbs latency (UART blocking, log-level gating, ISR
deferral). Never mix the two:

- **Capture sessions** — logging ON (ring-dump `ts,rssi,ch,len,hex` via
  `ceepew_monitor.py`). Goal: frame contents, lengths, RSSI, channels,
  sequence continuity. Timing columns are indicative only.
- **Timing sessions** — logging OFF (or errors-only). Goal: latency numbers
  via on-device timestamps (§7). No serial hex dumps running.

A result file must state which session type produced it. A latency claim
sourced from a capture-session log is invalid.

## 1. Serial / baud setup

- Preferred capture baud: **921600**. Both DUT ports + monitor must agree:
  `python tools/ceepew_monitor.py --port COMx --port COMy --baud 921600
  --duration 900 --log capture_runN.txt`
- Why not 115200: a full 288 B frame dumps as ~576 hex chars + `ts,rssi,ch,len`
  CSV overhead ≈ 590 chars ≈ 5900 bits on the wire ≈ **~50 ms/frame of UART
  distortion at 115200**. At 921600 the same dump costs ≈ 6.4 ms/frame —
  still non-zero (hence §0), but it stops the log path from collapsing the
  RF duty cycle. Keep frames back-to-back only in timing sessions where the
  dump path is compiled out.

## 2. Per-run record table

Copy one row per run into the run notes / sidecar `.txt`. Every field is
mandatory.

| Run | Date (UTC) | Adapter | Channel | Duration | Frames | File sha256 |
|-----|-----------|---------|---------|----------|--------|-------------|
| run1 | YYYY-MM-DD HH:MM | on-node promiscuous (DUT self-log) | 6 (fixed, no hop) | 900 s | <from converter stdout `parsed=`> | `certutil -hashfile run1.pcap SHA256` / `sha256sum` |
| run2 | … | 3rd-ESP32 sniffer (SD pcap) | 1 (fixed, no hop) | … | … | … |

Field notes:

- **Adapter**: `on-node promiscuous` (the DUT's own RX-callback ring-dump) or
  `3rd-ESP32 sniffer` (see `tools/capture/ota_sniffer_plan.md`). Never leave blank —
  the two adapters have different timestamp domains.
- **Channel**: fixed single channel per run (default 1 or 6). No hopping
  during a run. Record in the pcap filename: `<date>_ch<CH>_<config>.pcap`.
- **Duration / frame count**: from the monitor log + converter summary line.
- **sha256**: hash the final `.pcap`, not the intermediate `.txt` log.

## 3. Conversion: log → pcap

```powershell
# Basic (linktype 105 IEEE80211, no radiotap)
python tools/capture/serial_to_pcap.py capture_run1.txt -o run1_ch6.pcap

# With RSSI/channel preserved as radiotap (linktype 127)
python tools/capture/serial_to_pcap.py capture_run1.txt -o run1_ch6_rt.pcap --radiotap

# Self-test (3 synthetic lines: full 288 B, short ACK, len-mismatch)
python tools/capture/serial_to_pcap.py --self-test
```

Converter checks (see stdout/stderr):

- `LEN_MISMATCH`: declared `len` ≠ actual hex bytes — actual bytes are written,
  line is flagged. Investigate before trusting the run.
- `non_full_size`: actual ≠ 288 B (249 payload + 39 header). Expected for
  HW-ACK (0x1d) control frames (§5); anything else short is a finding.
- `skipped`: non-ring-dump log lines ignored (boot text, diagnostics).

## 4. tshark analysis commands

Length histogram (Fig 3b input — payload-length distribution):

```powershell
tshark -r run1_ch6_rt.pcap -T fields -e data.len | Sort-Object | Group-Object | Sort-Object Count -Descending | Format-Table Count, Name
```

Per-channel counts (should be a single channel; anything else is leakage or a
hop left enabled):

```powershell
tshark -r run1_ch6_rt.pcap -T fields -e radiotap.channel.freq | Group-Object | Format-Table Count, Name
# No-radiotap variant: group by the converter log's ch column instead.
```

Airtime table (reuse the single-source calculator — do not hand-compute):

```powershell
python tools/wireshark/airtime_calc.py
python tools/wireshark/airtime_calc.py --dwell   # frames-per-5s-dwell budget
```

Per-packet table for the Fig 3b/3c recompute (join key `wlan.seq`, §6):

```powershell
tshark -r run1_ch6_rt.pcap -T fields -e frame.time_epoch -e wlan.sa -e wlan.da -e wlan.seq -e data.len -e radiotap.dbm_antsignal -Y "wlan.fc.type==0 || wlan.fc.type==2" -E header=y -E separator=, > fig3_recompute.csv
```

## 5. HW-ACK (0x1d) vs application frames separation

The 0x1d HW-ACK control frames are short link-layer acknowledgements, not
application delivery. Mixing them into delivery/timing statistics inflates
frame counts and biases length histograms. Separate first:

```powershell
# Count ACK-subtype frames vs everything else (adjust filter to the DUT OUIs)
tshark -r run1_ch6_rt.pcap -Y "wlan.fc.type==1" | Measure-Object -Line
tshark -r run1_ch6_rt.pcap -Y "wlan.fc.type==0 || wlan.fc.type==2" | Measure-Object -Line
```

Rule: Fig 3b/3c inputs use data/action frames only; ACK frames get their own
row (count + mean RSSI) and are excluded from delivery-ratio denominators.
The converter's `non_full_size` counter is the first-pass check that the only
short frames present are the expected ACK subtype — confirm with the tshark
split above before signing off a run.

## 6. Join key: wlan.seq

All cross-source joins use **`wlan.seq`** (802.11 sequence number) as the key:

- Loss / retry accounting: gaps in `wlan.seq` per transmitter (`wlan.sa`) =
  lost frames; `wlan.fc.retry == 1` marks ARQ retries of an earlier seq
  (same airtime row as the original — see `airtime_calc.py` RETRY_FILTER).
- DUT-log ↔ sniffer-pcap correlation: match receiver serial `len,hex` lines
  to sniffer records on `wlan.seq` + frame bytes; serial timestamps are the
  cross-check column, sniffer `frame.time_epoch` is the authority.
- Replay/monotonicity audits (§8 in `tools/capture/ota_sniffer_plan.md`): group by
  transmitter, assert CCMP packet numbers strictly increase per `wlan.seq`
  order; any retreat is a finding.

## 7. Replay test framing

Goal: prove a captured legitimate frame, re-injected verbatim, is rejected
(same CCMP packet number / nonce → replay-window drop).

1. Capture N legitimate CCMP frames in a capture session; note their
   `wlan.seq` + CCMP packet numbers.
2. On node A (test TX role), replay one captured payload byte-for-byte over
   the air via raw 802.11 TX on the same channel. If the peer filters by
   source MAC, **spoof the original transmitter MAC** on the replay frame;
   record whether spoofing was needed.
3. Keep replay runs in **separate `.pcap` files** from measurement runs.
4. Evidence required — a **receiver-serial + promiscuous-log pair**:
   - receiver serial sidecar shows **no duplicate delivery** to the
     application layer for the replayed seq/PN, AND
   - promiscuous/sniffer log shows the replayed frame **was on the air**
     (proves the test actually injected, not that TX failed silently).
   One half without the other is inconclusive.

## 8. Cleartext-injection test (replaces unverified Table V cell)

Goal: fill the paper's unverified Table V cell — does an encrypted peer
accept plaintext from an unconfigured sender?

1. Register the encrypted peer on node B (full pairing/key derivation, so B
   expects CCMP-protected frames from that peer identity).
2. From unconfigured node A (no keys, no pairing), transmit a **plaintext**
   (unencrypted) frame to B using the peer's addressing.
3. Observe: **does B's rx-callback fire / deliver to the application?**
   - Callback fires + delivers → FAIL finding: cleartext accepted by an
     encrypted peer (record pcap + both serial logs).
   - No callback / drop counted → PASS: cleartext rejected.
4. Evidence: same pair as §7 — B's serial log (callback fire/no-fire) plus
   the promiscuous pcap proving the plaintext frame was actually on the air.
   Until this test is executed and its evidence pair is filed, the Table V
   cell stays marked unverified.

## 9. On-device timing (never serial stamps)

Serial timestamps measure the UART, not the RF path. All latency numbers come
from hardware timestamps:

- Sample `esp_timer_get_time()` (µs, 64-bit) — or the CPU cycle counter for
  sub-µs resolution — at **TX-complete** and at **RX-callback entry**. No
  `printf` in either path.
- Toggle a spare **GPIO at the same two points** (`gpio_set_level`, ~100 ns
  scale) so a logic analyser / second capture can verify the deltas
  independently of firmware bookkeeping.
- OTA delta per frame = `t_rx_cb − t_tx_done`, joined on `wlan.seq` (§6).
- Report mean / p50 / p99 over **≥ 1000 frames per configuration**, and state
  ESP-IDF version, CPU frequency, and channel in every results file.
- Capture sessions (§0) may embed these hardware timestamps in frame metadata
  / sidecar logs, but the serial arrival stamp itself is never a timing input.
