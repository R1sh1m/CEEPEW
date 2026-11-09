# Master campaign execution script
# Run this when boards are connected (COM3, COM4, COM6 sniffer)
# All builds are pre-prepared; this script only captures

# Prerequisites:
# - COM3: Device A (8c:94:df:97:d0:c0)
# - COM4: Device B (1c:c3:ab:c2:52:ac)
# - COM6: Sniffer (58:2a:bd:d8:80:38) [optional, for X7/X10]

# Step 1: Verify builds
Write-Host "=== Verifying builds ==="
foreach ($b in @("build_bench3", "build_fec2", "build_lite2", "build_prod")) {
    if (Test-Path "$b/ceepew.bin") { Write-Host "  OK: $b/ceepew.bin" }
    else { Write-Host "  MISSING: $b/ceepew.bin" }
}
if (Test-Path "build_sniffer/ceepew_sniffer.bin") {
    Write-Host "  OK: build_sniffer/ceepew_sniffer.bin"
} else {
    Write-Host "  MISSING: build_sniffer/ceepew_sniffer.bin"
}

# Step 2: List available ports
Write-Host ""
Write-Host "=== Available ports ==="
python tools/ceepew_pipeline.py --list-ports

# Step 3: Flash instructions
Write-Host ""
Write-Host "=== Flash commands (run manually) ==="
Write-Host "  Bench (FEC off): idf.py -B build_bench3 -p COM3 flash; idf.py -B build_bench3 -p COM4 flash"
Write-Host "  FEC on:          idf.py -B build_fec2 -p COM3 flash; idf.py -B build_fec2 -p COM4 flash"
Write-Host "  Lite:            idf.py -B build_lite2 -p COM3 flash; idf.py -B build_lite2 -p COM4 flash"
Write-Host "  Sniffer:         idf.py -B build_sniffer -p COM6 flash  (app: components/sniffer/sniffer_app.c)"

# Step 4: Capture commands
Write-Host ""
Write-Host "=== Capture commands ==="
Write-Host "  X4 header tamper: python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 300 --log data/raw/X4_header_tamper_20261029/capture_dual.log"
Write-Host "  X5 DoS sweep:     python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 300 --log data/raw/X5_dos_rate_sweep_20261029/capture_dual.log"
Write-Host "  X6 FEC off:       python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 480 --log data/raw/X6_fec_ablation_20261029/capture_fecoff.log"
Write-Host "  X6 FEC on:        python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 480 --log data/raw/X6_fec_ablation_20261029/capture_fecon.log"
Write-Host "  X7 channel hop:   python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 300 --log data/raw/X7_channel_hop_20261029/capture_dual.log"
Write-Host "                    python tools/ceepew_monitor.py --port COM6 --duration 300 --log data/raw/X7_channel_hop_20261029/sniffer_com6.log"
Write-Host "  X9 memory:        python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 480 --log data/raw/X9_memory_20261029/capture_dual.log"
Write-Host "  X10 clean (DUT):  python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 900 --log data/raw/X10_clean_capture_20261029/capture_dual.log"
Write-Host "  X10 clean (Snif): python tools/ceepew_monitor.py --port COM6 --duration 900 --log data/raw/X10_clean_capture_20261029/sniffer_com6.log"
Write-Host "  X11 multi-dist:   python tools/ceepew_monitor.py --port COM3 --port COM4 --duration 300 --log data/raw/X11_multi_distance_20261029/capture_20cm.log"

# Step 5: Wireshark PCAP Conversion & App Screenshots
Write-Host ""
Write-Host "=== Wireshark PCAP & Screenshot Procedure (for Paper Fig 2) ==="
Write-Host "  1. Convert sniffer capture to PCAP:"
Write-Host "     python tools/capture/serial_to_pcap.py --radiotap data/raw/X10_clean_capture_20261029/sniffer_com6.log --out data/raw/X10_clean_capture_20261029/clean_traffic.pcap"
Write-Host "  2. Open PCAP in Wireshark with Lua dissector:"
Write-Host '     & "C:\Program Files\Wireshark\Wireshark.exe" -r data\raw\X10_clean_capture_20261029\clean_traffic.pcap -X lua_script:tools\capture\wireshark\ceepew_espnow.lua'
Write-Host "  3. Capture Wireshark Screenshots:"
Write-Host '     Panel (a) Mode B CCMP filter: wlan.addr == 8c:94:df:97:d0:c0 || wlan.addr == 1c:c3:ab:c2:52:ac'
Write-Host '     Save screenshot as: data\derived\wireshark_views\shots\wireshark_modeB_ccmp_dut_frames.png'
Write-Host '     Panel (b) Mode C ESL filter: ceepew.esl.magic0 == 0x45 && ceepew.esl.version == 1 && ceepew.esl.seq < 100000'
Write-Host '     Save screenshot as: data\derived\wireshark_views\shots\wireshark_modeC_esl_frame1366.png'
Write-Host "  4. Regenerate Paper Figure 2 PDF:"
Write-Host "     python tools/scratch/build_wireshark_crop_figure.py"

# Step 6: Analysis commands
Write-Host ""
Write-Host "=== Analysis commands ==="
Write-Host "  X6: python tools/campaign/run_x8_delivery.py --log data/raw/X6_fec_ablation_20261029/capture_fecoff.log --log data/raw/X6_fec_ablation_20261029/capture_fecon.log --out-json data/derived/X6_fec_ablation.json"
Write-Host "  X8: python tools/campaign/run_x8_delivery.py --log <capture> --out-json data/derived/X8.json"
Write-Host "  X1: python tools/campaign/run_x1_timing.py --log <capture> --out-json data/derived/X1.json --out-csv data/derived/X1.csv"
Write-Host "  X12: python tools/campaign/run_x12_aead.py --log <capture> --out-json data/derived/X12.json"

Write-Host ""
Write-Host "=== All campaigns prepped. Ready for board connection. ==="
