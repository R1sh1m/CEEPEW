# E1 Frame Length Report: E1_B_ccmp_20261018

## Source Counts

| Source | Count |
|--------|-------|
| ARQ_TX | 173 |
| ARQ_RX | 174 |
| RADIO_RX | 180 |
| SECURE_CHAT_TX | 170 |
| BENCH58_TX | 160 (forged: 16) |
| BENCH58_DONE | 2 (sent: 160) |

## Length Histograms

### ARQ TX
```
{12:13, 74:92, 77:2, 78:66}
```

### RADIO RX
```
{2:1, 5:1, 9:3, 13:13, 75:94, 78:2, 79:66}
```

### SECURE CHAT TX
```
{1:13, 58:157}
```

### BENCH58 TX
```
{58:160}
```

## ARQ Sequence Coverage

- Range: 0x00..0x57
- Unique: 88
- Gaps: 0
- **ZERO gaps** (complete coverage)

## Ring-Line Truncation Audit

- Total ring rows: 9596
- Clean: 1404 (14.6%)
- Corrupted: 8192 (85.4%)
- Clean length histogram: `{28:95, 32:136, 114:92, 116:127, 213:91, 242:59, 246:96, 300:280}`
- Corrupted length histogram: `{26:1, 213:598, 235:466, 237:233, 239:501, 242:879, 246:938, 261:963, 300:2315}`

## Limitations

Ring/pcap-derived length histograms from this run are length-biased due to UART interleaving corruption and must not be used as the primary frame-size evidence. The intact short-line histograms above are the primary source.

