# What OS options exist for the ESP32-S3-Touch-AMOLED-2.06? (researched 2026-11-13)

Short answer: there are demos and hobby firmwares, but **no OS that is both power-first and programmable without reflashing**. That is why Ember OS exists.

| Option | What it is | Power management | Add apps without reflashing? | Verdict |
|---|---|---|---|---|
| Waveshare factory firmware (ESP-Brookesia + Xiaozhi AI chat) | Phone-style demo launcher | none beyond screen off | no | demo; the AI app talks to a cloud server |
| [joaquimorg/OLEDS3Watch](https://github.com/joaquimorg/OLEDS3Watch) | ESP-IDF + Brookesia watch firmware, GPL-3 | not documented | no | early, 9 commits |
| [robin3951/waveshare_amoled_smartwatch](https://github.com/robin3951/waveshare_amoled_smartwatch) | ESP-IDF + LVGL, Chronos BLE, software pedometer | screen off after 15 s only (panel never sleeps) | no | active hobby project |
| [withinaz/watch-esp32](https://github.com/withinaz/watch-esp32) | launcher + voice assistant | not documented | no | touch broken in some apps |
| [infinition/waveshare-watch-rs](https://github.com/infinition/waveshare-watch-rs) | Rust no_std + Embassy, games, 386★ | light sleep via Embassy, AOD; **no deep sleep** (author: "a few hours" of battery). A [fork](https://users.rust-lang.org/t/rust-stack-for-the-smartwatch-esp32-s3-touch-amoled-2-06/139904) adds deep sleep | no (Rust trait, reflash) | best power work so far |
| [LostBeard/SpawnWear](https://github.com/LostBeard/SpawnWear) | C# on .NET nanoFramework, apps loaded from SD | "power-aware", not detailed | **yes (C# DLLs on SD)** | early; heavy runtime; no licence |
| [turfptax/ESP32Watch](https://github.com/turfptax/ESP32Watch) | MicroPython, drivers for CO5300 + FT3168 | none | yes (Python files) | slow display without a C module; MicroPython idles at much higher current |
| Tactility | ESP32 app OS | — | yes (ELF apps) | support for this board only in an [open PR](https://github.com/TactilityProject/Tactility/pull/525) |
| InfiniTime / wasp-os / Bangle.js | real smartwatch OSes | excellent | yes | **nRF52 chips only, won't run on ESP32** |

**Why build instead of adopt:**
- The low-power ones can't load apps.
- The programmable ones ignore power.
- Our own firmware already had verified drivers for this exact board: display flush fix, touch, steps, recorder and BLE.

Ember OS adds the two missing pieces on top of those drivers: a sleep-first power manager and a sandboxed Lua app runtime.

Other sources: [Waveshare docs](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.06) · [Waveshare GitHub](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06)
