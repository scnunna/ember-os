# Who does what: watch, Pixel 8, laptop, VPS (2026-11-14)

Rule of thumb: **do each job on the closest device that does it well enough**. Closer means faster, more private and works offline. Only send work further away when it clearly improves the result.

## The four devices
| Device | Strengths | Limits | Role |
|---|---|---|---|
| **Watch** (ESP32-S3) | Always on you, buzz, buttons, mic, ~1 s boot, sleeps well | ~90 KB of free internal RAM, no AI, tiny speaker | **Senses and signals:** hold-to-talk capture, check-in cards, buzz, an offline copy of today's schedule |
| **Pixel 8** (Tensor G3, 8 GB) | Always within 10 m, decent CPU/GPU, battery, offline | Small models only; real speed unknown until the bench runs | **Everyday brain:** speech-to-text, the check-in engine, quick commands, translation, notification triage, the security gate |
| **Laptop** (RTX 4060, ~8 GB VRAM) | 10–50× the phone's AI power | Only at home (or over your VPN); often busy with renders | **Home brain:** better meeting transcripts, speaker labels, smarter answers, the nightly weekly review |
| **VPS** (your rented server) | Always online | A third-party data centre, no GPU | **Only if you approve:** VPN coordination and encrypted backups. **Never raw audio or plain text** |

## Job by job
| Job | Runs on | Fallback | Why |
|---|---|---|---|
| Half-hour check-in, buzz, Yes/Switched/Snooze | **Watch** (schedule cached each morning) + phone alarm | — | Works even if the phone is out of range for a while |
| Capturing your voice | **Watch** (hold-to-talk, ADPCM over BLE) | Phone mic | Built and tested |
| Speech-to-text for commands | **Phone** (whisper-tiny/base, sherpa-onnx) | Laptop at home | Short clips; fast enough on a phone CPU (bench will confirm) |
| "Move gym to 5", "what's next" | **Phone**: rule-based parser first, small LLM only if needed | — | Rules are instant and battery-cheap; the LLM handles odd phrasing |
| Free-form questions or coaching | **Phone** (Gemma 4 E2B, 2.6 GB) | **Laptop** model when at home | Phone for privacy and away from home; laptop for quality |
| Meeting notes | **Phone** draft right away (whisper-base) → **laptop** overnight re-transcribe (large-v3-turbo + speaker labels) + better summary | Phone only | Fast draft now, best version by morning |
| Travel translation | **Phone** (Gemma 4 E2B, offline) | — | Abroad there's no laptop; the phone app has no internet by design |
| Weekly review, habit insights | **Laptop** (nightly) | Phone (simpler) | Heavy, not urgent |
| Backups | **Laptop** (encrypted) | **VPS**, only with client-side encryption (restic or age; keys never leave your devices) | 3-2-1 rule without trusting the server |

## How they connect
- **Watch ↔ phone:** bonded, encrypted BLE (done, S1). Wi-Fi hotspot only for big files, with a new password and key each time (S4).
- **Phone ↔ laptop at home:** home Wi-Fi. Only the **Ember Gateway** app talks to the network. The laptop gets *pinned* once (key exchange, e.g. by QR code) and is then auto-allowed; anything else needs your tap.
- **Phone ↔ laptop away from home**, two options:
  1. **Your existing Tailscale tailnet.** It works today. Traffic is encrypted end to end; Tailscale's servers only coordinate keys.
  2. **Headscale on the VPS.** Same, but you run the coordinator. The VPS never sees content, only encrypted packets.
  - **Not recommended:** a plain WireGuard hub on the VPS, because the hub would decrypt the traffic.
- **Laptop GPU sharing:** assistant jobs go in a queue. Short answers interrupt nothing; transcripts run when no render is active, or overnight.

## Pixel 8: what to expect and how to decide
- **The bench screen** (Ember Companion → Settings → "What can my phone run?") measures:
  - speech: whisper-tiny/base real-time factor
  - Gemma 4 E2B: tokens/s on CPU and GPU, and first-token time
  - text-to-speech latency

  Run it once and paste me the results.
- **Decision rule (Gemma decode speed):**
  | Speed | Verdict |
  |---|---|
  | **≥10 tok/s** | The phone is enough |
  | **4–10 tok/s** | OK for short answers; use the laptop for long ones |
  | **<4 tok/s** | Rules + laptop; consider a phone upgrade |
- **Upgrade warning (unverified, worth checking first):** a 2026 benchmark source says Pixel phones run LiteRT-LM on the **CPU**, because Google's TPU has no public LiteRT plugin. Google's own figure of ~52 tok/s is for a Snapdragon Galaxy S26 Ultra GPU. So for *this* local-AI stack, a **Snapdragon flagship may beat a Pixel 10**. Check current numbers before buying.

## What I'd set up next
1. Run the bench on your Pixel 8 (needs the app installed; 2.6 GB model over Wi-Fi).
2. "Home brain" service on the laptop. It needs owner approval because it installs software, e.g. a local LLM server plus faster-whisper, both already in your AI Library. It should answer only the pinned phone.
3. Decide on away-from-home access: existing tailnet (now) or Headscale on the VPS (stricter). Any VPS change needs your go-ahead, because it also runs your finance services.
