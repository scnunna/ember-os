# Continuation notes (start here in a new session)

## 2026-11-16 overnight (owner asleep: DO NOT touch his phone - no rings, no installs)
- BIGGEST battery bug found: the I2S driver's power lock (APB_FREQ_MAX) was held ~98 % of the time with no audio playing
  -> light sleep NEVER happened. Cause: the BSP enables both I2S channels behind esp_codec_dev's back; esp_codec_dev
  defers disabling one direction. Fix: hw_audio_park() at boot and after every mic close (not while a recording is
  only paused). Verified with `pmstats` (CONFIG_PM_PROFILING): both i2s locks released after boot/rec/beep/voice.
  The diag snapshot now logs sleep=% and wk/s per 5 min -> the next outing measures the real gain.
- Screen dark => all LVGL timers pause (300 ms after), mic apps (Tuner/Sound/Translate) close; recorder paused for
  Amber resumes by itself (rec_pause_for_mic).
- Crashes fixed: mic close by a stopped app (session tickets); mic-tap buffer freed during a send (mutex); task
  watchdog when parking I2S during a paused 44.1 kHz recording (park hold).
- All device tests take the phone off the link (ember.open_watch radio=False default). Ringer bug fixed on the phone
  (orphan looping ringtone; 60 s auto-stop). tools/clean_test_recs.py deletes test recordings (run after test sessions).
- Amber speaks (v1.5): firmware SPK + jitter buffer + Settings "Amber voice"; phone TTS side built (112 tests).
  New APKs are on the tailnet install page (NOT installed on the phone).
- Open for the owner: battery purchase (MX1.25 2-pin, 3.7 V, protected LiPo; measure the cavity first; Waveshare does
  not publish capacity/dimensions), 6 h/day recording sync plan (Wi-Fi bulk sync while charging; Google Drive needs
  either the Drive API in Gateway or Life OS/VPS + rclone - owner decision), install the new APKs.

## Battery (owner: "major concern") - first outing 2026-11-15 16:45-20:55, steps only
Data (diag.log): 4.2 h, 12,254 steps, BLE connected throughout (0 drops), 0 crashes, battery 100 -> 12 % = ~21 %/h,
screen on 52 % of the time (active_s 7882 of 15181), 95 wakes, 0 deep sleeps, 20:00 check-in logged missed.
Causes found: (1) raise-to-wake revived a DIMMED screen and reset its timer -> arm swing kept the screen on;
(2) phone link at 48 ms, latency 0 (~21 radio wakes/s); (3) 50 Hz IMU polling while walking.
Fixed 21:05: raise only from a dark screen + must hold face-up and still 400 ms (self-test "walking-swing"),
timeout 20+8 s -> 10+4 s, BLE idle params 120 ms latency 4 (Android accepted; fast 15-30 ms only during PTT, with a
1 s audio ring), SNAP now logs on_5min, raise count, itvl. NEXT: measure %/h on the next outing; then IMU 25 Hz/FIFO,
brightness default, check-in prompt timeout vs "missed".

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

## 2026-11-18
- Greeting at power-on/restart ("Good morning/afternoon/evening" + weekday, 2 s); "Good bye" (2 s) when BOOT turns the screen off on the face; power held ~1 s -> "Good bye / Switching off..." + steps/log saved before the PMU cuts power at 6 s.
- Translate: tap the language pair to swap; gear -> two wheels (I speak / They speak, 30 languages) -> Save sends tr_cfg to the phone (phone saves + echoes).
- Amber app: Voice/Text chip; BOOT click inside Amber toggles spoken replies (mutes her mid-sentence). Hold still = talk.
- Battery: diag sleep% parse fixed (was always 0). Screen off + battery rules: 87% light sleep. Every SNAP now logs `block=` (usb / recording / wifi / setting_off / ok). Next: one real unplugged wear to read block= and sleep% with the phone linked.
- Crash capture verified: `CRASHINFO core reason bt=...` -> `xtensa-esp32s3-elf-addr2line -pfiaC -e build/*.elf <addrs>`.
- Translate on the phone: still needs the Gemma model (TR-06) and a USB/adb look at the phone; not verified yet.
