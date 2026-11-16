"""Ember OS companion: manage watch apps over USB.

  python tools/ember.py push myapp.lua [--run]   install (or replace) an app, optionally open it
  python tools/ember.py ls                       list installed apps
  python tools/ember.py rm NAME                  remove an app (and its saved data)
  python tools/ember.py run NAME                 open an app on the watch
  python tools/ember.py pwr                      power state, battery, sleep counters
  python tools/ember.py log [SECONDS]            show app print() output and errors
  python tools/ember.py cmd "ANY SHELL LINE"     send a raw shell command

The port is found automatically (Espressif USB, VID 303A); override with --port COMx.
"""
import sys, time, os
import serial
from serial.tools import list_ports


def find_port(argv):
    if '--port' in argv:
        return argv[argv.index('--port') + 1]
    for p in list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    sys.exit('watch not found on USB (no Espressif VID 303A port)')


def radio_off(s):
    """Takes the owner's phone off the watch for the whole test: no test may ring his phone, send audio to Amber or
    write check-ins to his Life OS (2026-11-15/16 incidents). The radio comes back at exit, even after a failure."""
    import atexit
    s.write(b'radio off\n')
    t0 = time.time()
    while time.time() - t0 < 4:
        l = s.readline().decode(errors='replace')
        if l.startswith('CMD radio off'):
            if 'link_ready=0' not in l: raise SystemExit('ABORT: the phone could not be taken off the link: ' + l.strip())
            break
    else:
        raise SystemExit('ABORT: the watch did not confirm "radio off" (old firmware?) - refusing to run with the phone linked')
    if not getattr(s, '_radio_atexit', False):
        atexit.register(lambda: (s.write(b'radio on\n'), time.sleep(0.3)) if s.is_open else None)
        s._radio_atexit = True


def open_watch(port, radio=False):
    """Opens the USB shell. radio=False (default) switches the watch's Bluetooth link off for the test (see radio_off);
    only a test that really needs the radio (link_ble_test.py) passes radio=True."""
    # Set DTR/RTS low BEFORE opening: pyserial's default open raises them for a moment, which resets the watch.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    if not radio:
        for _ in range(10):                      # the shell may still be booting
            s.write(b'pwr\n'); time.sleep(0.4)
            if b'CMD pwr' in s.read(s.in_waiting or 1): break
        s.reset_input_buffer()
        radio_off(s)
    return s


def ask(s, line, prefix, timeout=4.0, sink=None):
    """Send a shell line, return the reply starting with prefix. App output (EMBER ...) seen meanwhile goes to
    sink (a list) if given, else it is printed."""
    s.write((line + '\n').encode())
    t0 = time.time()
    while time.time() - t0 < timeout:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith(prefix):
            return l
        if l.startswith('EMBER'):
            if sink is not None: sink.append(l)
            else: print(l)
        elif 'task_wdt' in l or l.startswith('Backtrace') or 'Guru' in l:
            print('WATCH:', l[:200])            # the watchdog's report about a stuck task: always show it
        elif os.environ.get('EMBER_SHOW_ERRORS') and (l.startswith('E (') or l.startswith('W (')) \
                and 'FT5x06' not in l and 'lcd_panel.io.i2c' not in l and 'pull-up' not in l:
            print('WATCHLOG:', l[:200])         # error/warning log lines (debugging), known touch-chip noise skipped
    return None


def main():
    argv = sys.argv[1:]
    if not argv:
        print(__doc__); return 1
    s = open_watch(find_port(argv))
    cmd = argv[0]
    if cmd == 'push':
        path = argv[1]
        name = os.path.splitext(os.path.basename(path))[0].lower()
        data = open(path, 'rb').read()
        compile_check(data, path)
        r = ask(s, f'app put {name}', 'CMD app put')
        if not r or 'ok' not in r:
            sys.exit(f'put failed: {r}')
        for i in range(0, len(data), 256):
            r = ask(s, 'app hex ' + data[i:i + 256].hex(), 'CMD app hex')
            if not r or ' ok ' not in r + ' ':
                sys.exit(f'transfer failed at byte {i}: {r}')
        r = ask(s, 'app end', 'CMD app end', 8)
        print(f'installed {name} ({len(data)} bytes): {r}')
        if '--run' in argv:
            print(ask(s, f'app run {name}', 'CMD app run'))
            watch_log(s, 3)
    elif cmd == 'ls':
        print(ask(s, 'app ls', 'CMD app ls'))
    elif cmd == 'rm':
        print(ask(s, f'app rm {argv[1]}', 'CMD app rm'))
    elif cmd == 'run':
        print(ask(s, f'app run {argv[1]}', 'CMD app run'))
        watch_log(s, 3)
    elif cmd == 'pwr':
        print(ask(s, 'pwr', 'CMD pwr'))
    elif cmd == 'log':
        watch_log(s, float(argv[1]) if len(argv) > 1 and argv[1][0].isdigit() else 30)
    elif cmd == 'cmd':
        print(ask(s, argv[1], 'CMD', 10))
    else:
        print(__doc__); return 1
    return 0


def watch_log(s, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        l = s.readline().decode(errors='replace').strip()
        if l.startswith('EMBER'):
            print(l)


def compile_check(data, path):
    """Catch syntax errors on the PC first if a Lua interpreter is installed (optional)."""
    import shutil, subprocess
    luac = shutil.which('luac') or shutil.which('luac5.4')
    if luac:
        r = subprocess.run([luac, '-p', path], capture_output=True, text=True)
        if r.returncode:
            sys.exit('syntax error: ' + r.stderr.strip())


if __name__ == '__main__':
    sys.exit(main())
