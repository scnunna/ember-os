# Use case: "Ember Coach", a private Jarvis-style assistant on the watch (2026-11-14)

**Goal:** talk to the watch in plain language at any time. Every 30 minutes it checks you are doing what your schedule says. Everything runs on the watch and your phone; nothing goes to an outside server unless you approve that specific request.

## 1. How it works (who does what)
```
 WATCH (mic, screen, buzz, buttons)  --Bluetooth (encrypted)-->  PHONE (the brain, offline)
   hold-to-talk -> audio (ADPCM, built)                           speech-to-text (sherpa-onnx Whisper, built)
   check-in card + buttons <- text                                 coach logic + schedule (local calendar)
   buzz / show answer      <- text (+ phone/earbud voice)          small local LLM (Gemma 4 E2B, LiteRT-LM)
                                                                    text-to-speech (Android offline voices)
 optional: HOME BRAIN (your RTX-4060 laptop on home Wi-Fi) for a smarter model, still your own hardware
 optional: CLOUD, only through an "approval gate": you see exactly what is sent and tap Allow, every time
```
Why the phone is the brain: the watch chip (ESP32-S3) cannot run speech recognition or an LLM. The phone can, offline. The commercial pendants (Plaud, Omi) split it the same way, except they use the cloud.

## 2. The daily loop
1. **Morning plan (2 min):** say "Today: report until 11, gym at 12, calls 2 to 4." The phone turns it into blocks, merged with your phone calendar, which it reads on the phone and never uploads.
2. **Every 30 min check-in:** the watch buzzes and shows *"11:30 — plan: Report. Still on it?"* with buttons **Yes / Switched / Snooze 10**. You can also hold the button and say *"No, I got pulled into email."*
   - The phone logs it, nudges you back ("Report is due at 1 — want 25 focused minutes now?"), and can re-plan the rest of the day.
3. **Missed check-in:** a second buzz after 5 min, then it's logged as "no answer". During calendar meetings it only does a silent buzz.
4. **Ask anything, any time:** hold to talk, for example "What's next?", "Move gym to 5", "Remind me to call Mom at 7", "How did I do this week?"
5. **Evening review:** planned vs actual, streaks, one suggestion for tomorrow. It's also sent to Notes on the watch.

**Key design choice:** check-ins use fixed templates plus your schedule, so they work instantly with no AI at all. The LLM is only used when you speak free-form. The core loop is fast, reliable and battery-cheap, even if the phone model turns out slow.

## 3. "Talk at all times": three ways, honestly compared
| Mode | How | Battery | Verdict |
|---|---|---|---|
| **Hold-to-talk on the watch** (recommended) | Raise wrist, hold a button, speak, release (the Translate app already does this) | Tiny: mic on only while held | **Use this.** Reliable, private, no false triggers |
| Wake word on the **phone** ("Hey Ember") | Phone listens all the time (sherpa-onnx keyword spotting / openWakeWord, both offline and open-source) | Phone battery; muffled in a pocket | Good add-on at a desk, or with earbuds |
| Wake word on the **watch** | ESP-SR WakeNet. Custom words need Espressif's training process | **Estimate: watch lasts under 1 day.** The mic and codec plus constant processing stop it sleeping (not measured) | Not recommended on this board |

## 4. Security plan (the "hacking protection")
**Current state, important:** Ember Link v1 is **unauthenticated and unencrypted**. Anyone nearby with a BLE app could:
- connect,
- send fake notifications,
- install a Lua app,
- read the list of recordings,
- request the hotspot password and then download recordings.

The repo is public, so the protocol is known. **Fix this before adding the assistant.**

| # | Protection | What it stops | Effort |
|---|---|---|---|
| S1 | **BLE LE Secure Connections + bonding.** A 6-digit code shown on the watch is typed into the phone once, and all Ember Link data is encrypted. Ember Link characteristics require an encrypted, authenticated link. Only the bonded phone is accepted. | Strangers connecting, eavesdropping, fake commands | small–medium (firmware + app) |
| S2 | **Chronos service off by default** (old protocol, no auth); a toggle in Settings | The unauthenticated side door | small |
| S3 | **Confirm-on-watch for risky actions:** installing an app, starting the hotspot or deleting data needs a tap on the watch | A compromised phone app silently changing the watch | small |
| S4 | **Wi-Fi hotspot:** a new random password every session, sent only over the encrypted link, plus an access token for the web page; auto-off (built) | Someone reusing an old password | small |
| S5 | **Phone "air-gap" by design:** the assistant app has **no INTERNET permission**, so Android itself blocks all network access. A separate tiny "Gateway" app is the only thing that can go online. It shows exactly what would be sent, needs your tap each time, and logs everything. | Any leak "by accident" from the AI app; this is the strongest guarantee you can get | medium |
| S6 | **Prompt-injection guard:** text from notifications or files is data, not commands. The LLM can suggest actions; sending, deleting or installing always needs your tap | A malicious message tricking the AI | small |
| S7 | **Encryption at rest:** notes and logs encrypted with Android Keystore; raw audio auto-deleted after transcription (your choice); watch recordings deleted after transfer | A lost or stolen phone or watch exposing data | small–medium |
| S8 | **Watch flash encryption + Secure Boot** (eFuses). **Irreversible**, so do it last, in "development mode" first, with backups kept | Someone pulling your data or code off a stolen watch | medium, careful |
| S9 | **Red-team it ourselves:** a ~$10–30 nRF52840 BLE sniffer + Wireshark, the existing test scripts as an "attacker" (they must **fail** after S1), JSON fuzzing, and a Wi-Fi test with your Kali kit | Finding what we missed | small, ongoing |

**Privacy and law:** only record conversations you're part of, and tell people when you record meetings. Canada generally allows one-party consent, but this isn't legal advice.

## 5. Limitations (what it will NOT do well)
- **The phone must be nearby:** Bluetooth range is about 10 m. Away from the phone, the watch still keeps your schedule and check-ins if they were synced in advance, but voice needs the phone.
- **Phone AI speed on a Pixel 8 is unverified.** Google's published Gemma 4 numbers are for newer phones: about 52 tokens/s on an S26 Ultra GPU. Reports say Pixels often fall back to CPU (a few tokens/s on mid-range CPUs). Expect short answers. Measure on your phone in week 1; if too slow, use the Home Brain (Tier 2).
- **Speech:** Whisper tiny makes mistakes in noise or fast speech, and the watch mic is small. Commands like "move gym to 5" should be fine; long dictation will need correcting.
- **Small local model:** good at schedule logic and short coaching; weaker at complex reasoning; knows nothing after its training date; no internet unless you approve.
- **Voice replies:** the watch speaker is tiny. Spoken answers come from the phone or Bluetooth earbuds; the watch shows text plus a buzz.
- **Android background limits:** it needs a foreground service, exact-alarm permission and a battery-optimisation exemption, or check-ins can be late.
- The companion app **hasn't run on a real phone yet**. Week 1 is device testing.

## 6. Budget tiers
| Tier | One-time cost | What you get | Quality |
|---|---|---|---|
| **T0 — what you own** | **$0** | Watch + Pixel 8: check-ins, buttons, hold-to-talk, Gemma 4 E2B on the phone, offline speech | Works; answers short and maybe slow |
| **T1 — essentials** | **~$40–80** | microSD card (~$10, reliable recording), nRF52840 BLE sniffer (~$10–30, security testing), basic Bluetooth earbuds (~$20–40, private spoken replies) | Same brain, better reliability and privacy testing |
| **T2 — Home Brain** | **$0** using your RTX-4060 laptop, or **~$400** Jetson Orin Nano Super (price reportedly raised from $249; check), or **~$900+** Mac mini M6 (sources conflict; check) | The phone forwards questions over **home Wi-Fi** to a bigger local model (roughly 8–14B class on the laptop). Still 100% your hardware; only on home Wi-Fi unless we set up your own VPN | Big jump in answer quality and speed at home |
| **T3 — premium** | ~$800–1,300+ (rough; not researched) | A newer phone with stronger on-device AI (Google lists its advanced on-device speech mode for Pixel 10+), or a future custom watch with better mic, motor and battery | Best fully-offline experience |
| **Cloud (opt-in)** | Pay per use, $0 if never approved | For hard questions only, through the approval gate; you see the text before it's sent | Smartest answers. I'll price it exactly before you enable it |

**Data use:** T0–T2 send nothing over the internet, so mobile data use is about zero. Model downloads (~0.1–2 GB) happen once over Wi-Fi.

## 7. Recommended order
| Step | What | Why first |
|---|---|---|
| J0 | **Security S1–S4 + S6** | The current link is open; fix it before the assistant handles your schedule |
| J1 | **Check-in loop with templates + buttons** (no LLM) + morning plan by buttons/voice | Delivers "keep me on schedule" immediately and reliably |
| J2 | **Hold-to-talk Q&A** with Gemma 4 E2B on the phone + tool calls (schedule, reminders, focus timer) | Natural language |
| J3 | **Measure on your Pixel 8** (speed, battery, accuracy) → decide on T2 | Spend money only if numbers say so |
| J4 | S5 Gateway app, S7 encryption at rest, then S8 watch flash encryption | Full privacy guarantee |

## 8. A private network for just the watch and the phone ("Tailscale for two")
| Option | Servers involved | On this watch | Verdict |
|---|---|---|---|
| **Bonded BLE (LE Secure Connections) + our own app-layer encryption (Noise protocol, keys made at pairing)** | **None** | Fits easily (crypto is in ESP-IDF) | **Recommended for watch↔phone.** The pairing is the "network"; the same keys also protect Wi-Fi file transfers |
| Plain WireGuard, peer to peer (esp_wireguard / esphome wireguard + the official phone app) | None | Possible, but adds memory on top of Wi-Fi (~59 KB already) and needs synced clocks; battery cost | Only if we need IP networking on the watch |
| Tailscale on ESP32 (MicroLink, esphome-tailscale; S3 + PSRAM recommended) | Tailscale's coordination server (traffic is end-to-end encrypted; keys and metadata go through them) | Reported working, but one test saw resets during transfers | Not "strictly yours" |
| Tailscale clients + **Headscale** (self-hosted control server on your laptop) | Your own | As above | Good for **phone↔home-brain laptop** when away from home, not needed for the watch |

**Plan:** watch↔phone = bonded BLE + Noise keys (zero servers). Phone↔laptop away from home = WireGuard or Headscale that you host. No VPN on the watch itself.

## 9. What the leading watches do (late 2026) and where they fall short
- **Apple Watch Series 11:** polished, best with an iPhone; a 4-month review says it still needs **nightly charging** despite the 24 h claim.
- **Galaxy Watch 8:** about 1–2 days of battery (reports vary); most health features **Android-only**; Samsung added an AI Health Assistant (Jul 2026).
- **Pixel Watch 4/5:** Gemini on the wrist, but **mostly cloud**. Pixel Watch 5 added a *limited* offline mode (timers, alarms, simple actions).
- **Garmin:** 12–14 days of battery, but launched **Connect+ at $6.99/month** (2025 backlash). **Whoop** needs a subscription (from $199/yr). **Fitbit Premium** moved features behind a paywall.
- **Market:** on-device AI reached about 25% of shipments in Q1 2026, mostly Apple. Health is the main driver.

| Pain point | What we do differently |
|---|---|
| Daily charging | Sleep-first power manager; measured drain shown in Settings (still has to be proven on your wrist) |
| Subscriptions and paywalls | **None.** Your data stays as plain files on your phone, so you can export them |
| Privacy (a vendor survey says 74% of users worry) | Nothing leaves your devices; the AI app has no internet permission; cloud only per-approval |
| AI needs the cloud | The assistant runs on your phone or your laptop, fully offline |
| People stop wearing it after weeks; "data I can't use" | A **coach that acts**: half-hourly check-ins, re-planning, evening review, instead of dashboards |
| Notification overload | Phone-side triage: only important messages buzz; the rest are batched into a summary (local) |
| Lock-in and closed apps | Open protocol, Lua apps, open-source firmware |
| Repair and durability | $40-class board you can reflash or replace; 3D-printed case |

**Where we can't compete:** no heart-rate, SpO₂ or GPS sensors; no payments or LTE; the dev board isn't water-resistant; the speaker and mic are small. A BLE chest strap can add heart rate.

Sources:
- [LiteRT-LM Gemma 4](https://ai.google.dev/edge/litert-lm/models/gemma-4)
- [Google LiteRT-LM blog](https://developers.googleblog.com/blazing-fast-on-device-genai-with-litert-lm/)
- [Gemma 4 Android guide (S26 E2B/E4B speeds)](https://dev.to/rahul_patwa_f99f19cd1519b/gemma-4-on-android-complete-developer-guide-to-on-device-ai-f5b)
- [Edge Gallery benchmarks (Pixel CPU fallback)](https://codersera.com/blog/install-google-ai-edge-gallery-to-run-ai-models-on-your-phone/amp/)
- [sherpa-onnx keyword spotting](https://k2-fsa.github.io/sherpa/onnx/kws)
- [openWakeWord Android](https://github.com/Re-MENTIA/openwakeword-android-kt)
- [Wake word models 2026](https://dev.to/buguatec/how-to-get-a-wake-word-model-in-2026-3amo)
- [ESP-SR wake word customization](https://docs.espressif.com/projects/esp-sr/en/latest/esp32/wake_word_engine/ESP_Wake_Words_Customization.html)
- [ESP-SR S3 benchmark](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html)
- [Jetson price increase](https://www.notebookcheck.net/Nvidia-quietly-increases-Jetson-prices-by-up-to-101.1348323.0.html)
- [Local LLM hardware 2026](https://presenc.ai/research/local-llm-hardware-landscape-2026)
- [BLE Secure Connections MITM (USENIX 2023)](https://www.usenix.net/system/files/usenixsecurity23-shi-min.pdf)
- [Nordic: sniffing a paired connection](https://academy.nordicsemi.com/?p=44097)
- [Bluetooth SIG security](https://www.bluetooth.com/?p=65558)
- [esp_wireguard](https://components.espressif.com/components/trombik/esp_wireguard)
- [esphome wireguard](https://components.espressif.com/components/esphome/wireguard)
- [MicroLink (Tailscale on ESP32)](https://github.com/CamM2325/microlink)
- [esphome-tailscale](https://github.com/Csontikka/esphome-tailscale)
- [CoreS3 Tailscale test](https://lilting.ch/en/articles/m5stack-cores3-tailscale-direct-voice-server)
- [Apple Watch S11 long-term review](https://www.phonearena.com/reviews/apple-watch-series-11-review_id7826)
- [Galaxy Watch 8](https://en.wikipedia.org/wiki/Samsung_Galaxy_Watch_8)
- [Pixel Watch 4](https://en.wikipedia.org/wiki/Pixel_Watch_4)
- [Tom's Guide best smartwatches](https://www.tomsguide.com/best-picks/best-smartwatches)
- [Garmin Connect+ backlash](https://www.techradar.com/health-fitness/garmin-connect-plus)
- [Garmin Connect+ walkthrough](https://dcrainmaker.com/2025/03/garmin-connect-plus-subscription-walkthrough.html)
- [Gemini offline on Pixel Watch 5](https://9to5google.com/?p=725170)
- [Gemini on Wear OS is cloud-based](https://www.androidauthority.com/gemini-wear-os-offline-3656267/)
- [Clutch wearables survey](https://clutch.co/press-releases/wearable-technology-adoption-survey)
