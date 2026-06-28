import sys
import threading
import time

sys.path.insert(0, r'C:\Espressif\tools\python\v6.0.1\venv\Lib\site-packages')
import serial

def capture(port, label, duration):
    try:
        ser = serial.Serial(port, 115200, timeout=1)
        out = open(f'cap_{label}.log', 'w', buffering=1)
        start = time.time()
        while time.time() - start < duration:
            try:
                line = ser.readline().decode('utf-8', errors='replace').rstrip('\r\n')
                if line:
                    ts = time.time() - start
                    out.write(f'[{ts:09.3f}] {line}\n')
            except Exception as e:
                out.write(f'[SERIAL ERROR] {e}\n')
    except Exception as e:
        with open(f'cap_{label}.log', 'w') as f:
            f.write(f'[PORT ERROR] {e}\n')
    finally:
        try: ser.close()
        except: pass

duration = int(sys.argv[1]) if len(sys.argv) > 1 else 180
t5 = threading.Thread(target=capture, args=('COM5', 'com5', duration), daemon=True)
t6 = threading.Thread(target=capture, args=('COM6', 'com6', duration), daemon=True)

t5.start()
t6.start()
t5.join()
t6.join()
