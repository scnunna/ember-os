# Ember Companion (Android)

Phone app for the Ember OS watch. Talks to the watch over Bluetooth using the Ember Link protocol (`../docs/EMBER_LINK.md`). Everything runs on the phone: no accounts, no cloud, no analytics. The internet is used only when you tap a Download button (speech model, translation language packs).

Package `com.nunna.embercompanion`, minSdk 31, target/compile 36, Kotlin + Compose Material 3.

## Screens
- **Home**: pair the watch (system chooser showing only names starting `Watch-`), connection status, watch battery, steps and software version, "Find my watch", a Stop button while the phone rings. Links to every other screen.
- **Notifications**: tick which apps buzz the watch (default: messaging apps). Chats that allow replies get 3 quick replies (OK, On my way, Call you later) that the watch can send back. Dismissing on the watch clears it on the phone, and clearing on the phone removes it from the watch.
- **Steps**: 7-day bar chart. Each day is written to Health Connect once (re-syncing updates that day instead of adding a copy). Asked for on connect and every hour.
- **Watch apps**: list what is on the watch, remove apps, install one of 5 bundled Lua apps (Pomodoro, Habit tick, Water counter, Interval timer, Box breathing) or pick your own `.lua` file.
- **Meeting notes**: lists recordings on the watch. "Download and make notes" starts the watch hotspot, joins it, downloads the WAV, switches the hotspot off, turns speech into text with sherpa-onnx (Whisper tiny, downloaded once), then writes a summary and action items. A note shows the transcript with bookmark chips that jump to that spot, can be shared as text, and sent to the watch. Notes are JSON files in the app's private folder.
- **Travel translate**: pick your language and theirs, download the two ML Kit language packs, then hold the button on the watch (or the "I speak" / "They speak" buttons on the phone). The speech is converted to text, translated, spoken aloud in the target language and sent to the watch.
- **Guitar practice**: weekly minutes, streak, fastest-tempo trend, and a button that opens GuitarNoodle if it is installed.
- **Settings**: speech model download/delete, optional AI summary model file, permission shortcuts.

Also: the watch's "Find phone" rings the phone loudly (alarm volume) until the watch or you stop it; the watch Remote app sends play/pause, next, previous and volume to the phone's media session.

### Summaries
Always available: a built-in offline summarizer (sentence scoring plus action-item detection by cue phrases such as "will", "need to", "follow up", "by Friday", "Name will/needs to ..."). Optional: pick a model file (`.task` or `.litertlm`, for example Gemma) in Settings and the app uses MediaPipe LLM Inference instead, falling back to the simple summary if it cannot run. The app never downloads such models itself.

## Layout
- `protocol/` pure Kotlin, no Android classes: framing, JSON messages, IMA-ADPCM decoder, WAV reader/resampler, summarizer, steps mapping, Lua catalog list, chunker, practice maths.
- `link/` pure Kotlin link state machine (`LinkController`) and translate pipeline (`TranslateFlow`).
- `android/` Bluetooth glue (`EmberGatt`), foreground `LinkService`, notification listener, Health Connect, Wi-Fi download, sherpa-onnx, ML Kit, TTS, stores.
- `ui/` Compose screens.
- `app/src/main/assets/lua/` the bundled watch apps.
- `app/src/main/java/com/k2fsa/sherpa/onnx/` vendored sherpa-onnx Kotlin API, `jniLibs/arm64-v8a/` its native libraries (see `THIRD_PARTY.md`).

## Build and test
Needs JDK 17 and the Android SDK (`local.properties` points at `%LOCALAPPDATA%\Android\Sdk`).
```powershell
cd "ESP32 Projects\Projects\AMOLED Watch 2.06\companion"
.\gradlew.bat assembleDebug        # APK: app\build\outputs\apk\debug\app-debug.apk (about 100 MB, mostly ONNX Runtime)
.\gradlew.bat testDebugUnitTest    # 35 JVM + Robolectric tests
```
Install on the phone: copy the APK over, or `adb install -r app-debug.apk`.

## Tests (all on the JVM, no phone)
Framing and ADPCM against `docs/link_vectors.json` (bit-exact), round trip of every message type (also through framing at MTU 23), sequence gaps and wrap, app upload chunking, steps-to-Health-Connect mapping, summarizer action items, Lua catalog files (each has `function start()`, balanced blocks, timers of 100 ms or more), WAV/resampler, chunker, practice stats, translate pipeline with fakes, stores (Robolectric), and the link state machine against an in-memory `FakeWatch` (connect, hello, time, steps_req, hourly steps, small MTU, PTT audio with gaps, app install order).

## NOT tested on a real phone or watch (be honest)
Nothing in this app has run on a device yet (no phone, no emulator was available). Specifically unverified:
- Companion Device Manager pairing and the chooser result handling, the GATT connection, MTU 247 / 2M PHY, notification subscribe order, write queue, reconnect backoff, foreground service and boot restart.
- The watch firmware side of every message (the firmware was built in parallel).
- Notification forwarding, RemoteInput replies, dismiss.
- Health Connect writes and the permission screen.
- Joining the watch hotspot with `WifiNetworkSpecifier` and the HTTP download (the app downloads through the joined network object rather than binding the whole process).
- sherpa-onnx on the phone: native libraries load, Whisper tiny model download and unpacking (about 116 MB), speed, and accuracy. Whisper tiny is small, so expect mistakes on noisy or fast speech.
- ML Kit pack downloads and translation, Android TTS voices, the phone microphone mode.
- MediaPipe LLM summaries with an imported model (`.litertlm` support depends on the MediaPipe version; not tried).
- Find phone volume/ringtone and media key dispatch.
- Meeting recordings are read fully into memory; very long recordings (over about an hour at 44.1 kHz) may run out of memory.
- Only arm64 native libraries are packaged (right for a Pixel 8, not for emulators).
