# E1 Frame Length Report: E1_C_ceepew_20261018

## Source Counts

| Source | Count |
|--------|-------|
| ARQ_TX | 170 |
| ARQ_RX | 168 |
| RADIO_RX | 175 |
| SECURE_CHAT_TX | 166 |
| BENCH58_TX | 160 (forged: 16) |
| BENCH58_DONE | 2 (sent: 160) |

## Length Histograms

### ARQ TX
```
{77:2, 173:8, 233:94, 237:66}
```

### RADIO RX
```
{2:1, 5:1, 9:3, 78:2, 174:8, 234:94, 238:66}
```

### SECURE CHAT TX
```
{1:7, 58:159}
```

### BENCH58 TX
```
{58:160}
```

## ARQ Sequence Coverage

- Range: 0x00..0x55
- Unique: 86
- Gaps: 0
- **ZERO gaps** (complete coverage)

## Ring-Line Truncation Audit

- Total ring rows: 10668
- Clean: 2055 (19.3%)
- Corrupted: 8613 (80.7%)
- Clean length histogram: `{28:250, 213:174, 235:159, 239:181, 242:108, 246:209, 261:111, 300:309}`
- Corrupted length histogram: `{21:1, 213:780, 235:809, 236:362, 239:984, 242:782, 246:1036, 261:1000, 300:1907}`

## Limitations

Ring/pcap-derived length histograms from this run are length-biased due to UART interleaving corruption and must not be used as the primary frame-size evidence. The intact short-line histograms above are the primary source.

