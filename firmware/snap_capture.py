"""Reads SNAP_* screenshot dumps from the watch over USB serial and writes PNGs to .\snaps\ ."""
import serial, time, base64, os, struct, zlib, re

os.makedirs('snaps', exist_ok=True)
LINE = re.compile(r'^@(\d{5}) ([A-Za-z0-9+/=]+)$')


def png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def ch(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))


s = serial.Serial('COM9', 115200, timeout=2)
s.dtr = False; s.rts = True; time.sleep(.2); s.rts = False
name = None; lines = {}; t0 = time.time()
while time.time() - t0 < 400:
    l = s.readline().decode(errors='replace').strip()
    if l.startswith('child'): l = 'child' + l[5:]
    if l.startswith('SNAP_BEGIN'):
        _, name, w, h, stride = l.split(); w, h, stride = int(w), int(h), int(stride); lines = {}
    elif l == 'SNAP_END' and name:
        total = stride * h; nl = (total + 599) // 600
        miss = [i for i in range(nl) if i not in lines]
        data = b''.join(base64.b64decode(lines[i]) if i in lines else bytes(600) for i in range(nl)).ljust(total, bytes(1))
        rgb = bytearray()
        for y in range(h):
            row = data[y * stride:y * stride + w * 2]
            for x in range(w):
                v = row[2 * x] | (row[2 * x + 1] << 8)
                rgb += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
        png(f'snaps/{name}.png', w, h, bytes(rgb))
        print('saved', name, f'(missing lines: {len(miss)})' if miss else '')
        name = None
    elif name and l:
        m = LINE.match(l)
        if m:
            lines[int(m.group(1))] = m.group(2)
    elif l.startswith('SNAP_ALL_DONE'):
        break
    elif l.startswith(('TREE', 'child')):
        print(l)
    elif l.startswith(('abort', 'Guru', 'assert')) or ' E (' in l:
        print(l[:200])
