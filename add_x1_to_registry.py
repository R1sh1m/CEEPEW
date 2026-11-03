import json
with open('results/registry.json') as f:
    reg = json.load(f)
entries = [
    {
        'id': 'BENCH-X1-ED25519-SIGN',
        'description': 'Ed25519 signing (X1 campaign, sealed run)',
        'formula': 'on-device timing',
        'value': '332.26',
        'unit': 'ms',
        'class': 'M-HW',
        'n': 49,
        'source': {
            'run_id': 'X1_20261029g',
            'script': 'tools/campaign/run_x1_timing.py',
            'raw_log': 'data/raw/X1_20261029g/capture_dual.log',
            'ci95': [332.222, 332.349],
            'manifest': 'data/raw/X1_20261029g/manifest.json'
        }
    },
    {
        'id': 'BENCH-X1-ED25519-VERIFY',
        'description': 'Ed25519 verification (X1 campaign, sealed run)',
        'formula': 'on-device timing',
        'value': '663.28',
        'unit': 'ms',
        'class': 'M-HW',
        'n': 50,
        'source': {
            'run_id': 'X1_20261029g',
            'script': 'tools/campaign/run_x1_timing.py',
            'raw_log': 'data/raw/X1_20261029g/capture_dual.log',
            'ci95': [663.200, 663.332],
            'manifest': 'data/raw/X1_20261029g/manifest.json'
        }
    },
    {
        'id': 'BENCH-X1-X25519',
        'description': 'X25519 ECDH (X1 campaign, sealed run)',
        'formula': 'on-device timing',
        'value': '148.0',
        'unit': 'ms',
        'class': 'M-HW',
        'n': 50,
        'source': {
            'run_id': 'X1_20261029g',
            'script': 'tools/campaign/run_x1_timing.py',
            'raw_log': 'data/raw/X1_20261029g/capture_dual.log',
            'ci95': [147.925, 148.002],
            'manifest': 'data/raw/X1_20261029g/manifest.json'
        }
    }
]
reg['entries'].extend(entries)
with open('results/registry.json', 'w') as f:
    json.dump(reg, f, indent=2)
print('done')
