# AMOLED Watch 2.06 — Status

> ## 2026-11-14: board #2 runs **Ember OS 1.0** (see `EMBER_OS.md`, research in `OS_RESEARCH.md`)
> Board #2: shows up as **COM3** (scripts now find the port themselves). It has a **battery fitted** (100 %, 4.17 V on arrival).
> Factory flash (ESP-Brookesia + AI chat, 2026-10-15 build) backed up and **verified against the chip**: `Backup_Factory_Board2_2026-11-13/` (SHA-256 in `SHA256.txt`).
> Restore: `python -m esptool --chip esp32s3 -p COM3 write_flash 0 Backup_Factory_Board2_2026-11-13/full_flash_32MB_factory_board2.bin`
>
> Verified on the device: OS tests 15/15 (`python tools\ember_test.py`), layout 0 problems, touch 11/11, 150-step navigation stress with no leaks, deep sleep + timer wake, boot in 1.1 s.
> **Not verified yet (needs you):** light sleep on battery (only happens with USB unplugged), wrist-motion wake from deep sleep, real battery life (read `/flash/power.csv`), and how the panel looks after waking.

## Board #1 history (2026-10-03)

> **2026-10-16: this board was restored to factory and returned.** The plan is to buy the same board *with* the MX1.25 battery and pick up here.
> Our firmware's full flash: `Backup_OurFirmware_2026-10-16/full_flash_32MB_our_firmware.bin` (SHA-256 in `SHA256.txt`), plus `firmware_source_and_images.zip`.
> On the new board: copy its factory flash first (as with `Backup_Factory/`), then run `firmware\build.ps1 flash`.

Board: Waveshare ESP32-S3-Touch-AMOLED-2.06 · COM9 · 32 MB flash · 8 MB PSRAM · no battery, no SD yet.

## What we did to the board
1. Dumped all 32 MB → `Backup_Factory/full_flash_32MB_original.bin` (SHA-256 in `SHA256.txt`).
2. It carried the **Xiaozhi** cloud assistant (talks to `api.tenclass.net`). **Erased the whole flash.** No cloud code or address remains.
3. Flashed our own firmware (`firmware/`). Nothing in it contacts the internet. The only radios: Bluetooth LE (your phone) and an optional Wi-Fi *hotspot* the watch creates itself.

No "firmware update" exists for this board: Waveshare publishes only the Xiaozhi image, and the chip's ROM/fuses can't be updated. Updating = reflashing our own firmware.

## What the firmware does
Swipe up from the clock for the app grid · swipe right = back · **BOOT button**: single = back / screen off, **double = start/stop a meeting recording from anywhere**, long = screen on/off.

| App | State |
|---|---|
| Watch face (time, date, steps ring, battery, REC timer, BT/Wi-Fi/notification icons) | ✅ verified on screen |
| Steps (software pedometer, 7-day history, saved to flash) | ⚠️ works at rest; needs a real walk to tune |
| Recorder (Meeting 16 kHz / Music 44.1 kHz WAV, background, crash-safe header) | ✅ file format + mic rate verified; ⚠️ internal flash drops audio → **needs SD** |
| Tuner (YIN, nearest guitar string, cents bar) | ✅ algorithm exact on 6 synthetic strings; ⚠️ not tried on a real guitar |
| Metronome (30–240 BPM, accent beat) | ✅ builds/UI; ⚠️ sound not heard by us |
| Timer + stopwatch (countdown keeps running + beeps in background) | ✅ UI; ⚠️ beep not heard by us |
| Sound meter + 24-band spectrum | ✅ |
| Phone (BLE Chronos link, notifications list, Wi-Fi hotspot toggle) | ⚠️ untested with a real phone |
| Remote (music play/pause/next/prev/volume, find my phone via Chronos) | ⚠️ untested with a real phone |
| Settings (status, brightness, screen timeout, set clock, deep-sleep toggle) | ✅ |

## Build / flash / test
```powershell
cd "...\AMOLED Watch 2.06\firmware"
.\build.ps1            # compile        .\build.ps1 flash   # flash to COM9
.\selftest.ps1         # flash a self-test build and print PASS/FAIL lines
.\snap.ps1             # flash a screenshot build, save every screen to snaps\*.png
python tools\ble_test.py   # acts like the phone app (needs laptop Bluetooth ON)
```
Toolchain: ESP-IDF v5.5.5 at `C:\esp\esp-idf-v5.5.5` · LVGL 9.5 · Waveshare BSP v2. Add an app = one file + one line in `main/apps.c`.
After changing `sdkconfig.defaults`: delete `sdkconfig`, run `idf.py set-target esp32s3`.
Partitions (32 MB): 2 × 6 MB app slots (OTA-ready) + ~20 MB FAT storage.

## Using it from your Android phone
1. **Time + notifications + music remote:** install the *Chronos* app, pick `Watch-XXXX` (Bluetooth must be on). Protocol written from fbiego/chronos-esp32 (read-only reference); never tested on a phone yet.
2. **Recordings:** Phone app → Wi-Fi hotspot ON → join `Watch-XXXX` with the password on the watch screen → open `http://192.168.4.1` → Save / Delete / "Set watch clock from this phone". Auto-off after 10 idle minutes. No internet is used or needed.

## Next (needs you / parts)
- [ ] Turn laptop Bluetooth on → `python tools\ble_test.py` (validates BLE + time + notification parsing).
- [ ] Insert microSD (FAT32, 32 GB) → reboot → Recorder shows "SD card"; do a 10-min meeting test.
- [ ] Fit the 400 mAh `402728` LiPo — **multimeter-check polarity first** (see `Printing_Package/`). Then: battery %, charging, overnight drain, and decide whether to enable *Deep sleep on battery*.
- [ ] Walk 100 steps, compare, tune `THR` in `main/hw.c`.
- [ ] Pluck a real guitar string in Tuner; adjust gain/threshold if needed.

## 2026-10-04 fixes after your first hands-on test
| You saw | Cause | Fix | Proof |
|---|---|---|---|
| Elements stacked on each other (old clock/app icons at the top, -1m/+1m at the bottom) | Display dropped strips of the screen: 2,412 SPI flush errors (draw buffer not DMA-capable, internal RAM exhausted after Bluetooth/Wi-Fi started) | Two DMA-capable draw buffers allocated at boot; display commands only under the display lock | `DISPLAY FLUSH ERRORS: 0` over layout + 150-step stress |
| Clock 4 min off | Seeded from build time + blind taps on the dark screen pressing `-1m` | PC time pushed right after every flash; taps on a dark screen only wake it; swipes never press buttons | Touch tests 11/11, 6 runs in a row |
| (found while testing) random reboot risk | Stock touch driver aborts on one failed read | Own touch reader, errors ignored | no reboots in stress |
| Settings/Metronome overlaps | Layout bugs | Re-laid out; automated overlap checker | `layout problems: 0` |

Run all device tests: `python tools\e2e.py COM9 150` (needs the watch on USB; ~2.5 min).
Also: `python tools\settime.py` sets the watch clock from this PC.
