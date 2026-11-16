"""Mic ownership + hold-to-talk-to-Amber + double-press recording on the real watch (no phone connected: nothing is sent anywhere).
Usage: python tools/mic_test.py [COMx]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import ember

port = sys.argv[1] if len(sys.argv) > 1 else ember.find_port([])
s = ember.open_watch(port)
res = []
AMBER, TUNER = 12, 2
PAUSE, SAVE, KEEP = (205, 234), (205, 322), (205, 407)


def check(ok, what, extra=''):
    res.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + what + (f'   [{str(extra)[:180]}]' if extra != '' else ''), flush=True)


def ask(line, prefix='CMD', timeout=6):
    s.write((line + '\n').encode())
    t0 = time.time()
    while time.time() - t0 < timeout:
        l = s.readline().decode(errors='replace').strip()
        if 'Guru Meditation' in l or 'rst:0x' in l or 'Backtrace' in l or 'abort()' in l: print('!! CRASH', l[:400]); res.append(False)
        if l.startswith(prefix): return l
    return ''


def wait(secs):
    t0 = time.time()
    while time.time() - t0 < secs: s.readline()


def mic():
    r = ask('micstate', 'CMD micstate')
    return dict(kv.split('=', 1) for kv in re.findall(r'(\w+=[^ ]*)', r))


def labels():
    return re.findall(r'\[([^\]]*)\]', ask('labels', 'CMD labels') or '')


for _ in range(10):
    if ask('pwr', 'CMD pwr', 2): break
# no phone may take part: the radio is off for the whole test (and back on at exit, even after a failure)
import atexit
r = ask('radio off', 'CMD radio', 5) or ''
if 'link_ready=0' not in r:
    print('ABORT: could not take the phone off the link:', r); sys.exit(2)
atexit.register(lambda: (s.write(b'radio on' + bytes([10])), time.sleep(0.3)))
ask('recstop', 'CMD recstop')
ask('face', 'CMD face', 5)
check(mic().get('busy') == '0', 'mic starts free')

# 1) an app is listening (Tuner): HOLDING the button closes it, opens Amber and listens; releasing sends
ask(f'launch {TUNER}', 'CMD launch', 6); wait(1.0)
m = mic()
check(m.get('who') == 'Tuner' and m.get('what') == 'listening', 'the watch knows the Tuner owns the mic', m)
ask('btn 8', 'CMD btn'); wait(1.2)
r = ask('btn 9', 'CMD btn'); wait(0.6)
lb = labels()
check(f'app={AMBER}' in r, 'hold opens Amber (from inside another app)', r)
check(any('Closed Tuner' in x for x in lb), 'toast says which app was closed so Amber can listen', [x for x in lb if 'Closed' in x])
check(any('Phone not connected' in x for x in lb), 'no phone: Amber says so instead of a confusing "mic in use"', [x for x in lb if 'Phone' in x])
check(mic().get('busy') == '0', 'mic released again (nothing left listening)')

# 2) double press = meeting recording, with an honest message when it can't start
ask('face', 'CMD face', 5)
free_ok = 'ok=1' in ask('recstart 0', 'CMD recstart'); ask('recstop', 'CMD recstop')
ask('btn 2', 'CMD btn'); wait(1.0)
if not free_ok:
    lb = labels()
    check(any('Storage full' in x for x in lb), 'storage full: double press says so (not "mic busy")', [x for x in lb if 'Storage' in x or 'Mic' in x])
    print('SKIP recording tests: watch storage is full (move recordings to the phone: Companion > Meeting notes)')
    ask('face', 'CMD face', 5)
    print(f'MIC TESTS DONE pass={sum(res)} fail={len(res) - sum(res)} (recording part skipped)')
    sys.exit(0 if all(res) else 1)
m = mic()
check(m.get('who') == 'Recorder' and m.get('rec') == '1', 'double press starts a meeting recording', m)
r = ask('taptest', 'CMD taptest', 4)
fr = int(re.search(r'frames=(\d+)', r).group(1)) if 'frames=' in r else 0
check('open=1' in r and fr > 12000, 'a listener can share the meeting mic (about 1 s of 16 kHz audio copied)', r)
ask('btn 8', 'CMD btn'); wait(1.2); ask('btn 9', 'CMD btn'); wait(0.6)
check(not any('Mic in use' in x for x in labels()), 'meeting recording: holding for Amber shows no "mic in use" sheet (it shares)')
m = mic()
check(m.get('rec') == '1' and m.get('paused') == '0' and m.get('who') == 'Recorder', 'the recording kept going', m)
ask('face', 'CMD face', 5)
ask('btn 2', 'CMD btn'); wait(1.5)
check(mic().get('rec') == '0', 'double press again stops and saves the recording')

# 3) a music recording would lose audio: the sheet says who/what and offers pause or save
check('ok=1' in ask('recstart 1', 'CMD recstart'), 'music recording started')
wait(1.0)
ask('btn 8', 'CMD btn'); wait(1.2); ask('btn 9', 'CMD btn')
lb = labels()
check(any('Mic in use' in x for x in lb) and any('Recorder: music recording' in x for x in lb), 'sheet: "Mic in use - Recorder: music recording"', lb[:6])
check(any('Pause, talk, resume' in x for x in lb) and any('Save & stop' in x for x in lb) and any('Keep it running' in x for x in lb), 'options: pause / save / keep')
ask(f'tap {KEEP[0]} {KEEP[1]}', 'CMD tap'); wait(0.6)
check(not any('Mic in use' in x for x in labels()) and mic().get('rec') == '1', 'Keep it running: sheet closes, recording continues')
ask('btn 8', 'CMD btn'); wait(1.2); ask('btn 9', 'CMD btn')
ask(f'tap {PAUSE[0]} {PAUSE[1]}', 'CMD tap'); wait(1.5)
m = mic()
check(m.get('rec') == '1' and m.get('paused') == '0' and m.get('who') == 'Recorder',
      'Pause, talk, resume (no phone): the recording is resumed, nothing lost', m)
ask('btn 8', 'CMD btn'); wait(1.2); ask('btn 9', 'CMD btn')
ask(f'tap {SAVE[0]} {SAVE[1]}', 'CMD tap'); wait(1.5)
m = mic()
check(m.get('rec') == '0' and m.get('busy') == '0', 'Save & stop: recording saved, mic free', m)
r = ask('recstop', 'CMD recstop')
check(re.search(r'G_\d{8}_\d{6}\.wav', r), 'the music recording was written to a file', r)

# 4) pause/resume keeps one file
check('ok=1' in ask('recstart 0', 'CMD recstart'), 'meeting recording for pause test')
wait(0.8)
check('ok=1' in ask('recpause 1', 'CMD recpause') and mic().get('busy') == '0', 'pause frees the mic')
check('ok=1' in ask('recpause 0', 'CMD recpause') and mic().get('who') == 'Recorder', 'resume takes it back')
ask('recstop', 'CMD recstop')
ask('face', 'CMD face', 5)
print(f'MIC TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}')
sys.exit(0 if all(res) else 1)
