"""Deletes recordings made today by the device tests (names M_/G_<today>_*), via the same verified rec_del the phone
uses. Run at the end of test sessions so test audio never fills the watch. Usage: python tools/clean_test_recs.py [COMx] [--all]"""
import datetime, json, os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import ember
port = next((a for a in sys.argv[1:] if not a.startswith('--')), None) or ember.find_port([])
everything = '--all' in sys.argv
s = ember.open_watch(port)
def recs():
    s.write(b'link {"t":"recs_req"}\n'); t0 = time.time()
    while time.time() - t0 < 4:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('LINK tx {') and '"files"' in l: return json.loads(l[8:])['files']
    return []
today = datetime.date.today().strftime('%Y%m%d')
n = 0
for _ in range(5):                      # the list holds the newest 40: repeat until none are left
    batch = [f for f in recs() if everything or f['n'][2:10] == today]
    if not batch: break
    for f in batch:
        s.write(('link ' + json.dumps({'t': 'rec_del', 'n': f['n'], 's': f['s']}) + '\n').encode()); time.sleep(0.4); n += 1
    time.sleep(1); s.reset_input_buffer()
s.write(b'du\n'); t0 = time.time()
while time.time() - t0 < 10:
    l = s.readline().decode(errors='replace').strip()
    if l.startswith('CMD du'): print(f'deleted {n} recordings; {l}'); break
