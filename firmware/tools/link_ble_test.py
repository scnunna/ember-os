"""End-to-end Ember Link test over REAL Bluetooth: this PC plays the phone (Ember Companion) and the USB cable is used
to look at / poke the watch screen. Needs: laptop Bluetooth on, watch on USB, `pip install bleak pyserial`.

  python tools/link_ble_test.py            -> PASS/FAIL lines + summary
"""
import asyncio, json, os, re, sys, time, base64, math, datetime
sys.path.insert(0, os.path.dirname(__file__))
import ember, ember_adpcm
from bleak import BleakScanner, BleakClient

BASE = "-6d62-4f53-8a1e-656d6265720a"
SVC, CMD, EVT, AUD = (f"eb3f000{n}{BASE}" for n in (1, 2, 3, 4))
res = []


def check(ok, what, extra=''):
    res.append(bool(ok))
    print(('PASS ' if ok else 'FAIL ') + what + (f'   [{str(extra)[:160]}]' if extra != '' else ''), flush=True)


class Phone:
    def __init__(self, client):
        self.c, self.buf, self.msgs, self.audio = client, None, [], []

    def on_evt(self, _, data: bytearray):
        f, p = data[0], bytes(data[1:])
        if f & 1: self.buf = b''
        if self.buf is None: return
        self.buf += p
        if f & 2:
            try: self.msgs.append(json.loads(self.buf.decode()))
            except Exception as e: self.msgs.append({'t': '_bad', 'err': str(e), 'raw': self.buf[:80]})
            self.buf = None

    def on_audio(self, _, data: bytearray):
        self.audio.append(bytes(data))

    async def send(self, obj):
        b = json.dumps(obj, separators=(',', ':')).encode()
        room = max(20, self.c.mtu_size - 3) - 1
        parts = [b[i:i + room] for i in range(0, len(b), room)] or [b'']
        for i, p in enumerate(parts):
            f = (1 if i == 0 else 0) | (2 if i == len(parts) - 1 else 0)
            await self.c.write_gatt_char(CMD, bytes([f]) + p, response=True)

    async def wait(self, t, timeout=5.0, pred=lambda m: True):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.msgs:
                if m.get('t') == t and pred(m):
                    self.msgs.remove(m); return m
            await asyncio.sleep(0.05)
        return None


def usb(s, line, prefix='CMD', timeout=6):
    return ember.ask(s, line, prefix, timeout, sink=[])


def labels(s):
    r = usb(s, 'labels', 'CMD labels')
    return re.findall(r'\[([^\]]*)\]', r or '')


ALLOW = (306, 407)


def is_paired(st):
    from winrt.windows.devices.enumeration import DevicePairingResultStatus as R
    return st in (R.PAIRED, R.ALREADY_PAIRED)


async def winrt_dev(address):
    from winrt.windows.devices.bluetooth import BluetoothLEDevice
    return await BluetoothLEDevice.from_bluetooth_address_async(int(address.replace(':', ''), 16))


async def unpair(address):
    d = await winrt_dev(address)
    if d and d.device_information.pairing.is_paired:
        await d.device_information.pairing.unpair_async()


async def pair_with_watch_code(s, address):
    """Pair through Windows with Passkey Entry; the 6-digit code is read off the watch screen over USB."""
    from winrt.windows.devices.enumeration import DevicePairingKinds, DevicePairingProtectionLevel
    d = await winrt_dev(address)
    cp = d.device_information.pairing.custom
    seen = {}

    def on_request(sender, args):
        if args.pairing_kind != DevicePairingKinds.PROVIDE_PIN:
            return
        deferral = args.get_deferral()
        code = None
        for _ in range(12):                      # the code appears on the watch within ~1 s
            time.sleep(0.5)
            m = re.search(r'\[(\d{3}) (\d{3})\]', usb(s, 'labels', 'CMD labels') or '')
            if m: code = m.group(1) + m.group(2); break
        seen['code'] = code
        if code: args.accept_with_pin(code)
        deferral.complete()

    token = cp.add_pairing_requested(on_request)
    r = await cp.pair_with_protection_level_async(DevicePairingKinds.PROVIDE_PIN, DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION)
    cp.remove_pairing_requested(token)
    return r.status, seen.get('code')


async def attack_unpaired(dev, what):
    """Behave like a stranger: connect, try to subscribe + send a command. Must get nothing."""
    ph_msgs, err = [], None
    try:
        async with BleakClient(dev, timeout=15, winrt=dict(use_cached_services=False)) as c:
            ph = Phone(c)
            try:
                await c.start_notify(EVT, ph.on_evt)
                await ph.send({'t': 'find_watch'})
                await ph.send({'t': 'steps_req'})
            except Exception as e:
                err = type(e).__name__
            await asyncio.sleep(3)
            ph_msgs = ph.msgs
    except Exception as e:
        err = err or type(e).__name__
    return ph_msgs, err


async def main():
    s = ember.open_watch(ember.find_port([]))
    dev = await BleakScanner.find_device_by_filter(lambda d, ad: (d.name or '').startswith(('EmberWatchOS', 'Watch-')), timeout=15)
    check(dev is not None, 'watch found by Bluetooth scan (name EmberWatchOS)', dev.name if dev else 'not found')
    if not dev: return
    usb(s, 'face', 'CMD face', 5)

    # ---- 1. a stranger (not paired, pairing window closed) gets nothing
    await unpair(dev.address)
    for _ in range(30):                          # make sure no pairing window is left open from an earlier run
        r = usb(s, 'secstate', 'CMD secstate')
        if r and 'pairing_open=0' in r: break
        time.sleep(5)
    bonded_before = 'bonded=1' in (r or '')
    msgs, err = await attack_unpaired(dev, 'stranger')
    check(not msgs, 'UNPAIRED device: no data at all from the watch', f'got {len(msgs)} messages, error={err}')
    st, _ = await pair_with_watch_code(s, dev.address)
    r = usb(s, 'secstate', 'CMD secstate')
    check(not is_paired(st) and r and 'pairing_open=0' in r and (('bonded=1' in r) == bonded_before),
          'pairing attempt OUTSIDE the pairing window is refused (watch keeps its bonds unchanged)', f'{st} | {r}')
    await unpair(dev.address)

    # ---- 2. owner opens the pairing window (press-and-hold on the watch) and pairs with the shown code
    usb(s, 'launch 7', 'CMD launch')
    usb(s, 'tap 264 134', 'CMD tap'); usb(s, 'hold 130 454 1200', 'CMD hold', 5)
    r = usb(s, 'secstate', 'CMD secstate')
    check(r and 'pairing_open=1' in r, 'press-and-hold on the watch opens the pairing window', r)
    st, code = await pair_with_watch_code(s, dev.address)
    check(code and len(code) == 6, 'watch showed a 6-digit pairing code (read off its screen)', code)
    check(is_paired(st), 'pairing with the code succeeds (LE Secure Connections + passkey)', st)
    r = usb(s, 'secstate', 'CMD secstate')
    check(r and 'bonded=1' in r and 'pairing_open=0' in r, 'watch stored the bond and closed the pairing window', r)
    usb(s, 'face', 'CMD face', 5)

    async with BleakClient(dev, timeout=20, winrt=dict(use_cached_services=False)) as c:
        ph = Phone(c)
        check(c.is_connected, 'connected over BLE', f'MTU {c.mtu_size}')
        uuids = [x.uuid for x in c.services]
        check(SVC in uuids and all(c.services.get_characteristic(u) for u in (CMD, EVT, AUD)),
              'Ember Link service + CMD/EVT/AUDIO have exactly the UUIDs in the spec', uuids)
        await c.start_notify(EVT, ph.on_evt)
        await c.start_notify(AUD, ph.on_audio)
        hello = await ph.wait('hello', 5)
        check(hello and hello.get('fw') and 'batt' in hello, 'watch says hello on subscribe (fw, battery, steps)', hello)
        st = await ph.wait('steps', 5)
        check(st and len(st.get('days', [])) >= 1 and st['days'][0]['date'] > 20260000, 'steps history arrives (today first, dated)', st)

        # time sync: set a known odd time, read it back over USB, then restore real time
        now = datetime.datetime.now()
        await ph.send({'t': 'time', 'epoch': int(datetime.datetime(2026, 1, 2, 3, 4, 5, tzinfo=datetime.timezone.utc).timestamp()), 'tz': 0})
        await asyncio.sleep(0.6)
        r = usb(s, 'status')
        check(r and '2026-01-02 03:04' in r, 'time message sets the watch clock', r)
        loc = datetime.datetime.now().astimezone()
        await ph.send({'t': 'time', 'epoch': int(time.time()), 'tz': int(loc.utcoffset().total_seconds())})
        await asyncio.sleep(0.6)
        r = usb(s, 'status')
        check(r and now.strftime('%Y-%m-%d %H:') in r, 'clock restored to real local time', r)

        # notification with quick replies -> Phone app shows it -> tap first reply -> reply event
        await ph.send({'t': 'notif', 'id': 4242, 'app': 'Messages', 'title': 'Alex', 'body': 'Lunch at 1?', 'replies': ['Yes', 'Later']})
        await asyncio.sleep(1.0)
        usb(s, 'launch 7', 'CMD launch')
        await asyncio.sleep(1.0)
        lb = labels(s)
        check(any('Alex' in x and 'Lunch at 1?' in x for x in lb) and 'Yes' in lb and 'Later' in lb,
              'notification + reply buttons shown in the Phone app', lb)
        k = 3; w = (370 - (k - 1) * 8) // k
        usb(s, f'tap {20 + w // 2} 290', 'CMD tap')
        rep = await ph.wait('reply', 4)
        check(rep and rep.get('id') == 4242 and rep.get('idx') == 0, 'tapping "Yes" on the watch sends the reply to the phone', rep)

        # weather on the face
        await ph.send({'t': 'weather', 'temp': -3, 'text': 'Snow', 'hi': 0, 'lo': -6})
        await asyncio.sleep(0.5)
        usb(s, 'face', 'CMD face', 5)
        lb = labels(s)
        check(any('Snow' in x and '-3' in x for x in lb), 'weather appears on the watch face', lb)

        # recordings list with bookmarks
        await ph.send({'t': 'recs_req'})
        rc = await ph.wait('recs', 6)
        files = rc.get('files', []) if rc else []
        check(files and all('n' in f and 's' in f for f in files), 'recordings list arrives', f'{len(files)} files')
        check(any(f.get('marks') for f in files), 'bookmarks travel with the recording list', [f for f in files if f.get('marks')][:1])

        # meeting summary -> Notes app
        await ph.send({'t': 'note', 'id': 'M_TEST_link.wav', 'title': 'Link test meeting', 'summary': 'We tested Ember Link end to end.',
                       'actions': ['Buy a microSD card', 'Measure the battery']})
        await asyncio.sleep(1.0)
        usb(s, 'launch 9', 'CMD launch')
        lb = labels(s)
        check(any('Link test meeting' in x for x in lb), 'meeting summary shows up in the Notes app', lb)

        # Lua app install / list / remove over BLE
        src = b"-- icon: bell\n-- color: 2EC4B6\nfunction start() w.title('BLE app') w.label('installed over Bluetooth', nil, 250, 20) end\n"
        await ph.send({'t': 'app_put', 'name': 'bletest', 'size': len(src)})
        for i in range(0, len(src), 1536):
            await ph.send({'t': 'app_data', 'b64': base64.b64encode(src[i:i + 1536]).decode()})
        await ph.send({'t': 'app_end'})
        await asyncio.sleep(1.6)                              # S3: the watch asks first
        check(any('Allow this?' in x for x in labels(s)), 'install over Bluetooth asks for Allow on the watch')
        usb(s, f'tap {ALLOW[0]} {ALLOW[1]}', 'CMD tap')
        ok = await ph.wait('app_ok', 6)
        await ph.send({'t': 'app_ls'})
        ap = await ph.wait('apps', 4)
        check(ok and ap and 'bletest' in ap.get('list', []), 'Lua app installed over Bluetooth appears in the list', ap)
        await ph.send({'t': 'app_rm', 'name': 'bletest'})
        await asyncio.sleep(1.6)
        usb(s, f'tap {ALLOW[0]} {ALLOW[1]}', 'CMD tap')
        ap = await ph.wait('apps', 4)
        check(ap and 'bletest' not in ap.get('list', []), 'Lua app removed over Bluetooth', ap)

        # translation: config + push-to-talk audio stream + result display
        await ph.send({'t': 'tr_cfg', 'me': 'en', 'them': 'fr'})
        usb(s, 'launch 10', 'CMD launch')
        ph.audio.clear()
        usb(s, 'hold 107 410 1500', 'CMD hold', 8)          # hold the "Me" button 1.5 s
        on = await ph.wait('ptt', 3, lambda m: m.get('on') is True)
        off = await ph.wait('ptt', 4, lambda m: m.get('on') is False)
        n = len(ph.audio)
        seqs = [a[0] for a in ph.audio]
        gaps = sum(((seqs[i] - seqs[i - 1]) & 0xFF) != 1 for i in range(1, len(seqs)))
        check(on and on.get('side') == 'me' and on.get('codec') == 'ima-adpcm' and off, 'push-to-talk start/stop events', (on, off))
        check(n >= 60 and all(len(a) == 133 for a in ph.audio), 'audio blocks stream while holding (1.5 s ~ 90 blocks of 133 B)', f'{n} blocks')
        check(n and gaps <= max(2, n // 20), 'audio sequence numbers mostly continuous (lost blocks <= 5%)', f'{gaps} gaps')
        if n:
            pcm = sum((ember_adpcm.decode_block(a) for a in ph.audio), [])
            rms = math.sqrt(sum(x * x for x in pcm) / len(pcm))
            check(10 < rms < 20000, 'decoded audio is real microphone sound (not silence, not clipping)', f'rms {rms:.0f}')
        await ph.send({'t': 'tr', 'side': 'me', 'src': 'Where is the train station?', 'dst': 'Où est la gare ?'})
        await asyncio.sleep(0.8)
        lb = labels(s)
        check(any('Où est la gare' in x for x in lb), 'translation appears in large text on the watch', lb)

        # guitar practice log + find + wifi handshake
        await ph.send({'t': 'practice_req'})
        pr = await ph.wait('practice', 4)
        check(pr is not None and isinstance(pr.get('sessions'), list), 'practice log arrives', pr)
        await ph.send({'t': 'find_watch'})
        await asyncio.sleep(0.5)
        await ph.send({'t': 'wifi_req'})
        await asyncio.sleep(1.6)
        usb(s, f'tap {ALLOW[0]} {ALLOW[1]}', 'CMD tap')
        wf = await ph.wait('wifi', 12)
        check(wf and wf.get('ssid', '').startswith('Watch-') and len(wf.get('pass', '')) == 12 and re.fullmatch(r'[0-9a-f]{16}', wf.get('token', '')) and wf.get('ip') == '192.168.4.1',
              'Wi-Fi handshake over the ENCRYPTED link: SSID + 12-char password + access key', {k: v for k, v in (wf or {}).items() if k not in ('pass', 'token')})
        await ph.send({'t': 'wifi_off'})
        await asyncio.sleep(3)
        r = usb(s, 'secstate', 'CMD secstate')
        check(c.is_connected and r and 'link_ready=1' in r, 'Bluetooth link survives the Wi-Fi hotspot on/off', f'connected={c.is_connected} {r}')

        # music remote: tap every Remote button -> music_cmd / find_phone events
        usb(s, 'launch 6', 'CMD launch')
        usb(s, 'tapall 1', 'CMD tapall', 40)
        await asyncio.sleep(1.5)                     # let the queued radio events reach the 'phone' first
        mc = [m for m in ph.msgs if m.get('t') in ('music_cmd', 'find_phone')]
        check(len(mc) >= 4, 'Remote app buttons reach the phone (music / find phone)', [m.get('cmd', m.get('on')) for m in mc])
        bad = [m for m in ph.msgs if m.get('t') == '_bad']
        check(not bad, 'every watch message was valid JSON', bad[:2])
    # reconnect
    await asyncio.sleep(2)
    async with BleakClient(dev, timeout=20, winrt=dict(use_cached_services=False)) as c2:
        ph2 = Phone(c2)
        await c2.start_notify(EVT, ph2.on_evt)
        await ph2.send({'t': 'hello_req'})          # what Ember Companion does once it is listening (v1.2)
        check(await ph2.wait('hello', 6) is not None, 'reconnect works (encrypted again from the stored bond, no new pairing)')

    # ---- 4. this laptop forgets the watch -> it is a stranger again: no data, and re-pairing is refused
    await unpair(dev.address)
    await asyncio.sleep(2)
    msgs, err = await attack_unpaired(dev, 'forgotten')
    check(not msgs, 'after forgetting the pairing the laptop gets nothing', f'{len(msgs)} messages, error={err}')
    st, _ = await pair_with_watch_code(s, dev.address)
    check(not is_paired(st), 're-pairing without opening the window on the watch is refused', st)
    await unpair(dev.address)
    usb(s, 'face', 'CMD face', 5)
    print(f'LINK BLE TESTS DONE pass={sum(res)} fail={len(res) - sum(res)}')


if __name__ == '__main__':
    asyncio.run(main())
    sys.exit(0 if res and all(res) else 1)
