"""Sets the watch clock from this PC's clock over USB serial. Usage: python tools/settime.py [COMx]"""
import sys, time, datetime, serial
port = sys.argv[1] if len(sys.argv) > 1 else 'COM9'
s = serial.Serial(port, 115200, timeout=1)
s.dtr = False; s.rts = False
time.sleep(0.3)
n = datetime.datetime.now()
s.write(f"settime {n.year} {n.month} {n.day} {n.hour} {n.minute} {n.second}\n".encode())
t0 = time.time()
while time.time() - t0 < 3:
    l = s.readline().decode(errors='replace').strip()
    if l.startswith('CMD'):
        print(l); break
else:
    print('no reply from watch (is it running the new firmware?)')
