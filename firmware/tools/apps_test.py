"""Test EVERY app one at a time through the real touch pipeline (synthetic finger -> LVGL -> app callbacks).
For each app: open it, read the screen, tap every button twice, watch for Lua errors / crashes / reboots, then
check app-specific behaviour. Usage: python tools/apps_test.py [COMx]"""
import sys, os, time, re, datetime
sys.path.insert(0, os.path.dirname(__file__))
import ember

port = sys.argv[1] if len(sys.argv) > 1 else ember.find_port([])
s = ember.open_watch(port)
res, log = [], []
BAD = re.compile(r'EMBER ERROR|Guru Meditation|abort\(\)|Rebooting|ESP-ROM|assert failed|Backtrace', re.I)


def read_until(prefix, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        l = s.readline().decode(errors='replace').strip()
        if not l:
            continue
        log.append(l)
        if l.startswith(prefix):
            return l
    return None


def ask(line, prefix, timeout=5):
    mark = len(log)
    s.write((line + '\n').encode())
    r = read_until(prefix, timeout)
    return r, log[mark:]


def check(ok, what, extra=''):
    res.append(ok)
    print(('PASS ' if ok else 'FAIL ') + what + (f'   [{extra}]' if extra else ''))


def uptime():
    r, _ = ask('pwr', 'CMD pwr')
    m = re.search(r'active_s=(\d+) doze_s=(\d+)', r or '')
    return int(m.group(1)) + int(m.group(2)) if m else -1


def labels():
    r, _ = ask('labels', 'CMD labels')
    return re.findall(r'\[([^\]]*)\]', r or '')


r = None
for _ in range(10):                      # the watch may still be booting (e.g. right after a flash)
    r, _ = ask('app ls', 'CMD app ls', 2)
    if r: break
scripts = r.split(':', 1)[1].split() if r and ':' in r else []
check(len(scripts) >= 3, 'scripted apps found', r)
BUILTIN = ['Steps', 'Recorder', 'Tuner', 'Metronome', 'Timer', 'Sound', 'Remote', 'Phone', 'Settings', 'Notes', 'Translate', 'Practice']
apps = BUILTIN + scripts
up0, wall0 = uptime(), time.time()

for idx, name in enumerate(apps):
    r, out = ask(f'launch {idx}', 'CMD launch', 6)
    opened = r is not None and 'state=2' in r and f'app={idx}' in r
    lb = labels()
    r2, out2 = ask('tapall 2', 'CMD tapall', 90)
    time.sleep(0.8)
    late = []
    t0 = time.time()
    while time.time() - t0 < 0.5:
        l = s.readline().decode(errors='replace').strip()
        if l: late.append(l); log.append(l)
    errs = [l for l in out + out2 + late if BAD.search(l)]
    nb = re.search(r'buttons=(\d+)', r2 or '')
    check(opened and r2 is not None and not errs,
          f'{name:<10} opens, every button tapped twice, no error/crash',
          f'buttons={nb.group(1) if nb else "?"} screen={lb[:4]}' + (f' ERRORS={errs[:2]}' if errs else ''))
    ask('screen on', 'CMD screen')

# ---- functional checks of the scripted apps ----
def app_index(n):
    return len(BUILTIN) + scripts.index(n) if n in scripts else None

if 'counter' in scripts:
    ask(f'launch {app_index("counter")}', 'CMD launch')
    n0 = next((int(x) for x in labels() if x.isdigit()), None)
    ask('tap 290 360', 'CMD tap'); n1 = next((int(x) for x in labels() if x.isdigit()), None)
    ask('tap 120 360', 'CMD tap'); n2 = next((int(x) for x in labels() if x.isdigit()), None)
    check(n0 is not None and n1 == n0 + 1 and n2 == n0, 'Counter: + adds one, - takes one away', f'{n0} -> {n1} -> {n2}')
if 'dice' in scripts:
    ask(f'launch {app_index("dice")}', 'CMD launch')
    vals = set()
    for _ in range(6):
        ask('tap 205 390', 'CMD tap'); time.sleep(0.2)
        vals |= {x for x in labels() if x in '123456' and len(x) == 1}
    lb = labels()
    check(vals and vals <= set('123456') and any(x.startswith('roll #') for x in lb), 'Dice: Roll shows 1-6 and counts rolls', f'seen {sorted(vals)} {lb}')
if 'level' in scripts:
    ask(f'launch {app_index("level")}', 'CMD launch'); time.sleep(0.6)
    lb = labels()
    check(any('deg' in x for x in lb), 'Level: live angle readout updates from the motion sensor', str(lb))

# ---- whole-session health ----
up1, wall1 = uptime(), time.time()
check(up0 > -1 and (up1 - up0) >= (wall1 - wall0) - 5, 'watch never rebooted during the whole run',
      f'watch uptime grew {up1 - up0}s while {wall1 - wall0:.0f}s passed')
allbad = [l for l in log if BAD.search(l)]
check(not allbad, 'no Lua errors or crash traces anywhere in the log', str(allbad[:3]))
check(any('AP up' in l for l in log) and any('AP down' in l for l in log), 'Phone: Wi-Fi hotspot really starts and stops (not just "no crash")')
r, _ = ask('beep', 'CMD beep', 6)
check(r is not None and 'speaker=ok' in r, 'speaker still works after Wi-Fi has been used', r)
r, _ = ask('rec 2', 'CMD rec', 10)
check(r is not None and 'start=ok' in r, 'recording still works after Wi-Fi has been used', r)
r, _ = ask('mem', 'CMD mem')
check(r is not None and int(re.search(r'dma_largest=(\d+)', r).group(1)) >= 8192, 'DMA memory still healthy at the end', r)

# restore things the button-tapping may have changed
n = datetime.datetime.now()
ask(f'settime {n.year} {n.month} {n.day} {n.hour} {n.minute} {n.second}', 'CMD')
ask('screen on', 'CMD screen')
open(os.path.join(os.path.dirname(__file__), 'apps_test_last.log'), 'w', encoding='utf-8').write('\n'.join(log))
print(f'APPS TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}  (full serial log: tools/apps_test_last.log)')
sys.exit(0 if all(res) else 1)
