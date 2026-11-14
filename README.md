# Ember OS: a power-first, programmable OS for the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch

Ember OS runs on ESP-IDF 5.5, FreeRTOS and LVGL 9.5. You write watch apps in **Lua** and install them over USB or from your phone over the watch's own Wi-Fi hotspot. No reflashing, no cloud, no account.

- **Power:** three states.
  - ACTIVE: screen on, CPU capped at 160 MHz.
  - DOZE: panel asleep, automatic light sleep, interrupt-driven button and touch.
  - DEEP: deep sleep after 30 minutes of stillness. Wakes on wrist motion or the BOOT button.
- **Apps:** Lua 5.4, sandboxed. Each callback is limited to 250 ms, the heap to 256 KB, and timers pause while the screen is dark.
- **Built in:** watch face, steps, recorder, guitar tuner, metronome, timer, sound meter, phone link (Chronos BLE), music remote and settings, plus sample Lua apps.

Start with [EMBER_OS.md](EMBER_OS.md), which covers the hardware, the power design and the app API. [OS_RESEARCH.md](OS_RESEARCH.md) explains why the existing firmware options didn't fit. Current state is in [STATUS.md](STATUS.md).

## Build and flash (Windows, ESP-IDF v5.5.x)
```powershell
cd firmware
.\build.ps1 flash                         # finds the watch's USB port, flashes, sets the clock
python tools\ember.py push myapp.lua --run
python tools\apps_test.py                 # every app, every button, real taps
python tools\ember_test.py                # OS checks (power, sandbox, audio, memory)
python tools\e2e.py COM3 150              # layout / touch / navigation stress
```
Back up your board's factory flash before the first flash (see STATUS.md). The flash dumps are deliberately not in this repo.

Third-party code: Lua 5.4.7 (MIT) in `firmware/components/lua`. The Waveshare BSP, LVGL and other dependencies come from the ESP component registry via `dependencies.lock`.
