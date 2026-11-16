"""End-to-end device test over USB serial: status, layout overlap check on every screen, navigation stress.
Usage: python tools/e2e.py [COMx] [stress_steps]"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM9'
steps = int(sys.argv[2]) if len(sys.argv) > 2 else 150
s = serial.Serial(port, 115200, timeout=1)
s.dtr = False; s.rts = False
time.sleep(4.5)            # opening the port resets the board; let it finish booting
errors = []
boot = s.read(s.in_waiting).decode(errors='replace')
import os as _os; sys.path.insert(0, _os.path.dirname(__file__)); import ember
ember.radio_off(s)            # taps every button: the owner's phone must not be on the link
for l in boot.splitlines():
    if l.startswith(('E (','W (')) and 'sdmmc' not in l and 'vfs_fat' not in l and 'i2c.master' not in l: errors.append(l[:160])


def run(cmd, done_prefix, timeout):
    s.write((cmd + "\n").encode())
    t0 = time.time(); out = []
    while time.time() - t0 < timeout:
        l = s.readline().decode(errors='replace').rstrip()
        if not l:
            continue
        if l.startswith(('E (', 'W (')) and 'sdmmc' not in l and 'vfs_fat' not in l and 'i2c.master' not in l and 'i2s_common' not in l:
            errors.append(l[:160])
        if l.startswith(('LAYOUT', 'CMD', 'NAV', 'TOUCH')):
            out.append(l)
            print(l[:200])
            if l.startswith(done_prefix):
                return out
    print('!! timeout waiting for', done_prefix)
    return out


run('status', 'CMD status', 5)
lay = run('layout', 'LAYOUT DONE', 90)
touch = run('touch', 'TOUCH DONE', 120)
nav = run(f"nav {steps}", "NAV DONE", steps * 0.6 + 60)
run('status', 'CMD status', 5)
print('\n=== SUMMARY ===')
probs = [l for l in lay if 'OVERLAP' in l or 'OFFSCREEN' in l]
print('layout problems:', len(probs))
print('touch tests failed:', len([l for l in touch if l.startswith('TOUCH FAIL')]), 'passed:', len([l for l in touch if l.startswith('TOUCH PASS')]))
print('stress finished:', any(l.startswith('NAV DONE') for l in nav))
import re
m = next((re.search(r'heap (\d+)->(\d+)', l) for l in nav if l.startswith('NAV DONE')), None)
if m:
    lost = int(m.group(1)) - int(m.group(2))
    # a leaked app screen is several KB; small changes are LVGL's allocator settling
    print(('PASS' if lost < 2048 else 'FAIL') + f' no memory leak during navigation stress (heap lost {lost} bytes, limit 2048)')
disp = [e for e in errors if 'spi_master' in e or 'lcd_panel.io.spi' in e or 'sh8601' in e]
print('DISPLAY FLUSH ERRORS:', len(disp), '(must be 0)')
print('other error/warn log lines:', len(errors) - len(disp))
for e in errors[:15]:
    print('  ', e)
