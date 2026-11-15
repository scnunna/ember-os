# Continuation notes (start here in a new session)

Last updated 2026-11-15 10:40 local.

## State
- Firmware 1.2 (Ember Link v1.4, Amber check-ins + voice): flashed. tools/amber_test.py 33/33 on the real watch.
- Phone apps: first real-phone run done (Pixel 8, Android 17). Crashes fixed (Keystore IV, recycled Message, Life OS
  shell intent link). Chain watch <-> Companion <-> Gateway <-> Life OS works. Speech model approved in Gateway.
- Life OS side: separate private repo (Amber server, migrations 0022/0023), deployed.

## In progress: double press = talk to Amber, mic ownership (NOT finished)
Goal (owner 2026-11-15):
- double press -> open Amber and listen; double press again (or tap Send) sends
- triple press -> meeting recording (moved from double)
- mic owner tracking (`hw_mic_open_as`, `hw_mic_owner`): who + what + how long; face shows "<who> listening"
- meeting recording (16 kHz): Amber SHARES the mic (`hw_mic_tap_*`), recording continues, moment bookmarked
- music recording: "Mic in use" sheet with Pause-talk-resume / Save & stop / Keep running (`rec_pause`)
- Tuner/Sound/Translate: opening Amber closes them, toast says which
Code: hw.c (mic owner + tap), rec.c (pause/resume), link.c (PTT share + error reasons), app_amber.c (sheet, toggle,
amber_double_press), buttons.c (press counting), launcher.c (btn_double/triple), face.c, cmd.c (micstate, btn N,
recstart/recstop, taptest, recpause).
Test: tools/mic_test.py (run with Ember Companion force-stopped). LAST RUN 6/21 PASS. Failures to debug:
- `btn 2` returns app=2 (Tuner) instead of Amber 12, and "Phone not connected" never shows: check launcher_open from the
  cmd task while holding the display lock, and whether start_talk (vTaskDelay inside the lock) blocks the screen load.
- triple press (`btn 3`) does not start a recording; `recstart 1` fails too -> rec_start returns false. Suspect rec state
  (s_done false?) or mic left open by the Amber attempt; check micstate right after the Amber attempt.
- taptest open=0 only because no recording was running.
