"""Amber check-ins + Amber messages on the real watch, over USB (Ember Link v1.4, docs/LIFE_MANAGER_PLAN.md).
Acts as the phone with the dev shell ('link <json>') and moves a fake clock with 'citest <minute>'.
Usage: python tools/amber_test.py [COMx]"""
import datetime, json, os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import ember

port = sys.argv[1] if len(sys.argv) > 1 else ember.find_port([])
s = ember.open_watch(port)
res, tx, ci_lines, SEQS = [], [], [], []
ON, OFF, SNOOZE, DONE = (205, 251), (120, 331), (290, 331), (205, 385)


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
        if l.startswith('SCHED ci'):
            ci_lines.append(l); SEQS.append(int(re.search(r'seq=(\d+)', l).group(1)))
        if 'Guru Meditation' in l or 'rst:0x' in l: print('!! REBOOT/CRASH:', l); res.append(False)
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


def wait_labels(text, secs=6):
    """poll the screen until a label containing text shows (prompts appear within ~3 s)"""
    t0 = time.time()
    while time.time() - t0 < secs:
        lb = labels()
        if any(text in x for x in lb): return lb
        read_for(0.4)
    return labels()


def link(obj, wait=0.8):
    ask('link ' + json.dumps(obj, separators=(',', ':')), 'CMD link')
    read_for(wait)


def tap(xy, wait=0.8):
    ask(f'tap {xy[0]} {xy[1]}', 'CMD tap'); read_for(wait)


def sched():
    r = ask('sched', 'CMD sched') or ''
    t0 = time.time(); extra = []
    while time.time() - t0 < 0.6:                       # the block lines follow the CMD line
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('SCHED blk'): extra.append(l)
    return r + ' | ' + ' | '.join(extra)


def gci(slot, pred=lambda d: True):
    """latest check-in answer the watch logged for this slot (answers only go over the radio when a phone is connected)"""
    for l in list(ci_lines):
        d = dict(kv.split('=', 1) for kv in l.split()[2:])
        d = {k: (int(v) if v.lstrip('-').isdigit() else v) for k, v in d.items()}
        if d['slot'] == slot and pred(d):
            ci_lines.remove(l); return d
    return None


for _ in range(10):
    if ask('pwr', 'CMD pwr', 2): break
# no phone may take part: the radio is off for the whole test (and back on at exit, even after a failure)
import atexit
r = ask('radio off', 'CMD radio', 5) or ''
if 'link_ready=0' not in r:
    print('ABORT: could not take the phone off the link:', r); sys.exit(2)
atexit.register(lambda: (s.write(b'radio on' + bytes([10])), time.sleep(0.3)))
ask('face', 'CMD face', 5)
ask('schedreset', 'CMD schedreset')
check('date=0' in sched(), 'starts with no plan')

today = int(datetime.date.today().strftime('%Y%m%d'))
tomorrow = int((datetime.date.today() + datetime.timedelta(days=1)).strftime('%Y%m%d'))
blocks = [
    {'id': 'b1-gym', 's': 420, 'e': 450, 'title': 'Gym'},
    {'id': 'b2-wff', 's': 540, 'e': 660, 'title': 'WFF deep work'},
    {'id': 'b3-lunch', 's': 690, 'e': 720, 'title': 'Lunch with Rao'},
    {'id': 'b4-mrs', 's': 780, 'e': 900, 'title': 'MRS marketing - a very long block title that is cut at 39 chars'},
    {'id': 'b5-gtr', 's': 1140, 'e': 1170, 'title': 'Guitar practice'},
]
ask('citest 450', 'CMD citest')                     # clock 07:30, before the first check-in
link({'t': 'sched', 'date': today, 'gen': 'g1', 'blocks': blocks}, 2.5)
m = got('sched_ok')
check(m and m['date'] == today and m['n'] == 5 and m['gen'] == 'g1', 'sched stored, sched_ok sent back', m)
r = sched()
check('slots=480:-,720:-,960:-,1200:-,1440:-' in r, 'default slots 08 12 16 20 + end of day', r)
check('SCHED blk 3 780-900 done=0 MRS marketing - a very long block titl' in r and 'cut at' not in r, 'title cut at 39 characters')
link({'t': 'hello_req'})
m = got('hello')
check(m and m.get('sd') == today and m.get('ciq') == 0, 'hello carries the plan date (sd) and queue size (ciq)', m)

m = re.search(r'next_s=(\d+)', sched())
check(m and int(m.group(1)) == 1800, 'next check-in timer = 30 min at 07:30 (deep sleep wakes for it, not every minute)', m and m.group(0))

# --- 08:00: first check-in of the day lists the day ahead
ask('citest 481', 'CMD citest')
lb = wait_labels('08:00 check-in')
check(any('08:00 check-in' in x for x in lb) and any('Today:' in x and 'WFF deep work' in x for x in lb), '08:00 check-in shows the day ahead', lb[:6])
tap(ON, 1.0)
m = gci(480)
check(m and m['ans'] == 'on' and m['date'] == today and m['ids'] == 4 and m['seq'] > 0, 'On track -> ci {slot 480, on, the 4 blocks still ahead}', m)
check(not any('check-in' in x for x in labels()), 'prompt closes after the answer')

# --- 12:00 slept through -> logged missed, not asked late; 16:00 asked
ask('citest 961', 'CMD citest')
lb = wait_labels('16:00 check-in')
m = gci(720)
check(m and m['ans'] == 'missed', 'a slot the watch slept through is logged as missed', m)
check(any('16:00 check-in' in x for x in lb) and any('Planned since 12:00' in x and 'MRS marketing' in x for x in lb), '16:00 check-in lists what was planned since 12:00', lb[:6])
tap(SNOOZE, 1.0)
m = gci(960)
check(m and m['ans'] == 'snooze', 'Snooze is logged', m)
ask('citest 970', 'CMD citest'); read_for(1.0)
check(not any('check-in' in x for x in labels()), 'not asked again before the snooze ends')
ask('citest 977', 'CMD citest')
check(any('16:00 check-in' in x for x in wait_labels('16:00 check-in')), 'asked again after 15 min snooze')
tap(OFF, 1.0)
lb = labels()
check(any('What did you do' in x for x in lb) and any('tell Amber' in x for x in lb), 'Off track -> "what did you do instead?" with Hold: tell Amber', lb[:6])
ask('hold 205 284 600', 'CMD hold', 5); read_for(0.5)
check(any('Phone not connected' in x for x in labels()), 'hold to talk without a phone says so (no crash)')
tap(DONE, 1.0)
m = gci(960, lambda d: d['ans'] != 'snooze')
check(m and m['ans'] == 'off', 'Done -> ci off track', m)
check('960:off' in sched(), 'the final answer replaces the snooze')

# --- queue + ack
r = sched()
q = int(re.search(r'ciq=(\d+)', r).group(1))
check(q == 4, 'all 4 answers are queued until the phone acks them', r)
last = max(SEQS)
link({'t': 'ci_ack', 'upto': last - 2})
check('ciq=2' in sched(), 'ci_ack drops answers up to that seq')
link({'t': 'ci_ack', 'upto': last})
check('ciq=0' in sched(), 'ci_ack of the last one empties the queue')

# --- end of day: 20:00 missed, 00:00 asked for the day that ended; tomorrow's plan arriving doesn't lose it
ask('citest 1300', 'CMD citest'); read_for(2.0)
m = gci(1200)
check(m and m['ans'] == 'missed', '20:00 slept through -> missed', m)
link({'t': 'sched', 'date': tomorrow, 'gen': 'g2', 'blocks': [{'id': 't1', 's': 600, 'e': 660, 'title': 'Tomorrow block'}]}, 2.5)
r = sched()
check(f'date={tomorrow}' in r and f'prev={today}' in r, "tomorrow's plan keeps today's for its end-of-day check-in", r)
ask('citest 1440', 'CMD citest'); read_for(2.0)       # = 00:00 of tomorrow
lb = labels()
check(any('End of day check-in' in x for x in lb), 'end of day check-in at 00:00', lb[:5])
tap(ON, 1.0)
m = gci(1440)
check(m and m['date'] == today and m['ans'] == 'on', 'end of day answer belongs to the day that ended', m)
link({'t': 'sched', 'date': today, 'gen': 'old', 'blocks': []}, 1.5)
check(got('err') is not None and f'date={tomorrow}' in sched(), 'an older plan is refused')

# --- Amber's morning summary inside the 08:00 check-in (tomorrow's plan, as the phone sends it at 07:55)
brief = 'Today: 1 block. First: Tomorrow block 10:00. Top task: send the WFF weekly review draft before lunch, then call the bank about the card.'
link({'t': 'sched', 'date': tomorrow, 'gen': 'g3', 'brief': brief + '\n' + 'x' * 120,
      'blocks': [{'id': 't1', 's': 600, 'e': 660, 'title': 'Tomorrow block'}]}, 2.5)
ask('citest 1921', 'CMD citest')                      # 08:01 tomorrow
lb = wait_labels('08:00 check-in')
check(any(brief[:60] in x for x in lb), "08:00 check-in shows Amber's morning summary", [x for x in lb if 'Today' in x][:2])
sm = [x for x in lb if 'Today: 1 block' in x]
check(sm and all('|' not in x and len(x.encode()) <= 205 for x in sm), 'summary is cut to 200 bytes, no line breaks smuggled in', sm)
tap(ON, 1.0)
m = gci(480, lambda d: d['date'] == tomorrow)
check(m and m['ans'] == 'on' and m['ids'] == 1, '08:00 answer still carries the day\'s blocks', m)

# --- Amber messages
ask('launch 12', 'CMD launch', 6); read_for(0.8)
lb = labels()
check(any('Hold to talk' in x for x in lb), 'Amber app opens with Hold to talk', lb[:4])
link({'t': 'amber_heard', 'text': 'move gym to 6 pm'}, 1.0)
link({'t': 'amber', 'id': 'x1', 'text': 'Got it.', 'final': False}, 1.0)
link({'t': 'amber', 'id': 'x2', 'text': 'Done: Gym moved to 18:00 today.', 'final': True}, 1.5)
lb = labels()
check(any('You: move gym to 6 pm' in x for x in lb) and any('Amber: Done: Gym moved to 18:00' in x for x in lb), 'Amber app shows what you said and the replies', [x for x in lb if 'You' in x or 'Amber:' in x][:4])

# --- Today app + persistence across a reboot
ask('citest -1', 'CMD citest')
ask('launch 13', 'CMD launch', 6); read_for(0.8)
lb = labels()
check(any('Today' in x for x in lb), 'Today app opens', lb[:4])
ask('face', 'CMD face', 5)
r1 = sched()
s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False     # hardware reset (same as esptool's)
s.close(); time.sleep(7)
s = ember.open_watch(port)
for _ in range(10):
    if ask('pwr', 'CMD pwr', 2): break
r2 = sched()
check(r1.split(' next_s')[0].split(' on=')[0] == r2.split(' next_s')[0].split(' on=')[0], 'plan + answers survive a reboot', (r1[:90], r2[:90]))
ask('schedreset', 'CMD schedreset')
print(f'AMBER TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}')
sys.exit(0 if all(res) else 1)
