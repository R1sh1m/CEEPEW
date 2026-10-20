# E1 Frame Length Report: E1_A_plain_20261018

## Source Counts

| Source | Count |
|--------|-------|
| ARQ_TX | 167 |
| ARQ_RX | 167 |
| RADIO_RX | 173 |
| SECURE_CHAT_TX | 164 |
| BENCH58_TX | 160 (forged: 16) |
| BENCH58_DONE | 2 (sent: 160) |

## Length Histograms

### ARQ TX
```
{12:5, 74:94, 77:2, 78:66}
```

### RADIO RX
```
{2:2, 5:1, 9:3, 13:5, 75:94, 78:2, 79:66}
```

### SECURE CHAT TX
```
{1:5, 58:159}
```

### BENCH58 TX
```
{58:160}
```

## ARQ Sequence Coverage

- Range: 0x00..0x53
- Unique: 84
- Gaps: 0
- **ZERO gaps** (complete coverage)

## Ring-Line Truncation Audit

- Total ring rows: 6432
- Clean: 1433 (22.3%)
- Corrupted: 4999 (77.7%)
- Clean length histogram: `{28:50, 30:309, 32:27, 34:355, 46:250, 114:34, 116:20, 300:270}`
- Corrupted length histogram: `{0:3, 137:170, 161:132, 166:129, 231:212, 242:237, 246:135, 261:271, 300:2304}`

## Limitations

Ring/pcap-derived length histograms from this run are length-biased due to UART interleaving corruption and must not be used as the primary frame-size evidence. The intact short-line histograms above are the primary source.

