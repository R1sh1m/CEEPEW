import sys, time, serial
port_name = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
port = serial.Serial(port_name, 115200, timeout=1)
start = time.time()
while time.time() - start < 180:
    data = port.read(1024)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
port.close()
