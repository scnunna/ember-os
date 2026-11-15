# Ember OS roadmap: from a working OS to a useful daily watch (researched 2026-11-14)

## 1. How people use smartwatches now (late 2026)
- **Health first.** Counterpoint says watches with on-device AI reached 25% of shipments in Q1 2026, mostly Apple. Health monitoring is the main reason.
- **AI health and assistant layers.** Samsung's Health Assistant launched in July 2026. Gemini runs on Wear OS. Blood-pressure and "health coach" features are still beta or awaiting regulators.
- **Voice notes moved to dedicated AI recorders.** Examples are the Plaud NotePin, Omi (open source) and Bee. Limitless stopped selling after Meta bought it (Dec 2025). These devices record, transcribe and summarise, usually through a cloud service.
- **Translation lives on phones and earbuds.** The watch mostly shows text and starts the session.
- I found no news dated November 2026 specifically. The trends above come from mid-2026 sources.

## 2. What this watch can and cannot do
| Has | Missing (cannot be added in software) |
|---|---|
| AMOLED 410×502 + touch, BOOT and PWR buttons, **vibration motor (GPIO18)** | Heart-rate and SpO₂ sensors |
| 2 mics (ES7210) + speaker (ES8311) | GPS |
| 6-axis IMU, RTC, 400 mAh battery + AXP2101 fuel gauge | Bluetooth Classic, so **no call audio and no Bluetooth-headset mode** |
| BLE 5, Wi-Fi, 8 MB PSRAM, 32 MB flash, microSD slot | NFC payments, LTE |
| Fast enough for a tuner, metronome, step counting, wake word + ~200 English or Chinese voice commands (ESP-SR) | Too small for Whisper or LLMs. Transcription, translation and summaries must run on the **phone** |

## 3. Connecting to the phone (three options)
1. **Chronos app (already built into the firmware, never tested with a phone).** Fastest way to get time sync, notifications, music control and find-my-phone. It's closed source.
2. **Gadgetbridge (open source, no cloud).** If the watch speaks the Bangle.js protocol (Nordic UART + JSON), Gadgetbridge treats it like a Bangle.js. You'd get notifications, music and steps with no app work.
3. **Our own Android companion app, "Ember Companion" (recommended).** Only our own app can do the audio features: meeting transcription, translation and guitar sessions. Built with Kotlin + Compose, Companion Device Manager pairing and a foreground service. Two links:
   - **BLE GATT plus an L2CAP stream channel** for audio and data. Measured on ESP32-S3: 46 KB/s stock, about 100 KB/s with 2M PHY and logging off. 16 kHz speech is 32 KB/s raw, 8 KB/s as ADPCM and about 2–4 KB/s as Opus, so it fits comfortably.
   - **Wi-Fi** (the watch's hotspot, or the watch joining the phone's hotspot) for big files such as recordings.

   Install it via Obtainium, the same way as your other apps.

## 4. Your five use cases: now → next level
| Use case | On the watch | In the companion app (offline) | Needs |
|---|---|---|---|
| **Watch** | Haptic alarms and taps, raise-to-wake (IMU), weather and calendar from the phone, quick replies to notifications | NotificationListener, canned replies, time/weather/calendar push | Motor driver, BLE service |
| **Guitar practice** | Tuner (built) plus an "in tune" buzz; metronome with silent **haptic beat**; speed trainer (auto +BPM); one-minute chord-change trainer; practice timer; record takes | Practice log and charts, hand-off to **GuitarNoodle**, chord detection from recorded takes | SD card, motor |
| **Meeting notes** | Double-press records (built); tap to **bookmark** important moments; level meter | Transcription: sherpa-onnx (Whisper / Moonshine, plus who-said-what). Summary + action items: Gemma 4 E2B via LiteRT-LM. Results sent back to the watch | **microSD (internal flash drops audio)**, ADPCM/Opus encoder, BLE stream |
| **Steps** | Software pedometer (built, needs tuning on a real walk), move reminders, sleep estimate from wrist motion | Writes to **Health Connect** (reuse code from your Fitness Gym app), history charts | A real-walk calibration |
| **Travel translation** | Push-to-talk on each side; translated text in big letters for the other person; phrase cards offline | Phone speech recognition → **ML Kit offline translation (50+ languages, goes via English)** → phone text-to-speech; download language packs before the trip | Companion app; a phone with offline speech models |

## 5. New things this unlocks
- **Presentation clicker / camera shutter / media remote.** The watch acts as a Bluetooth keyboard (BLE HID) for a laptop or phone. A wrist flick means next slide.
- **"Hey Ember" offline voice commands** for things like "start recording", "metronome one twenty" or "timer five minutes" (ESP-SR, English).
- **Phone-left-behind alert** when the BLE link drops: the watch buzzes.
- **2FA codes on the watch** (TOTP, fully offline).
- **Watch-to-watch walkie-talkie or messages** over ESP-NOW, with no phone or Wi-Fi.
- **Heart rate** by pairing a Bluetooth chest strap directly to the watch (the watch acts as a BLE central).
- **Smart-home buttons** through Home Assistant over Wi-Fi.
- **Focus timer, medication and habit reminders**, as Lua apps with haptics.
- **Talk timer / teleprompter** for presentations.
- **App store in the companion app:** browse Lua apps and push them to the watch.

## 6. Upgrades needed
- **Hardware:**
  - microSD card, 8–32 GB FAT32 (about $10). Required for reliable recording.
  - Confirm the motor really vibrates (free test).
  - Optional BLE heart-rate strap (about $40–90).
- **Phone:**
  - A Pixel 8 can run sherpa-onnx and Gemma 4 E2B offline.
  - Google's own "Advanced" on-device speech mode is listed for **Pixel 10+**.
  - Pixel 8 support for ML Kit GenAI is not confirmed in official docs.
- **Firmware:**
  - Haptics driver, raise-to-wake.
  - Our own BLE "Ember Link" service (2M PHY, longer packets, L2CAP stream).
  - Opus or ADPCM encoder.
  - Optional ESP-SR wake word.
  - BLE central role for the heart-rate strap.
  - Measure the battery first (still pending).

## Status 2026-11-14: P0–P4 built
Watch firmware (Ember OS 1.1) is verified on the device. The companion app (`companion/`) builds and passes its JVM tests, but **has not run on a real phone yet**. See EMBER_OS.md "New in 1.1" and companion/README.md.

## 7. Phases (smallest useful step first)
| Phase | Delivers | Size |
|---|---|---|
| P0 | Haptics on GPIO18, battery measurement, SD recording, raise-to-wake | small, watch only |
| P1 | Companion MVP: pairing, time, notifications + quick replies, steps → Health Connect, push Lua apps | medium |
| P2 | Meeting notes: record → transfer → offline transcript → summary and action items → watch | medium |
| P3 | Travel translation: push-to-talk conversation mode, offline language packs | medium |
| P4 | Guitar suite: haptic metronome, speed and chord trainers, practice log, GuitarNoodle link | small–medium |
| P5 | Extras: BLE clicker, "Hey Ember", heart-rate strap, ESP-NOW, TOTP | pick and choose |

Sources:
- [Counterpoint Q1 2026](https://counterpointresearch.com/en/insights/apple-dominates-as-edge-ai-hits-25-percent-smartwatch-shipments-in-q1-2026)
- [Samsung Health](https://en.wikipedia.org/wiki/Samsung_Health)
- [Wearable AI recorders 2026](https://vibe.us/blog/best-wearable-ai-recorders/)
- [Omi protocol (Opus 16 kHz over BLE)](https://docs.omi.me/doc/developer/Protocol.md)
- [ESP32-S3 BLE throughput thread](https://esp32.com/viewtopic.php?p=134183)
- [ESP-SR command words](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html)
- [Android on-device inference blog (Jul 2026)](https://android-developers.googleblog.com/2026/07/android-on-device-inference.html)
- [ML Kit translation](https://developers.google.com/ml-kit/language/translation)
- [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)
- [LiteRT-LM on-device GenAI](https://developers.googleblog.com/on-device-genai-in-chrome-chromebook-plus-and-pixel-watch-with-litert-lm/)
- [Gemma 4 + ML Kit Prompt API guide](https://medium.com/google-cloud/building-your-first-on-device-ai-feature-with-gemma-4-and-the-ml-kit-prompt-api-1fe94039ab4d)
- [Gadgetbridge Bangle.js](https://gadgetbridge.org/gadgets/wearables/banglejs/)
- [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06)
