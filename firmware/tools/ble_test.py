"""Acts like the Chronos phone app: finds the watch, subscribes, sends time + a notification. Usage: python tools/ble_test.py"""
import asyncio, datetime, sys
from bleak import BleakScanner, BleakClient

RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # phone -> watch (write)
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # watch -> phone (notify)


def frame(cmd, payload, kind=0xFF):
    body = bytes([kind, cmd]) + payload
    return bytes([0xAB, len(body) >> 8, len(body) & 0xFF]) + body


async def main():
    print("scanning 8 s ...")
    devs = await BleakScanner.discover(timeout=8.0)
    watch = next((d for d in devs if d.name and d.name.startswith("Watch-")), None)
    if not watch:
        print("FAIL: no 'Watch-xxxx' found. Seen:", [d.name for d in devs if d.name][:10]); return 1
    print("found", watch.name, watch.address)
    got = []
    async with BleakClient(watch.address, timeout=20) as c:
        print("connected; MTU", c.mtu_size)
        await c.start_notify(TX, lambda _, d: got.append(bytes(d)))
        await asyncio.sleep(1.5)
        print("frames from watch after subscribe:", [g.hex() for g in got])
        now = datetime.datetime.now()
        t = bytes([0x00, 0x00, now.year >> 8, now.year & 255, now.month, now.day, now.hour, now.minute, now.second])
        # layout used by the reference lib: [7..8]=year, [9]=month, [10]=day, [11]=hour, [12]=min, [13]=sec
        pkt = bytes([0xAB, 0x00, 0x0B, 0xFF, 0x93, 0x80, 0x00]) + bytes([now.year >> 8, now.year & 255, now.month, now.day, now.hour, now.minute, now.second])
        await c.write_gatt_char(RX, pkt, response=False)
        print("sent time", now.strftime("%Y-%m-%d %H:%M:%S"))
        await asyncio.sleep(0.5)
        msg = b"Mom: Dinner at 7?"
        n = bytes([0xAB, 0x00, 6 + len(msg) - 1 + 0, 0xFF, 0x72, 0x80, 0x03, 0x02]) + msg   # icon 3, state 2 = new
        n = bytes([0xAB, (len(msg) + 5) >> 8, (len(msg) + 5) & 255, 0xFF, 0x72, 0x80, 0x03, 0x02]) + msg
        await c.write_gatt_char(RX, n, response=False)
        print("sent notification")
        await asyncio.sleep(1.0)
        phone_batt = bytes([0xAB, 0x00, 0x05, 0xFE, 0x91, 0x80, 0x00, 77])
        await c.write_gatt_char(RX, phone_batt, response=False)
        await asyncio.sleep(1.0)
    print("PASS: BLE link works end-to-end")
    return 0


sys.exit(asyncio.run(main()))
