# Ember Link v1: the watch ↔ phone protocol

Ember Link is the contract between the firmware (`firmware/main/link.c`) and the Android app (`companion/`). Both sides test against the vectors in `docs/link_vectors.json`.

## Transport
- **BLE GATT**: an extra primary service on the watch. The Chronos service stays for compatibility.
  | Role | UUID | Properties |
  |---|---|---|
  | Service | `eb3f0001-6d62-4f53-8a1e-656d6265720a` | |
  | CMD (phone → watch) | `eb3f0002-6d62-4f53-8a1e-656d6265720a` | Write, Write Without Response |
  | EVT (watch → phone) | `eb3f0003-6d62-4f53-8a1e-656d6265720a` | Notify |
  | AUDIO (watch → phone) | `eb3f0004-6d62-4f53-8a1e-656d6265720a` | Notify |
- Advertised name: `Watch-XXXX`, where XXXX is the last 2 bytes of the BT MAC in hex. The phone finds the watch by that name prefix, connects, discovers the service and subscribes to EVT and AUDIO.
- The phone should request **MTU 247** and **2M PHY** (Android: `requestMtu(247)`, `setPreferredPhy`). The watch accepts.
- **Wi-Fi** is only for big files. On request the watch starts its hotspot and sends the SSID and password over EVT. The phone joins with `WifiNetworkSpecifier` and fetches files over HTTP from `http://192.168.4.1`.

## Message framing (CMD and EVT)
Each message is one UTF-8 **JSON object** with a string field `t` (the type). A message is split into GATT packets of at most `MTU-3` bytes. Each packet starts with **one flag byte**:
- bit0 = FIRST: the first packet of a message
- bit1 = LAST: the last packet of a message
- a single-packet message uses flag `0x03`

The receiver concatenates payloads from a FIRST packet up to the LAST packet, then parses the JSON. Maximum message size is 4096 bytes. A FIRST packet throws away any half-built message. Unknown `t` values are ignored, as are unknown fields, so either side can add things later.

## Messages: phone → watch (CMD)
| `t` | Fields | Watch does |
|---|---|---|
| `time` | `epoch` (UTC seconds), `tz` (offset seconds, e.g. -18000) | Sets the RTC to local time = epoch + tz |
| `notif` | `id` (int), `app`, `title`, `body`, `replies` (up to 3 short strings) | Buzzes, shows a toast, stores up to 8 in the notification list; replies show as buttons |
| `notif_rm` | `id` | Removes it |
| `weather` | `temp` (°C int), `text`, `hi`, `lo` | Shows on the watch face |
| `steps_req` | — | Replies `steps` |
| `recs_req` | — | Replies `recs` |
| `wifi_req` | — | Starts the hotspot, replies `wifi` |
| `wifi_off` | — | Stops the hotspot |
| `note` | `id` (recording file name), `title`, `summary`, `actions` (array of strings) | Saves to `/flash/notes/` and shows it in the **Notes** app |
| `tr_cfg` | `me`, `them` (language codes such as `en`, `fr`) | Translate app labels |
| `tr` | `side` (`me`/`them`), `src` (heard text), `dst` (translated text) | Translate app shows `dst` in large type |
| `app_ls` | — | Replies `apps` |
| `app_put` | `name` (a-z0-9_-, ≤20), `size` | Begins an upload |
| `app_data` | `b64` (base64, ≤ 1536 decoded bytes) | Appends data |
| `app_end` | — | Installs the app, replies `app_ok` or `err` |
| `app_rm` | `name` | Removes it, replies `apps` |
| `find_watch` | — | Buzzes and beeps 3 times |
| `practice_req` | — | Replies `practice` |

## Messages: watch → phone (EVT)
| `t` | Fields | When |
|---|---|---|
| `hello` | `fw`, `name`, `batt` (%), `mv`, `usb` (bool), `steps` | On subscribe |
| `steps` | `days`: `[{"date":20261114,"steps":1234}, …]`, today first, up to 7 | On `steps_req`, and hourly while connected |
| `reply` | `id`, `idx` (which reply button) | The user tapped a quick reply |
| `dismiss` | `id` | The user dismissed a notification |
| `recs` | `files`: `[{"n":"M_20261114_093000.wav","s":bytes,"marks":[ms,…]}]` | On `recs_req` |
| `wifi` | `ssid`, `pass`, `ip`, or `err` | On `wifi_req` |
| `ptt` | `side` (`me`/`them`), `on` (bool), `rate` (16000), `codec` (`ima-adpcm`) | Push-to-talk starts or stops (audio flows on AUDIO between on and off) |
| `practice` | `sessions`: `[{"date":20261114,"start":"18:30","min":25,"tool":"metronome","bpm_max":120,"changes":0}]` | On `practice_req`, and after each session |
| `find_phone` | `on` | The user pressed Find phone |
| `music_cmd` | `cmd`: `toggle`,`next`,`prev`,`vol_up`,`vol_down` | Remote app |
| `apps` | `list` (names) | |
| `app_ok` | `name` | |
| `err` | `msg` | |

## AUDIO stream
Each AUDIO notification carries **exactly one IMA-ADPCM block**:
```
byte 0      : seq (u8, +1 per block, wraps)          -> the phone detects lost blocks
bytes 1..2  : predictor (int16 LE) = first sample of the block
byte 3      : step index (0..88)
byte 4      : reserved (0)
bytes 5..132: 128 bytes = 256 samples x 4 bits, LOW nibble first
```
That makes 133 bytes per block, 256 samples, 16 ms at 16 kHz, so 62.5 blocks/s and about 8.3 KB/s. Mono 16-bit at 16 kHz. Encoding is standard IMA ADPCM (the step table and index table from the IMA spec). Each block's header **resets** the decoder state, so a lost block costs only 16 ms of audio.

## Wi-Fi file transfer (existing server)
`GET /api/files` returns `[{"n":name,"s":size}]`. `GET /f/<name>` returns the WAV (16 kHz/44.1 kHz mono 16-bit). `POST /api/del?name=` deletes a file. Bookmarks for `X.wav` are in `recs` (`marks`, in ms from the start).

## Clarifications (v1.1, after the first phone-app build)
- **Ordering:** the watch accepts CMD writes as soon as the connection exists. The phone may send `time` and `steps_req` before `hello` arrives. The watch queues up to 32 messages and applies back-pressure (it delays the write response) instead of dropping.
- **`ptt` off** carries `blocks` = the number of AUDIO blocks sent. The phone can wait until it has seen that many, with a ~400 ms timeout for lost blocks.
- **`err`** carries `re` = the `t` of the message that failed (for example `app_put`).
- **`app_put.size`** is the raw byte count before base64. Write-with-response pacing is enough; no per-chunk acknowledgement is needed.
- **`notif.id`** is assigned by the phone; the same id replaces the earlier entry. `reply.idx` indexes `replies`.
- **Limits the watch keeps:** notif app 23, title 47, body 199 chars; up to 3 replies of 23 chars; the whole `note` message 4096 B; `tr.src` 199, `tr.dst` 239.
- **`steps.date`** is the local date as yyyymmdd; today's count is partial. `practice` sessions are unique by `date` + `start`.
- **`weather`** is optional. The phone app doesn't send it yet: there is no offline weather source, and the owner prefers no third-party servers.

## Security (v1.2, 2026-11-14): required, not optional
- **Pairing:** LE Secure Connections with bonding and MITM protection, method **Passkey Entry**. The watch shows a random 6-digit code; the user types it on the phone (Android's system pairing dialog). Legacy pairing is refused.
- **One phone only:** the watch keeps a single bond. A new phone can pair only when the user opens **Settings → Pair new phone** on the watch. That deletes the old bond and opens a 2-minute pairing window. Outside the window, pairing requests are rejected.
- **Every Ember Link characteristic needs an encrypted + authenticated link.** CMD writes and CCCD subscriptions (EVT, AUDIO) on an unencrypted link fail with *Insufficient Authentication* (0x05), which makes Android start pairing. The watch never notifies EVT or AUDIO unless the link is encrypted, authenticated and bonded.
- **Chronos service:** off by default (it has no authentication). Settings → *Chronos* turns it on, which takes effect after a reboot.
- **Confirm on the watch** (S3). These need a tap on the watch's *Allow* button within 30 s; otherwise the reply is `{"t":"err","re":<t>,"msg":"denied on watch"}`:
  - `app_end` (install)
  - `app_rm`
  - `wifi_req`
  Messages injected over the USB dev shell skip this, because USB means physical access.
- **Wi-Fi (S4):**
  - Every hotspot start gets a **new random 12-character password** and a **new 16-hex-character access token**.
  - `wifi` reply: `{"t":"wifi","ssid","pass","ip","token"}`.
  - Every HTTP endpoint needs `?k=<token>` (or the header `X-Ember-Key: <token>`); otherwise it returns 403.
  - The token is only ever sent over the encrypted link, or shown on the watch screen.
- **New message `rec_del`** `{"n": name, "s": size}`: the watch deletes the recording (and its .marks) only if the size matches. The phone sends it after a verified download. Reply: `recs`.

## Versioning
`hello.fw` is the firmware version. New fields and new `t` values are always optional. Removing or redefining a field needs a v2 service UUID.
