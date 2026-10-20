import sys

BANNED = [
    'delve', 'crucial', 'pivotal', 'seamless', 'leverage', 'holistic',
    'landscape', 'it is important to note', 'notably', 'crucially',
    'superseded', 'pending', 'placeholder', 'todo', 'now fixed',
    'zero-heap', 'state-of-the-art', 'novel', 'robust', 'comprehensive',
    'in conclusion', 'in summary', 'furthermore,', 'moreover,',
]

def main(path):
    text = open(path, encoding='utf-8').read()
    low = text.lower()
    found = 0
    for word in BANNED:
        count = low.count(word)
        if count:
            print(f'BANNED  {word!r:40s}  count={count}')
            found += 1
    paragraphs = [p for p in text.split('\n\n') if p.strip()]
    pages = max(1, len(text.split()) // 900)
    em_dashes = text.count('\u2014')
    print(f'em dashes per page: {em_dashes / pages:.1f}  (limit: 2)')
    lengths = [len(p.split()) for p in paragraphs]
    if lengths:
        print(f'paragraphs: {len(lengths)}  min/max words: {min(lengths)}/{max(lengths)}')
    for i, p in enumerate(paragraphs):
        if p.count('(') > 3:
            print(f'many parentheses in paragraph {i + 1}')
    if found == 0:
        print('PASS: no banned tokens found')
    # Extend: check that every number in the text appears in results/registry.json

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Usage: python tools/paper_lint.py <latex_or_text_file>')
        sys.exit(1)
    main(sys.argv[1])
