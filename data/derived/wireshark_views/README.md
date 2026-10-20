# Wireshark views — genuine GUI screenshots (2026-10-19)

These are **real Wireshark 4.6.9 GUI screenshots** taken from the actual capture
files in this repository (no mock-ups, no synthetic frames, no re-drawn bytes).
Each `.png` is a `PrintWindow` capture of the live Wireshark window after loading
the named pcap with the named display filter.

Captured: 2026-10-19, Wireshark 4.6.9 (x64), CEE-PEW dissector
`tools/capture/wireshark/ceepew_espnow.lua`.

## Files

| File | Shows |
|---|---|
| `shots/wireshark_modeB_ccmp_dut_frames.png` | **Mode B (CCMP)**: the 10 captured DUT→DUT Action frames, frame 43 selected; `IEEE 802.11 Action, Flags: ·p·····` (protected bit set), `Data (201 bytes)` as opaque ciphertext. Shows that Mode-B traffic carries no readable payload on air. |
| `shots/wireshark_modeC_esl_frame1366.png` | **Mode C (full CEE-PEW)**: the 6 intact ESL frames after the guard filter, frame 1366 selected; Protocol column `CEE-PEW`, Info `seq=14 nonce=27`; detail tree contains the `CEE-PEW ESP-NOW Frame` node; hex shows `45 53 01` ESL magic and the encrypted payload. |
| `E1_C_ringdump_reconstructed.pcap` | The Mode-C pcap reconstructed from the on-node ring dump of run `E1_C_ceepew_20261018` (input to the Mode-C screenshot). |

## Provenance of each source capture

### Mode B screenshot
- Source pcap: `data/raw/E1_B_ccmp_20261017/DEVICE_A.pcap` (sealed run, read-only).
- Display filter used: `wlan.addr == 8c:94:df:97:d0:c0 || wlan.addr == 1c:c3:ab:c2:52:ac`
- Selected frame: **43** (`246 Action`, first DUT frame in the file).
- 10 of 1417 frames match the DUT address pair; **all 10 are `protected=True`**
  (CCMP), `data.len` 201–268 B. Verified with:
  `tshark -r <pcap> -Y "wlan.addr==8c:94:df:97:d0:c0 || wlan.addr==1c:c3:ab:c2:52:ac" -T fields -e frame.number -e wlan.fc.protected`

### Mode C screenshot
- Source: `data/raw/E1_C_ceepew_20261018/capture_dual.log` (sealed run, read-only)
  → converted with `tools/capture/serial_to_pcap.py --radiotap` to
  `E1_C_ringdump_reconstructed.pcap` (10,293 frames total; 6 frames carry an
  intact ESL header and are the ones shown).
- Display filter: `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000`
- Selected frame: **1366**. Ground-truth dissection (tshark `-V`):
  ```
  CEE-PEW ESP-NOW Frame
      Frame Type: UNKNOWN(0x05)
      ESL Header (24 bytes)
          Magic Byte 0: 0x45 ; Magic Byte 1: 0x53 ; Version: 1 ; Flags: 0x00
          Timestamp (seconds): 71
          ESL Sequence Number: 14        <- replay-window msg_id
          Nonce Counter: 27              <- crypto nonce
      ESL Payload (51 bytes)
      ESL CRC-32: 0xdbfc7ea1
  ```
  The 6 frames carry seq/nonce pairs (4,6) (14,27) (23,44) (31,61) (42,82) (50,98).

## Caveats (read before using in the paper)

1. **Ring-dump capture is lossy.** Both pcaps were reconstructed from the DUT's
   own promiscuous ring dump over the 115200-baud console, where the IDF log task
   interleaves with the dump task. In Mode C only ~19–22 % of ring rows survive
   intact; the pcap contains exactly those survivors. No byte was invented or
   repaired — corrupted rows were *skipped*, never patched.
2. **`Frame Type: UNKNOWN(0x05)`** in the Mode-C tree: the dissector reads the
   first payload byte as the message type, which is only meaningful for control
   frames (0x00–0x04). For encrypted DATA frames that byte is part of the
   ciphertext, so `UNKNOWN(0xNN)` is expected. The **ESL header fields
   (seq/nonce/timestamp) are the trustworthy fields** in this figure.
3. **No Mode-A (cleartext) figure exists.** `data/derived/E1_A_plain_20261018/*.pcap`
   contain **zero** frames with a DUT MAC as source or destination, and zero hits
   for the BENCH58 `'A'`-run marker, even though the run log shows 86 `RADIO-RX`
   and 77 `Message complete` events — i.e. the traffic existed but its ring rows
   were destroyed by UART interleaving. A cleartext-visibility figure requires a
   fresh capture (e.g. with the live log level lowered during the dump), not a
   re-render of these files.
4. **Dissector fix applied 2026-10-19** (`tools/capture/wireshark/ceepew_espnow.lua`):
   the Info column previously read the 8-byte ESL seq/nonce as big-endian
   (displaying `seq=1008806316530991104` instead of `14`); it now uses
   `le_uint64()`, matching the little-endian field dissection (`add_le`) that was
   already correct.

## Reproduce

```powershell
# GUI (what the screenshots did):
& "C:\Program Files\Wireshark\Wireshark.exe" -r "<pcap>" `
  -Y "<filter>" -X lua_script:"tools\capture\wireshark\ceepew_espnow.lua" -g 1366

# Headless verification of the same content:
& "C:\Program Files\Wireshark\tshark.exe" -X lua_script:"tools\capture\wireshark\ceepew_espnow.lua" `
  -r "data\derived\wireshark_views\E1_C_ringdump_reconstructed.pcap" `
  -Y "ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000" `
  -T fields -e frame.number -e ceepew.esl.seq -e ceepew.esl.nonce_counter
```
