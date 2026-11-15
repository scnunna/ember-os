# Ember OS 1.1 — power-first, programmable OS for the Waveshare ESP32-S3-Touch-AMOLED-2.06

Ember OS runs on the bare ESP32-S3 (ESP-IDF 5.5 + FreeRTOS + LVGL 9.5). You write watch apps in **Lua**. You can install one over USB, over Bluetooth from the phone app, or over the watch's own Wi-Fi hotspot, with **no reflashing**. Everything stays local: no cloud and no account.

## New in 1.1 (roadmap P0–P4)
- **Haptics.** Vibration motor on GPIO18, plus `w.buzz(ms, count, gap)` for Lua apps. Used for notifications, an "in tune" buzz in the Tuner, a silent metronome beat and practice cues. Settings → *Vibrate*.
- **Raise-to-wake.** Lifting the wrist lights the screen. Settings → *Raise wake*. Synthetic self-test: `raisetest` (4 scenarios).
- **Battery drain.** Settings shows %/h and estimated hours per charge, measured from the last unplug.
- **Recorder.** No dropped audio even on internal flash (30 s test: 0 dropped samples). **Mark** button for bookmarks, which travel with the recording to the phone.
- **Ember Link** (`docs/EMBER_LINK.md`): our own Bluetooth protocol for the **Ember Companion** Android app (`companion/`). It carries time, notifications with **quick replies**, weather, steps → Health Connect, Lua app install and remove, recordings + Wi-Fi download, meeting summaries, translation audio, the practice log and the music remote. Chronos still works alongside it.
- **New apps.**
  - **Notes:** meeting summaries and action items sent back from the phone.
  - **Translate:** hold *Me* or *Them* and speak. The phone translates offline and speaks the result; the watch shows it in large text.
  - **Practice:** 1-minute chord-change trainer, session timer, weekly minutes and streak.
- **Upgraded apps.**
  - Metronome: *Vibe* (silent haptic beat) and *Speed* trainer (+2/+5/+10 BPM every 4 bars); sessions are logged.
  - Tuner: A=440/432 switch and a buzz when a string stays in tune.
- **Memory hardening.** Internal RAM is the scarce resource on this board.
  - Audio is claimed at boot.
  - BLE, Wi-Fi/LWIP and big buffers live in PSRAM.
  - Wi-Fi unloads completely when the hotspot turns off.
  - The hotspot refuses to start below 64 KB free instead of crashing.
- **Test suites** (run all after any change):
  - `tools/ember_test.py`: 32 OS checks
  - `tools/apps_test.py`: 25 checks, every app and every button, real taps
  - `tools/link_usb_test.py`: 20 protocol checks over USB
  - `tools/link_ble_test.py`: protocol over real Bluetooth, the laptop acting as the phone
  - `tools/e2e.py`: layout, touch and navigation

## The hardware (read from this board on 2026-11-13)
| Part | Chip | Notes |
|---|---|---|
| SoC | ESP32-S3R8 (QFN56) rev v0.2, dual-core up to 240 MHz | Secure Boot / Flash Encryption **off**, all eFuse keys empty |
| Memory | 8 MB octal PSRAM (AP, 3 V) + **32 MB** quad flash (GigaDevice, ID c8/4019) | |
| Display | 2.06" AMOLED 410×502, **CO5300** controller over QSPI (SH8601-compatible driver) | |
| Touch | FT3168 (I²C), INT → GPIO38 | |
| Power | **AXP2101** PMIC with fuel gauge | reported online by the factory firmware |
| Motion | QMI8658 6-axis IMU, INT1 → GPIO21 (an RTC pin, so it can wake deep sleep) | hardware pedometer unreliable on this revision, so steps are counted in software |
| Clock | PCF85063A RTC (battery backed) | |
| Audio | ES8311 speaker codec (amp enable GPIO46) + ES7210 mic ADC, 2 mics | |
| Storage | microSD slot (SDMMC 1-bit: CLK 2, CMD 1, D0 3) | |
| Buttons | BOOT (GPIO0) + PWR (AXP2101) | |
| Radio | Wi-Fi 4 + BLE 5 | |

Factory firmware on this unit: Waveshare's **ESP-Brookesia** phone-style launcher with an AI-chat app (ESP-IDF v5.5.4, built 2026-10-15). Its full 32 MB flash is backed up in `Backup_Factory_Board2_2026-11-13/`.

## Power design (the main point)
| State | When | What is on | Why it saves power |
|---|---|---|---|
| **ACTIVE** | screen on | CPU 80–160 MHz (capped; 240 is never needed for UI), LVGL drawing | Capped clock + race-to-idle |
| **DOZE** | screen off | Panel in **SLEEP-IN**, CPU in **automatic light sleep** between events, BLE advertising every ~1.1 s | The chip sleeps between events and wakes only when something needs doing |
| **DEEP** | 30 min dark + still + on battery + nothing running | Only the RTC domain and IMU wake-on-motion | ~10s of µA for the SoC. Lift your wrist or press BOOT to wake |

What I changed to make DOZE actually sleep (each item used to wake the CPU constantly):
- LVGL's 5 ms tick timer (**200 wake-ups/s**) is gone. LVGL reads the clock from `esp_timer` and sleeps until its next deadline, or at most 2 s.
- The BOOT button is now **interrupt-driven**. Before, it was polled 100×/s.
- The USB shell blocks instead of polling 5×/s.
- The step counter polls at **50 Hz only while you move**. It drops to 4 Hz after 10 s of stillness.
- App timers **pause automatically** while the screen is dark.
- GPIO hold: the display/touch reset, chip-select, amp, I²C and wake pins keep their level during light sleep. Before, auto light sleep isolated **every** pin, which would have floated the display's reset line.
- Bluetooth controller in modem-sleep mode with the main crystal kept on during light sleep, so BLE survives sleep.
- Boot from deep sleep is faster: no PSRAM memtest (saves ~290 ms), no image re-validation, quiet bootloader.

Light sleep cuts the USB serial link, so **the watch only sleeps on battery**. On USB it stays awake so you can develop. Toggle in *Settings → Power saving*. Deep-sleep delay: NVS pref `deep` (minutes, 0 = never).

Measuring: `python tools\ember.py pwr` shows time active/dozing, wake count, deep-sleep count and battery. The watch also appends a line to `/flash/power.csv` every 15 minutes. That log is the real battery-drain measurement, because the AXP2101 gives % and mV but not current.

## Writing an app
Save a file such as `timer2.lua`. The file name becomes the app name (a–z, 0–9, `_`, `-`, max 20 characters).
```lua
-- icon: bell          (audio bell bluetooth charge eye gps home image list loop play plus refresh
--                       settings shuffle tint up edit file wifi battery warning ok power)
-- color: 2EC4B6
local n = w.load('n', 0)           -- per-app storage survives reboots
local lbl
function start()                    -- called when the app opens
  w.title('Taps')
  lbl = w.label(tostring(n), nil, 220, 48)
  w.button('Tap', nil, 380, 200, 80, 0x2EC4B6, function() n = n + 1; w.text(lbl, n); w.save('n', n) end)
end
function on_button() return false end   -- BOOT pressed: return true to keep the app open
function on_stop() end                  -- app is closing
```
Install: `python tools\ember.py push timer2.lua --run`. The icon appears in the app grid right away.
Or from the phone: *Phone app → Wi-Fi hotspot ON* → join `Watch-XXXX` → open `http://192.168.4.1` → *Ember apps → Install*.

### API (`w.*`). The screen is 410×502. Widgets are placed by their **centre**; `nil` for x means centred.
| Call | Does |
|---|---|
| `w.title(text)` | title at the top |
| `w.label(text, cx, cy, size, color)` → h | text; size 14/20/28/48 |
| `w.button(text, cx, cy, w, h, color, fn)` → h | tap calls `fn` |
| `w.circle(cx, cy, d, color, ring)` → h | dot, or ring if `ring` = true |
| `w.bar(cx, cy, w, h, max)` → h · `w.value(h, v)` | progress bar |
| `w.text(h, s)` `w.color(h, c)` `w.move(h, cx, cy)` `w.show(h, bool)` | change widgets |
| `w.every(ms, fn)` → id · `w.cancel(id)` | repeating timer, ≥100 ms, paused while dark |
| `w.time()` → h, m, s, wday · `w.date()` → y, m, d · `w.millis()` | clock |
| `w.steps()` · `w.battery()` → pct, mv, charging, usb · `w.imu()` → x, y, z (g) | sensors |
| `w.beep(hz, ms)` · `w.toast(text)` | feedback |
| `w.load(key, default)` · `w.save(key, value)` | numbers and strings, per app |
| `print(...)` | shows in `ember.py log` |

Numbers you pass (positions, sizes, colours, timer intervals) may be fractions; they are rounded.

### Rules the OS enforces (so one bad app can't drain the battery)
- Apps only run when something happens: `start`, a tap, the button or a timer. There are no background apps. Background work such as steps, recording and alarms belongs to the OS.
- Any single callback longer than **250 ms** is stopped with an on-screen error.
- Lua memory is capped at **256 KB** (in PSRAM). There is no file, OS or network access.
- Timers must be at least 100 ms apart. They stop while the screen is dark and resume when it lights up.

## Files
`firmware/main/power.c` power manager · `script.c` Lua runtime + `w` API · `buttons.c` interrupt button · `launcher.c` grid with scripted apps · `components/lua/` Lua 5.4.7 (MIT, 32-bit numbers for the FPU) · `tools/ember.py` PC tool · `tools/ember_test.py` (28 OS checks), `tools/apps_test.py` (every app, every button, real taps) and `tools/e2e.py` (layout/touch/navigation): run all three after any change.
