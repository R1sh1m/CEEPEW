import json
for f in ['data/derived/X6_fec_ablation_20261030.json', 'data/derived/X8_delivery_20261031b.json', 'data/derived/X12_20261029j.json', 'data/derived/X12_20261029g.json']:
    try:
        d = json.load(open(f))
        print('===', f.split('/')[-1], '===')
        print(json.dumps(d, indent=2))
        print('\n' + '='*60 + '\n')
    except Exception as e:
        print('ERR', f, e)
