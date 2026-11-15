# Continuation notes (start here in a new session)

Last updated 2026-11-15 10:40 local.

## State
- Firmware 1.2 (Ember Link v1.4, Amber check-ins + voice): flashed. tools/amber_test.py 33/33 on the real watch.
- Phone apps: first real-phone run done (Pixel 8, Android 17). Crashes fixed (Keystore IV, recycled Message, Life OS
  shell intent link). Chain watch <-> Companion <-> Gateway <-> Life OS works. Speech model approved in Gateway.
- Life OS side: separate private repo (Amber server, migrations 0022/0023), deployed.

## Buttons (owner 2026-11-15, DONE + tested, tools/mic_test.py 7/7 + recording part skipped while storage is full)
- BOOT hold (450 ms) = talk to Amber while held, release sends (opens Amber from anywhere; closes Tuner/Sound/Translate with a toast)
- BOOT double = start/stop meeting recording (honest errors: "Storage full (x MB free)..." / "Mic in use: <who> (<what>)")
- BOOT short = back / screen off. No triple press.
- mic owner tracking + face "<who> listening"; meeting recording shares the mic with Amber; music recording -> sheet
  (Pause-talk-resume / Save & stop / Keep running)
- Amber replies are 1-4 short bullet lines (Life OS server cutReply + prompt); the watch keeps '
'.

## Power button (owner 2026-11-15, built): click = screen on/off, hold 6 s = device off/on. No Tuner shortcut.
- AXP2101: 0x27 OFFLEVEL=6 s, 0x22 bit1 long-press power-off (not restart), 0x41 bits 2/3 latch long/short press,
  status 0x49 bit3 = click (write-1-to-clear). Polled every 200 ms (pwr_task in buttons.c). Shell: `pmu`.
- Read back OK (r22=02 r27=14 r41=fc). Live click test with the owner pending. In deep sleep a power click does not
  wake the watch (no PMU IRQ line known to the ESP32); the BOOT button / wrist motion do.

## PROBLEM: /flash file system leaked ~6.8 MB (fs_free=0, files 13.8 MB of 20.6 MB, one garbled dir entry)
- Cause: likely writes cut by the many reboots/flashes on 11-15 (a meeting recording M_20261115_091121.wav 13.5 MB was being written).
- Fix plan (needs owner OK): owner downloads the recording via Companion > Meeting notes (also deletes it on the watch);
  back up /flash/apps + notes over USB; reformat the FAT partition; restore. Add a boot-time leak check.
- `du` shell command lists usage.
