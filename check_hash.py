import hashlib
s=open('docs/paper/The Paper/FINAL/ceepew_unrestricted.tex','rb').read()
m=open('docs/paper/The Paper/main.tex','rb').read()
print(hashlib.sha256(s).hexdigest())
print(hashlib.sha256(m).hexdigest())
print(s==m)
