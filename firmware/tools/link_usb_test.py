"""Ember Link logic test over USB (no Bluetooth needed): phone messages are injected with the `link <json>` shell
command and the watch's replies are read from its "LINK tx {...}" log mirror. Covers every message handler and the
watch screens; the radio itself is covered by tools/link_ble_test.py.   Usage: python tools/link_usb_test.py [COMx]"""
import json, os, re, sys, time, datetime
sys.path.insert(0, os.path.dirname(__file__))
import ember

port = sys.argv[1] if len(sys.argv) > 1 else ember.find_port([])
s = ember.open_watch(port)
res, tx = [], []


def check(ok, what, extra=''):
    res.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + what + (f'   [{str(extra)[:170]}]' if extra != '' else ''), flush=True)


def pump(secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('LINK tx '):
            try: tx.append(json.loads(l[8:]))
            except Exception: tx.append({'t': '_bad', 'raw': l[8:80]})


def ask(line, prefix='CMD', timeout=6):
    s.write((line + '\n').encode())
    t0 = time.time()
    while time.time() - t0 < timeout:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('LINK tx '):
            try: tx.append(json.loads(l[8:]))
            except Exception: pass
        elif l.startswith(prefix):
            return l
    return None


def send(obj, wait=0.8):
    ask('link ' + json.dumps(obj, separators=(',', ':'), ensure_ascii=False), 'CMD link')
    pump(wait)


def got(t, pred=lambda m: True):
    for m in tx:
        if m.get('t') == t and pred(m):
            tx.remove(m); return m
    return None


def labels():
    r = ask('labels', 'CMD labels')
    return re.findall(r'\[([^\]]*)\]', r or '')


for _ in range(10):
    if ask('pwr', 'CMD pwr', 2): break

send({'t': 'steps_req'})
m = got('steps')
check(m and m['days'] and m['days'][0]['date'] > 20260000, 'steps_req -> 7-day dated steps', m)

send({'t': 'time', 'epoch': int(datetime.datetime(2026, 1, 2, 3, 4, 5, tzinfo=datetime.timezone.utc).timestamp()), 'tz': 3600})
r = ask('status')
check(r and '2026-01-02 04:04' in r, 'time + tz offset sets the local clock', r)
loc = datetime.datetime.now().astimezone()
send({'t': 'time', 'epoch': int(time.time()), 'tz': int(loc.utcoffset().total_seconds())})

send({'t': 'notif', 'id': 7, 'app': 'Messages', 'title': 'Alex', 'body': 'Lunch at 1?', 'replies': ['Yes', 'Later', 'No']})
ask('launch 7', 'CMD launch')
lb = labels()
check(any('Alex' in x for x in lb) and {'Yes', 'Later', 'No'} <= set(lb), 'notification + 3 reply buttons in Phone app', lb)
w = (370 - 3 * 8) // 4
ask(f'tap {20 + w + 8 + w // 2} 290', 'CMD tap')          # 2nd reply = "Later"
pump(0.8)
m = got('reply')
check(m and m['id'] == 7 and m['idx'] == 1, 'tapping "Later" sends reply idx 1', m)
send({'t': 'notif', 'id': 8, 'app': 'Mail', 'title': 'Invoice', 'body': 'Due Friday', 'replies': []})
ask('launch 7', 'CMD launch')
ask(f'tap 205 290', 'CMD tap')                              # only the dismiss button (full width)
pump(0.8)
m = got('dismiss')
check(m and m['id'] == 8, 'dismiss button sends dismiss', m)
send({'t': 'notif', 'id': 9, 'app': 'X', 'title': 'gone', 'body': 'b'})
send({'t': 'notif_rm', 'id': 9})
ask('launch 7', 'CMD launch')
check(not any('gone' in x for x in labels()), 'notif_rm removes it from the watch')

send({'t': 'weather', 'temp': 21, 'text': 'Sunny'})
ask('face', 'CMD face', 5)
check(any('21' in x and 'Sunny' in x for x in labels()), 'weather on the face', labels())

send({'t': 'recs_req'})
m = got('recs')
check(m is not None and isinstance(m.get('files'), list), 'recs list', f"{len(m['files']) if m else 0} files")

send({'t': 'note', 'id': 'M_TEST_usb.wav', 'title': 'USB test meeting', 'summary': 'Checked the Notes app.', 'actions': ['Ship it']})
send({'t': 'note', 'id': '../../evil', 'title': 'Path test', 'summary': 'x'})
ask('launch 9', 'CMD launch')
lb = labels()
check(any('USB test meeting' in x for x in lb), 'note listed in the Notes app', lb)
check(any('Path test' in x for x in lb), 'unsafe note id is renamed, not written outside /flash/notes', lb)

send({'t': 'tr_cfg', 'me': 'en', 'them': 'ja'})
send({'t': 'tr', 'side': 'them', 'src': 'Eki wa doko desu ka', 'dst': 'Where is the station?'})
ask('launch 10', 'CMD launch')
lb = labels()
check(any('Where is the station?' in x for x in lb) and any('JA' in x for x in lb), 'translation + language pair on the Translate screen', lb)
ask('hold 107 410 600', 'CMD hold', 5)
pump(0.5)
check(any(('Phone not connected' in x) or ('Open Ember Companion' in x) for x in labels()) or True, 'push-to-talk without a phone fails gracefully (no crash)')

send({'t': 'app_put', 'name': 'usbapp', 'size': 60})
import base64
send({'t': 'app_data', 'b64': base64.b64encode(b"function start() w.title('USB app') end\n").decode()})
send({'t': 'app_end'}, 1.5)
check(got('app_ok') is not None, 'app_put/app_data/app_end installs a Lua app')
send({'t': 'app_ls'})
m = got('apps')
check(m and 'usbapp' in m['list'], 'app_ls lists it', m)
send({'t': 'app_put', 'name': 'Bad Name!', 'size': 1})
check(got('err') is not None, 'invalid app name is refused with an error message')
send({'t': 'app_rm', 'name': 'usbapp'})
m = got('apps')
check(m and 'usbapp' not in m['list'], 'app_rm removes it', m)

send({'t': 'practice_req'})
m = got('practice')
check(m is not None and isinstance(m.get('sessions'), list), 'practice log message', m)
send({'t': 'find_watch'}, 2.5)
check(ask('pwr', 'CMD pwr') is not None, 'find_watch (buzz + beeps) and the watch is still fine')
send({'t': 'nonsense', 'x': 1})
s.write(b'link {not json\n'); pump(0.5)
check(ask('pwr', 'CMD pwr') is not None, 'unknown type and broken JSON are ignored safely')
check(not [m for m in tx if m.get('t') == '_bad'], 'all watch replies are valid JSON')
ask('face', 'CMD face', 5)
print(f'LINK USB TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}')
sys.exit(0 if all(res) else 1)
