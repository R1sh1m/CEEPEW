# X7 - Channel-hop verification (OTA + sniffer)

## Purpose
Verify channel hopping by observing per-channel dwell via channel-scanning sniffer.

## Build
- build_bench3 (Full profile, FEC off, bench flags on)
- Sniffer: tools/capture/sniffer_fw/ (flashed to COM6 or spare board)
- ESP-IDF v6.0.3, 160 MHz, PM disabled

## Procedure
1. Flash build_bench3 to COM3 and COM4:
   idf.py -B build_bench3 -p COM3 flash; idf.py -B build_bench3 -p COM4 flash
2. Flash sniffer firmware to COM6:
   idf.py -C tools/capture/sniffer_fw -p COM6 flash
3. Start monitor on COM3 and COM4 (DUTs):
   python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 300 --log data/raw/X7_channel_hop_20261029/capture_dual.log
4. Start sniffer monitor on COM6:
   python tools/ceepew_monitor.py --port COM6 --duration 300 --log data/raw/X7_channel_hop_20261029/sniffer_com6.log
5. Pair devices, observe channel hopping
6. Sniffer captures frames across channels

## Expected outcome
- Observed dwell per channel matches schedule
- Channel occupancy reported

## Analysis & PCAP Conversion
1. Convert sniffer capture to pcap:
   python tools/capture/serial_to_pcap.py --radiotap data/raw/X7_channel_hop_20261029/sniffer_com6.log --out data/raw/X7_channel_hop_20261029/hop_traffic.pcap
2. Count frames per channel and analyze with tshark:
   python tools/capture/analyze_pcap.py --pcap data/raw/X7_channel_hop_20261029/hop_traffic.pcap
3. Compare observed dwell vs expected interval
