"""On-device tests for Ember OS (scripted apps + power manager). Needs the watch on USB.
   python tools/ember_test.py [COMx]      prints PASS/FAIL lines and a summary."""
import sys, time, os, tempfile, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import ember

argv = sys.argv[1:]
port = argv[0] if argv else ember.find_port([])
s = ember.open_watch(port)
res = []


def check(ok, what, extra=''):
    res.append(ok)
    print(('PASS ' if ok else 'FAIL ') + what + (f'   [{extra}]' if extra else ''))


def push(name, src):
    data = src.encode()
    r = ember.ask(s, f'app put {name}', 'CMD app put')
    assert r and 'ok' in r, r
    for i in range(0, len(data), 256):
        r = ember.ask(s, 'app hex ' + data[i:i + 256].hex(), 'CMD app hex')
        assert r and ' ok ' in r + ' ', r
    return ember.ask(s, 'app end', 'CMD app end', 8)


def collect(secs):
    out, t0 = [], time.time()
    while time.time() - t0 < secs:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('EMBER'):
            out.append(l)
    return out


def run(name, secs=2.0):
    early = []
    r = ember.ask(s, f'app run {name}', 'CMD app run', sink=early)   # start() output arrives before the reply
    return r, early + collect(secs)


# 1. sample apps installed on first boot
s.reset_input_buffer()
r = ember.ask(s, 'app ls', 'CMD app ls')
check(r is not None and all(n in r for n in ('counter', 'dice', 'level')), 'sample apps counter/dice/level installed', r)

# 1b. regression 2026-11-14: sound/mic apps crashed the watch (no internal DMA RAM left for the audio driver)
r = ember.ask(s, 'mem', 'CMD mem')
big = int(r.split('dma_largest=')[1].split()[0]) if r else 0
check(big >= 8192, 'internal DMA RAM headroom after full boot (>= 8 KB in one piece)', r)
r = ember.ask(s, 'beep', 'CMD beep', 6)
check(r is not None and 'speaker=ok' in r, 'speaker works after Bluetooth + display are up (Metronome/Timer/Dice path)', r)
r = ember.ask(s, 'rec 2', 'CMD rec', 10)
check(r is not None and 'start=ok' in r, 'microphone recording works (Recorder/Tuner/Sound path)', r)
for i, name in enumerate(['Steps', 'Recorder', 'Tuner', 'Metronome', 'Timer', 'Sound', 'Remote', 'Phone', 'Settings', 'Notes', 'Translate', 'Practice']):
    r = ember.ask(s, f'open {i}', 'CMD open', 8)
    check(r is not None, f'built-in app {name} opens and closes without a crash', r)
r = ember.ask(s, 'beep', 'CMD beep', 6)
check(r is not None and 'speaker=ok' in r, 'speaker still works after opening every app', r)

# 2. API smoke test + key/value persistence across launches
API = """
local runs = w.load('runs', 0) + 1
w.save('runs', runs)
w.save('word', 'ember')
function start()
  w.title('API test')
  local h, m = w.time()
  local pct, mv, chg, usb = w.battery()
  local x, y, z = w.imu()
  local b = w.bar(nil, 300, 300, 20, 100); w.value(b, 50)
  local c = w.circle(205, 200, 40, 0xFF0000); w.move(c, 100, 100); w.color(c, 0x00FF00); w.show(c, false)
  local l = w.label('hi', nil, 400, 28); w.text(l, 'ok')
  print(string.format('API runs=%d word=%s time=%s steps=%d usb=%s g=%.2f', runs, w.load('word', '?'), tostring(h ~= nil), w.steps(), tostring(usb), math.sqrt(x*x+y*y+z*z)))
end
"""
push('t_api', API)
r, out = run('t_api')
check(r is not None and 'state=2' in r, 'scripted app opens (launcher state = app)', r)
line = next((l for l in out if 'API runs=' in l), '')
check('word=ember' in line and 'time=true' in line, 'API calls work (time, battery, imu, widgets, kv)', line)
g = float(line.split('g=')[1]) if 'g=' in line else 0
check(0.8 < g < 1.2, 'motion sensor reads ~1 g at rest through the app API', f'g={g}')
r, out = run('t_api', 1.5)                          # 2nd launch
line2 = next((l for l in out if 'API runs=' in l), '')
check('runs=2' in line2, 'app data persists between launches', line2)

# 3. runaway loop is stopped by the power limit, watch stays responsive
push('t_loop', "function start() while true do end end")
r, out = run('t_loop', 2.5)
check(any('power limit' in l for l in out), 'infinite loop aborted after 250 ms (power limit)', out[-1] if out else 'no output')
r = ember.ask(s, 'pwr', 'CMD pwr')
check(r is not None, 'watch still responsive after runaway app', r)

# 4. syntax error is reported, not a crash
push('t_bad', "function start( w.title('x') end")
r, out = run('t_bad', 1.5)
check(any('ERROR' in l for l in out), 'syntax error shown instead of crashing', out[-1] if out else 'none')

# 5. memory cap
push('t_mem', "function start() local t = {} for i = 1, 1e6 do t[i] = string.rep('x', 100) .. i end end")
r, out = run('t_mem', 3)
check(any('memory' in l or 'power limit' in l for l in out), 'memory hog is stopped (256 KB cap / CPU limit)', out[-1] if out else 'none')

# 6. timers run while lit and stop while dark (power rule)
push('t_tmr', "local n = 0\nfunction start() w.every(100, function() n = n + 1; print('tick', n) end) end")
ember.ask(s, 'screen on', 'CMD screen')
r, on1 = run('t_tmr', 2.0)
ember.ask(s, 'screen off', 'CMD screen')
time.sleep(0.3); s.reset_input_buffer()
dark = collect(2.0)
ember.ask(s, 'screen on', 'CMD screen')
on2 = collect(2.0)
check(len(on1) >= 12, 'w.every(100) ticks while the screen is on', f'{len(on1)} ticks in 2 s')
check(len(dark) == 0, 'app timers are paused while the screen is dark', f'{len(dark)} ticks in 2 s dark')
check(len(on2) >= 12, 'timers resume when the screen wakes', f'{len(on2)} ticks')

# 7. power manager bookkeeping
r = ember.ask(s, 'pwr', 'CMD pwr')
check(r is not None and 'wakes=' in r and int(r.split('wakes=')[1].split()[0]) >= 1, 'power manager counts screen wakes', r)
check(r is not None and 'sleep_allowed=0' in r, 'no light sleep while on USB power (keeps the USB link alive)', r)

# 8. cleanup
# regression 2026-11-14: rapid install/remove while the screen slides used to hang LVGL's layout (task watchdog)
stress_ok = 0
for trial in range(3):
    for n in ('r1', 'r2', 'r3', 'r4'): push(n, f"function start() w.title('{n}') end")
    ember.ask(s, 'app run r4', 'CMD app run', 5, sink=[])
    rs = [ember.ask(s, f'app rm {n}', 'CMD app rm', 6) for n in ('r1', 'r2', 'r3', 'r4')]
    stress_ok += all(r and r.endswith('ok') for r in rs) and ember.ask(s, 'pwr', 'CMD pwr', 4) is not None
check(stress_ok == 3, 'rapid app install/remove during screen animations never hangs the UI', f'{stress_ok}/3 rounds')

rm_replies = {n: ember.ask(s, f'app rm {n}', 'CMD app rm', 6) for n in ('t_api', 't_loop', 't_bad', 't_mem', 't_tmr')}
r = ember.ask(s, 'app ls', 'CMD app ls')
check(r is not None and 't_' not in r, 'test apps removed', f'{r}  rm replies: {rm_replies}')
ember.ask(s, 'screen on', 'CMD screen')

print(f'EMBER TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}')
sys.exit(0 if all(res) else 1)
