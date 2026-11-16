"""Wake policy (owner 2026-11-15): only the BOOT button (and the power click) lights the screen.
Touch, wrist motion, check-ins, Amber replies and phone notifications never do. Usage: python tools/wake_test.py [COMx]"""
import atexit, datetime, json, os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import ember
s = ember.open_watch(sys.argv[1] if len(sys.argv) > 1 else ember.find_port([]))
res = []
def check(ok, what, extra=''):
    res.append(bool(ok)); print(('PASS ' if ok else 'FAIL ') + what + (f'   [{extra}]' if extra != '' else ''), flush=True)
def ask(c, pre='CMD', t=6):
    s.write((c + '\n').encode()); t0 = time.time()
    while time.time() - t0 < t:
        l = s.readline().decode(errors='replace').strip()
        if 'Guru Meditation' in l or 'rst:0x' in l: res.append(False); print('!! CRASH', l)
        if l.startswith(pre): return l
    return ''
def screen():
    return 'screen=on' in (ask('pwr', 'CMD pwr') or '')
def wait(sec):
    t0 = time.time()
    while time.time() - t0 < sec: s.readline()
for _ in range(10):
    if ask('pwr', 'CMD pwr', 2): break
r = ask('radio off', 'CMD radio', 5)
if 'link_ready=0' not in r: print('ABORT: phone still linked'); sys.exit(2)
atexit.register(lambda: (s.write(b'radio on' + bytes([10])), time.sleep(0.3)))
r = ask('raisetest', 'CMD raisetest')
check('fails=0' in r, 'raise detector self-test still passes (it is just not used)', r)
ask('face', 'CMD face', 5)
check(screen(), 'screen on after face (the check itself works)')
ask('screen off', 'CMD screen'); wait(0.5)
check(not screen(), 'screen off')
ask('tap 205 250', 'CMD tap'); wait(0.8)
check(not screen(), 'a touch on a dark screen does nothing')
today = int(datetime.date.today().strftime('%Y%m%d'))
ask('schedreset', 'CMD schedreset')
ask('citest 470', 'CMD citest')
ask('link ' + json.dumps({'t': 'sched', 'date': today, 'gen': 'w', 'blocks': [{'id': 'x', 's': 540, 'e': 600, 'title': 'Wake test'}]}, separators=(',', ':')), 'CMD link'); wait(2)
ask('citest 481', 'CMD citest'); wait(3)
check(not screen(), 'a check-in prompt does NOT light the screen (it buzzes)')
r = ask('pwr', 'CMD pwr') or ''
check('screen=off' in r, 'still dark a few seconds later (nothing on screen is updated while dark)')
ask('link ' + json.dumps({'t': 'amber', 'id': 'w1', 'text': 'Wake test reply', 'final': True}), 'CMD link'); wait(1.5)
check(not screen(), 'an Amber reply does not light the screen')
ask('btn 1', 'CMD btn'); wait(1.6)
check(screen(), 'one BOOT press lights the screen')
lb = re.findall(r'\[([^\]]*)\]', ask('labels', 'CMD labels') or '')
check(any('08:00 check-in' in x for x in lb), 'the waiting check-in prompt is shown after the press', lb[:3])
ask('tap 205 251', 'CMD tap'); wait(1)
ask('citest -1', 'CMD citest'); ask('schedreset', 'CMD schedreset'); ask('face', 'CMD face', 5)
r = ask('pmu', 'CMD pmu')
print(f'WAKE TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}')
sys.exit(0 if all(res) else 1)
