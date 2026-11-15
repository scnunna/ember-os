"""End-to-end without a phone: this laptop plays the phone between the REAL watch (USB dev shell) and the REAL Life OS.
  Amber (typed) -> Life OS creates a time block -> /api/watch/today -> 'sched' to the watch -> the watch asks at the
  check-in -> tap On track -> 'ci' -> POST /api/watch/checkins -> ci_ack -> Amber's reply from /api/watch/inbox -> watch.
Use a TEST account's device token only.
Usage: LIFEOS_BASE=https://<host>.ts.net:<port> LIFEOS_TOKEN=lw_... python tools/lifeos_bridge_e2e.py [COMx]"""
import datetime, json, os, re, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(__file__))
import ember

BASE, TOKEN = os.environ['LIFEOS_BASE'].rstrip('/'), os.environ['LIFEOS_TOKEN']
port = sys.argv[1] if len(sys.argv) > 1 else ember.find_port([])
res, ci_lines = [], []


def check(ok, what, extra=''):
    res.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + what + (f'   [{str(extra)[:200]}]' if extra != '' else ''), flush=True)


def api(method, path, body=None):
    req = urllib.request.Request(BASE + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Authorization': 'Bearer ' + TOKEN, 'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=60) as r: return r.status, json.loads(r.read() or b'{}')
    except urllib.error.HTTPError as e: return e.code, {}


s = ember.open_watch(port)


def read_for(secs, stop=None):
    t0 = time.time()
    while time.time() - t0 < secs:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('SCHED ci'): ci_lines.append(l)
        if 'Guru Meditation' in l or 'rst:0x' in l: print('!! REBOOT/CRASH:', l); res.append(False)
        if stop and stop(l): return l
    return None


def ask(line, prefix='CMD', timeout=6):
    s.write((line + '\n').encode())
    return read_for(timeout, lambda l: l.startswith(prefix))


def labels():
    return re.findall(r'\[([^\]]*)\]', ask('labels', 'CMD labels') or '')


def wait_labels(text, secs=8):
    t0 = time.time()
    while time.time() - t0 < secs:
        lb = labels()
        if any(text in x for x in lb): return lb
        read_for(0.4)
    return labels()


tz = int(datetime.datetime.now().astimezone().utcoffset().total_seconds() // 60)
today = datetime.date.today()
for _ in range(10):
    if ask('pwr', 'CMD pwr', 2): break

# 1) Amber, typed (the phone would send the transcript of your voice the same way)
st, r = api('POST', '/api/watch/amber', {'text': 'Add a one hour time block called Bridge test today at 3 pm.', 'at': int(time.time())})
check(st == 200 and r.get('ok') and r.get('id'), 'Amber accepted the instruction', r)
msg_id, reply = r.get('id'), None
t0 = time.time()
while time.time() - t0 < 180 and not reply:
    time.sleep(5)
    st, ib = api('GET', '/api/watch/inbox')
    reply = next((i for i in ib.get('items', []) if i.get('reply_to') == msg_id), None)
check(reply is not None, f'Amber replied via the inbox in {time.time() - t0:.0f} s', reply and reply['text'])

# 2) the plan
st, plan = api('GET', f'/api/watch/today?date={today.isoformat()}&tz={tz}')
blk = next((b for b in plan.get('blocks', []) if 'bridge test' in b['title'].lower()), None)
check(st == 200 and blk and blk['s'] == 900 and blk['e'] == 960, 'the new block is in /api/watch/today (15:00-16:00)', blk)

# 3) to the watch, as the phone would
ask('schedreset', 'CMD schedreset')
sched = {'t': 'sched', 'date': int(today.strftime('%Y%m%d')), 'gen': plan.get('gen', ''), 'slots': plan.get('slots'), 'blocks': plan.get('blocks', [])}
ask('link ' + json.dumps(sched, separators=(',', ':')), 'CMD link'); read_for(2.5)
r = ask('sched', 'CMD sched') or ''
check(f"gen={plan.get('gen')}" in r, 'watch stored the Life OS plan', r[:120])
ask('citest 961', 'CMD citest')
lb = wait_labels('16:00 check-in')
check(any('Bridge test' in x for x in lb), 'the 16:00 check-in on the watch lists the block Amber created', [x for x in lb if 'check' in x or 'Planned' in x][:3])
ask('tap 205 251', 'CMD tap'); read_for(1.5)
ask('citest -1', 'CMD citest')

# 4) answers -> Life OS -> ack
items = []
for l in ci_lines:
    d = dict(kv.split('=', 1) for kv in l.split()[2:])
    items.append({'seq': int(d['seq']), 'date': int(d['date']), 'slot': int(d['slot']), 'ans': d['ans'], 'at': int(time.time()),
                  'ids': [blk['id']] if d['slot'] == '960' and blk else []})
check(any(i['slot'] == 960 and i['ans'] == 'on' for i in items), 'watch logged 16:00 = on track', items)
st, r = api('POST', '/api/watch/checkins', {'items': items})
check(st == 200 and r.get('stored') == len(items) and r.get('upto') == max(i['seq'] for i in items), 'Life OS stored every check-in', r)
st, r2 = api('POST', '/api/watch/checkins', {'items': items})
check(st == 200 and r2.get('stored') == 0, 're-sending stores nothing twice', r2)
ask('link ' + json.dumps({'t': 'ci_ack', 'upto': r.get('upto', 0)}), 'CMD link'); read_for(0.8)
check('ciq=0' in (ask('sched', 'CMD sched') or ''), 'watch queue empty after the ack')

# 5) Amber's reply -> watch, then ack the inbox
if reply:
    ask('link ' + json.dumps({'t': 'amber', 'id': reply['id'], 'text': reply['text'][:300], 'final': True}), 'CMD link'); read_for(1.5)
    ask('launch 13', 'CMD launch', 6); ask('launch 12', 'CMD launch', 6); read_for(0.8)
    check(any(reply['text'][:30] in x for x in labels()), "Amber's reply shows in the watch's Amber app")
    st, r = api('POST', '/api/watch/inbox/ack', {'ids': [reply['id']]})
    check(st == 200, 'inbox acknowledged', r)
ask('face', 'CMD face', 5)
ask('schedreset', 'CMD schedreset')
print(f'BRIDGE E2E DONE pass={sum(res)} fail={len(res) - sum(res)}')
sys.exit(0 if all(res) else 1)
