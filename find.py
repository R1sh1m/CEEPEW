with open('docs/paper/The Paper/main.tex') as f:
    for i,l in enumerate(f):
        if any(x in l for x in ['ed25519', 'x25519', 'ascon', 'timing', 'BENCHPRIM']):
            print(i+1, repr(l))
            if i > 500:  # don't go too far
                break
