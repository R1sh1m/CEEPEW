# X10 - Clean capture for figures and census (OTA)

## Purpose
Produce clean OTA captures for paper figures (Fig 3b/3c) and frame census.

## Build
- build_bench3 (Full profile, FEC off, bench flags on)
- ESP-IDF v6.0.3, 160 MHz, PM disabled
- Sniffer on COM6 (optional for radiotap)

## Procedure
1. Flash build_bench3 to COM3 and COM4
2. Flash sniffer firmware to COM6: idf.py -C tools/capture/sniffer_fw -p COM6 flash
3. Start monitor on COM3 and COM4: python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 900 --log data/raw/X10_clean_capture_20261029/capture_dual.log
4. Start sniffer capture on COM6: python tools/ceepew_monitor.py --port COM6 --duration 900 --log data/raw/X10_clean_capture_20261029/sniffer_com6.log
5. Pair devices, run normal session
6. Capture 900s of clean traffic (no forged/replay injection)

## Expected outcome
- Clean pcap for figures (Figure 2 in paper)
- Frame census data
- Entropy analysis data

## Analysis & Figure Generation
1. Convert sniffer capture to pcap:
   python tools/capture/serial_to_pcap.py --radiotap data/raw/X10_clean_capture_20261029/sniffer_com6.log --out data/raw/X10_clean_capture_20261029/clean_traffic.pcap
2. Open in Wireshark with Lua dissector:
   & "C:\Program Files\Wireshark\Wireshark.exe" -r data/raw/X10_clean_capture_20261029/clean_traffic.pcap -X lua_script:tools/capture/wireshark/ceepew_espnow.lua
3. Capture unedited GUI window screenshots:
   - Panel (a) Mode B CCMP: filter `wlan.addr == 8c:94:df:97:d0:c0 || wlan.addr == 1c:c3:ab:c2:52:ac` -> save to `data/derived/wireshark_views/shots/wireshark_modeB_ccmp_dut_frames.png`
   - Panel (b) Mode C ESL: filter `ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000` -> save to `data/derived/wireshark_views/shots/wireshark_modeC_esl_frame1366.png`
4. Rebuild paper Figure 2:
   python tools/scratch/build_wireshark_crop_figure.py
5. Run tshark analysis:
   Compute entropy, frame lengths, RSSI with tools/capture/analyze_pcap.py
