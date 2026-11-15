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

## Proposed next (owner asked "what do you think"): PWR button
- PWR click = screen on/off, PWR hold = device off/on (AXP2101 hardware; options 4/6/8/10 s, suggest 6 s),
  PWR double = Tuner (if the owner confirms he wants a Tuner shortcut). Firmware does not read PWR yet:
  poll AXP2101 IRQ status (PKEY short/long bits) while awake; verify on the device first.

## PROBLEM: /flash file system leaked ~6.8 MB (fs_free=0, files 13.8 MB of 20.6 MB, one garbled dir entry)
- Cause: likely writes cut by the many reboots/flashes on 11-15 (a meeting recording M_20261115_091121.wav 13.5 MB was being written).
- Fix plan (needs owner OK): owner downloads the recording via Companion > Meeting notes (also deletes it on the watch);
  back up /flash/apps + notes over USB; reformat the FAT partition; restore. Add a boot-time leak check.
- `du` shell command lists usage.
