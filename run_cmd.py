import subprocess, sys
cmd = sys.argv[1:]
p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
out = p.communicate()[0]
open(sys.argv[2], 'wb').write(out)
print('exit', p.returncode, 'bytes', len(out))