"""Security checks (Ember Link v1.2, S1-S4) over USB. Usage: python tools/security_test.py [COMx]
The radio-side checks (unpaired phone refused) are in link_ble_test.py and need the laptop's Bluetooth."""
import json, os, re, sys, time, base64
sys.path.insert(0, os.path.dirname(__file__))
import ember

port = sys.argv[1] if len(sys.argv) > 1 else ember.find_port([])
s = ember.open_watch(port)
res, tx = [], []
DENY, ALLOW = (104, 407), (306, 407)


def check(ok, what, extra=''):
    res.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + what + (f'   [{str(extra)[:170]}]' if extra != '' else ''), flush=True)


def read_for(secs, stop=None):
    t0 = time.time()
    while time.time() - t0 < secs:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('LINK tx '):
            try: tx.append(json.loads(l[8:]))
            except Exception: pass
        if stop and stop(l): return l
    return None


def ask(line, prefix='CMD', timeout=6):
    s.write((line + '\n').encode())
    return read_for(timeout, lambda l: l.startswith(prefix))


def got(t, pred=lambda m: True):
    for m in tx:
        if m.get('t') == t and pred(m):
            tx.remove(m); return m
    return None


def labels():
    r = ask('labels', 'CMD labels')
    return re.findall(r'\[([^\]]*)\]', r or '')


def send(obj, untrusted=False, wait=0.8):
    ask(('linkb ' if untrusted else 'link ') + json.dumps(obj, separators=(',', ':')), 'CMD link')
    read_for(wait)


def upload(name, untrusted):
    src = b"function start() w.title('sec') end\n"
    send({'t': 'app_put', 'name': name, 'size': len(src)}, untrusted)
    send({'t': 'app_data', 'b64': base64.b64encode(src).decode()}, untrusted)
    send({'t': 'app_end'}, untrusted, 1.6)      # the Allow/Deny prompt appears within 1 s


def apps():
    r = ask('app ls', 'CMD app ls')
    return r or ''


for _ in range(10):
    if ask('pwr', 'CMD pwr', 2): break

r = ask('secstate', 'CMD secstate')
check(r and 'chronos=0' in r, 'Chronos (no-password protocol) is OFF by default', r)

# --- S3: install from the "radio" needs a tap on the watch
upload('sec_a', untrusted=True)
lb = labels()
check(any('Allow this?' in x for x in lb) and any('sec_a' in x for x in lb), 'risky request shows an Allow/Deny prompt on the watch', lb[:4])
ask(f'tap {DENY[0]} {DENY[1]}', 'CMD tap'); read_for(1.0)
m = got('err')
check(m and m.get('re') == 'app_end' and 'denied' in m.get('msg', ''), 'Deny -> install refused, phone told why', m)
check('sec_a' not in apps(), 'denied app is NOT installed')
upload('sec_b', untrusted=True)
ask(f'tap {ALLOW[0]} {ALLOW[1]}', 'CMD tap'); read_for(1.5)
check(got('app_ok') is not None and 'sec_b' in apps(), 'Allow -> install goes ahead')
send({'t': 'app_rm', 'name': 'sec_b'}, untrusted=True, wait=1.6)
ask(f'tap {DENY[0]} {DENY[1]}', 'CMD tap'); read_for(1.0)
check('sec_b' in apps(), 'remove request denied -> app kept')
got('err')                                       # consume that denial so the timeout check below sees its own
send({'t': 'app_rm', 'name': 'sec_b'}, untrusted=True, wait=1.6)
ask(f'tap {ALLOW[0]} {ALLOW[1]}', 'CMD tap'); read_for(1.5)
check('sec_b' not in apps(), 'remove request allowed -> app removed')

# --- S3: no answer = deny (30 s)
send({'t': 'wifi_req'}, untrusted=True, wait=1.6)
check(any('Wi-Fi hotspot' in x for x in labels()), 'hotspot request asks first')
read_for(31)
m = got('err')
check(m and m.get('re') == 'wifi_req', 'no answer in 30 s -> denied (default deny)', m)
check(not any('Allow this?' in x for x in labels()), 'the prompt disappears after the timeout')
check(got('wifi') is None, 'hotspot details were never sent')

# --- S4: fresh password + access key every session
creds = []
for i in range(2):
    send({'t': 'wifi_req'}, wait=6)
    m = got('wifi')
    creds.append(m)
    send({'t': 'wifi_off'}, wait=2)
ok = all(c and len(c.get('pass', '')) == 12 and re.fullmatch(r'[0-9a-f]{16}', c.get('token', '')) for c in creds)
check(ok, 'hotspot reply carries a 12-char password + 16-hex access key', [{k: v for k, v in (c or {}).items() if k not in ('pass', 'token')} for c in creds])
check(ok and creds[0]['pass'] != creds[1]['pass'] and creds[0]['token'] != creds[1]['token'], 'password and key are NEW every session')

# --- rec_del only with the right size
r = ask('rec 2', 'CMD rec', 10)
name = r.split('file=')[1].split()[0].rsplit('/', 1)[1] if r and 'file=' in r else ''
send({'t': 'recs_req'}, wait=1.0)
m = got('recs')
size = next((f['s'] for f in (m or {}).get('files', []) if f['n'] == name), None)
send({'t': 'rec_del', 'n': name, 's': (size or 0) + 1}, wait=1.0)
m = got('recs')
check(size and m and any(f['n'] == name for f in m['files']), 'rec_del with the wrong size keeps the recording', name)
send({'t': 'rec_del', 'n': name, 's': size}, wait=1.0)
m = got('recs')
check(m and not any(f['n'] == name for f in m['files']), 'rec_del with the verified size deletes it')
send({'t': 'rec_del', 'n': '../../apps/counter.lua', 's': 1}, wait=1.0)
check('counter' in apps(), 'rec_del cannot reach outside the recordings folder')

# --- S1: pairing window only by press-and-HOLD
ask('launch 7', 'CMD launch')
ask('tap 264 134', 'CMD tap'); ask('tap 130 454', 'CMD tap')
r = ask('secstate', 'CMD secstate')
check(r and 'pairing_open=0' in r, 'a short tap on "Hold: add device" does NOT open pairing', r)
ask('tap 264 134', 'CMD tap'); ask('hold 130 454 1200', 'CMD hold', 5)
r = ask('secstate', 'CMD secstate')
check(r and 'pairing_open=1' in r, 'press-and-hold opens the 2-minute pairing window', r)
ask('face', 'CMD face', 5)
print(f'SECURITY TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}')
sys.exit(0 if all(res) else 1)
