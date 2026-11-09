# Hardware session runbook

Everything in the manuscript that does not need a radio is finished: the code
compiles for all five configurations, the host tests pass 17/17, the arithmetic
model agrees with the text, and the publication gate authorises the PDF. This
document is the sequence for the session that closes the remaining
hardware-dependent claims. Work it top to bottom; each step names the artifact
it produces, because an artifact that is not sealed into a manifest does not
enter `results/registry.json` and therefore cannot enter the paper.

---

## 0. What is still missing, and why it needs a board

| Claim | Registry entry it will create | Campaign |
|---|---|---|
| Per-stage timing with the associated-data-bound AEAD actually in the frame | `X1-*` medians with cluster bootstrap intervals | X1 |
| Same-board AEAD and signature baseline, including the hardware-AES and mbedTLS-software-fallback labels | `X12-BENCHCASE-*` | X12 |
| Delivery accounting with an explicit drop breakdown, PHY-dropped versus tag-rejected separated | `X8-*` | X8 |
| Forged-tag rejection counted at the layer that rejects it | `X2-*` | X2 |
| Replay, header-tamper, DoS-survival, hop dwell, region high-water, distance sweep, capture census | see the X3--X11 rows in `ISSUES.md` | X3--X11 |

X1, X8 and X12 have runners in `tools/campaign/`. X2--X7 and X9--X11 are
procedures rather than scripts: the capture is taken with the monitor, the
analysis is written against the sealed log, and the numbers are appended to the
registry only after `registry_append.py --verify-all` passes. Do not invent a
ninth analysis script mid-session; write the analysis after the session, when
the log is stable.

---

## 1. Physical setup

| Role | Port | Wi-Fi MAC | BLE MAC |
|---|---|---|---|
| Device A | `COM3` | `8c:94:df:97:d0:c0` | record from the boot banner |
| Device B | `COM4` | `1c:c3:ab:c2:52:ac` | record from the boot banner |
| Sniffer | `COM6` | `58:2a:bd:d8:80:38` | n/a |

- Wire **GPIO14 of A to GPIO14 of B**. The timing rig takes its synchronisation
  edge from that line; without it the cross-board latency figures are not
  comparable with the archived ones.
- Put both boards on the same surface, 20 cm apart, for the propagation figure.
- Confirm no other process holds the ports. A monitor left running from an
  earlier session is the most common cause of a silent zero-byte capture.

Sanity check before anything else:

```powershell
python tools/ceepew_pipeline.py --list-ports
```

Expect exactly three ports. If a board enumerates under a different number than
the table above, update the table in this file and in the manifest command
rather than silently using the new number.

---

## 2. Build the campaign images

Configuration must go through `SDKCONFIG` and `SDKCONFIG_DEFAULTS`. Passing
`-D CONFIG_*` on the `idf.py` command line is silently ignored and produces a
shipping image that measures the wrong binary (ISS-T07).

```powershell
$env:IDF_PATH='C:\esp\v6.1\esp-idf'
$env:IDF_TOOLS_PATH='C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python_env\idf6.0_py3.13_env'
& 'C:\esp\v6.1\esp-idf\export.ps1' | Out-Null

python tools/campaign/build_matrix.py --configs bench fec lite
```

Every arm must print `OK`. An arm that prints `FAILED` is a configuration that
did not produce the symbols it promised, and its build directory must not be
flashed. The images live at:

| Arm | Directory | Use |
|---|---|---|
| bench | `build_bench3` | X1, X12, X2--X7 |
| fec | `build_fec2` | X6 FEC ablation |
| lite | `build_lite2` | Lite-profile rows |

`build_bench3` is the workhorse: Full profile, FEC compiled out (the shipping
default), benchmark module and scripted campaign linked in.

---

## 3. Flash and start a session

Flash the same image to both boards. They are symmetric; the lower BLE MAC
becomes the initiator without any configuration.

```powershell
idf.py -B build_bench3 -p COM3 flash
idf.py -B build_bench3 -p COM4 flash
```

Development mode is on in this image, so the UI advances without the OLED and
the session code is the fixed headless string. Power both boards within a few
seconds of each other. A healthy start looks like this in the monitor:

```
I (....) session: BLE advertising, commitment=...
I (....) fsm: phase DISCOVERY -> PAIRING
I (....) fsm: phase PAIRING -> HANDOFF
I (....) fsm: phase HANDOFF -> ACTIVE
I (....) bench: [BENCH58_TX] i=0
...
I (....) bench: [BENCH58_DONE] sent=80
```

If it stalls before `ACTIVE`, capture the log anyway and read
`DEVICE_LOG_FINDINGS.md` after ingestion; do not reflash repeatedly, because
each reflash costs the pairing state and gains no information.

---

## 4. Capture

The monitor writes one combined file plus one file per port. Keep both: the
combined file is what the runners parse, the per-port files are what makes a
disagreement between the two boards visible.

```powershell
python tools/ceepew_monitor.py --port COM3 --port COM4 `
    --duration 900 --log-per-port `
    --log data/raw/X1_20261023/capture_dual.log
```

### X1, per-stage timing

- Run the 80-frame scripted campaign end to end, once per board pair.
- The image emits `[BENCH58_TX]`, `[BENCHPRIM] x25519|ed25519_sign|ed25519_verify`
  and `[BENCHCASE]` lines. Every record carries `cpu_hz`; a record at anything
  other than 160 MHz is a failed run, not a fast one.
- Analysis:

```powershell
python tools/campaign/run_x1_timing.py `
    --log data/raw/X1_20261023/capture_DEVICE_A.log `
    --log data/raw/X1_20261023/capture_DEVICE_B.log `
    --out-json data/derived/X1_20261023.json --bootstraps 10000 --seed 20261023
```

The runner fails loudly on a missing CPU-frequency record rather than assuming
160 MHz. It reports a cluster bootstrap over frames, not over samples, because
samples inside one frame are not independent.

### X12, same-board cryptographic baseline

```powershell
python tools/campaign/run_x12_aead.py `
    --log data/raw/X12_20261023/capture_DEVICE_A.log `
    --out-json data/derived/X12_20261023.json
```

Each case carries `hw_aes` and `opt`. A case the build does not provide is
reported unavailable; it is never dropped silently. Read the output before
sealing it: an AES row with `hw_aes=0` is the mbedTLS software fallback and must
be labelled that way in the paper, not presented as a hardware measurement.

### X8, delivery accounting

```powershell
python tools/campaign/run_x8_delivery.py `
    --log data/raw/X8_20261023/capture_dual.log `
    --out-json data/derived/X8_20261023.json
```

The accounting separates three deficits that are routinely conflated: frames
the sender never transmitted, frames the PHY dropped with a bad FCS, and frames
that arrived intact but failed authentication. Only the first two are losses.
A frame rejected at the Ascon tag is a delivered frame that the receiver
refused, and counting it as a delivery failure inflates the loss rate.

### X2--X7, X9--X11

Capture with the same monitor, one directory per campaign id, and keep the
`.pcap` beside the logs when the sniffer is involved. Do not analyse them until
the session is over; the analysis needs the complete file, and a half-written
log produces a plausible but wrong interval.

#### Wireshark PCAP & Figure 2 Screenshot Generation (X10 & X7)
Campaigns X10 and X7 use the dedicated sniffer on `COM6` (`build_sniffer/ceepew_sniffer.bin`, app: `components/sniffer/sniffer_app.c`) alongside `COM3` and `COM4`:
1. **Capture traffic concurrently:**
   ```powershell
   # In terminal 1 (DUTs):
   python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 900 --log data/raw/X10_clean_capture_20261029/capture_dual.log
   # In terminal 2 (Sniffer):
   python tools/ceepew_monitor.py --port COM6 --duration 900 --log data/raw/X10_clean_capture_20261029/sniffer_com6.log
   ```
2. **Convert sniffer capture to PCAP:**
   ```powershell
   python tools/capture/serial_to_pcap.py --radiotap data/raw/X10_clean_capture_20261029/sniffer_com6.log --out data/raw/X10_clean_capture_20261029/clean_traffic.pcap
   ```
3. **Open in Wireshark with Lua dissector:**
   ```powershell
   & "C:\Program Files\Wireshark\Wireshark.exe" -r data\raw\X10_clean_capture_20261029\clean_traffic.pcap -X lua_script:tools\capture\wireshark\ceepew_espnow.lua
   ```
4. **Capture GUI Window Screenshots:**
   - **Panel (a) Mode B (CCMP Baseline):** Filter `wlan.addr == 8c:94:df:97:d0:c0 || wlan.addr == 1c:c3:ab:c2:52:ac` &rarr; save as `data/derived/wireshark_views/shots/wireshark_modeB_ccmp_dut_frames.png`.
   - **Panel (b) Mode C (Full CEE-PEW):** Filter `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000` &rarr; save as `data/derived/wireshark_views/shots/wireshark_modeC_esl_frame1366.png`.
5. **Recompile Figure 2 PDF for The Paper:**
   ```powershell
   python tools/scratch/build_wireshark_crop_figure.py
   ```
   This automatically generates `ceepew_wireshark_real_crop.pdf` at native DPI for inclusion in `docs/paper/The Paper/main.tex`.

---

## 5. Seal the session

A measurement without a manifest does not exist.

```powershell
python tools/campaign/manifest.py --campaign X1_20261023 `
    --board "Device A:COM3:8c:94:df:97:d0:c0" `
    --board "Device B:COM4:1c:c3:ab:c2:52:ac" `
    --runner tools/campaign/run_x1_timing.py `
    --firmware build_bench3/ceepew.bin --build-dir build_bench3 `
    --artifact data/raw/X1_20261023/capture_DEVICE_A.log `
    --artifact data/raw/X1_20261023/capture_DEVICE_B.log `
    --artifact data/derived/X1_20261023.json `
    --notes "Full profile, FEC out, 160 MHz, PM disabled, D14 sync wired"
```

The manifest hashes the firmware, the build directory's `sdkconfig`, every
artifact and the runner, and records the git SHA and the IDF release. Then:

```powershell
python tools/campaign/registry_append.py --manifest data/manifests/X1_20261023.json `
    --id X1-ED25519-VERIFY-MEDIAN --value <median> --unit us --n <frames>

python tools/campaign/registry_append.py --verify-all
```

`--verify-all` must print `VERDICT: OK`. If it does not, the corresponding
artifact changed after sealing, and the entry it backs is no longer supported by
the file on disk.

---

## 6. Put the numbers into the paper

Only entries that exist in `results/registry.json` may be quoted. After the
appends:

1. Update the affected tables in `docs/paper/The Paper/main.tex`.
2. Recompile three passes, then copy: `Copy-Item main.pdf CEEPEW_full_13pages.pdf -Force`.
3. `python tools/recompute_paper_numbers.py` must not gain a new mismatch.
4. `python tools/publication_gate.py --paper the_paper --external` must exit 0
   with an artifact newer than the `.tex`.

The gate is not advisory. A submission whose newest gate artifact predates the
manuscript is not authorised, however green the earlier run was.
