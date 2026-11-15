# Amber: personal agent on the watch (Phase 1 + 2)

**Amber** is the owner's personal agent. You talk to it from the watch. It knows your routine, keeps your schedule in
Life OS, checks in **every 4 hours** (08:00, 12:00, 16:00, 20:00, and 00:00 end of day) to ask whether you kept up and
what you actually did, and applies your spoken instructions to your notes and your JUNO dashboards (maps such as
"myPersonalDashboard", "Roadmaps", "Planning App", which live in Life OS table `juno_maps`).

Revision 2 (2026-11-15, owner): the 30-minute / per-block check-ins are replaced by fixed check-in slots. Voice talk
comes first.

```
Watch ──BLE (encrypted, bonded)──> Ember Companion (Pixel, NO internet)
  hold-to-talk audio (ADPCM)          speech-to-text on the phone (sherpa-onnx Whisper)
  check-in answers ("ci")             │ IPC (signature permission)
  shows Amber's replies              Ember Gateway (Pixel, only app with INTERNET; tailnet host only)
                                      │ https over the owner's tailnet
                                   Life OS on the owner's server (https://<host>.<tailnet>.ts.net:<port>)
                                      /api/watch/*  →  Amber worker (Claude CLI on the VPS: Haiku/Sonnet/Opus by task)
```

Life OS is the owner's private app (separate private repo); only its watch API contract is described here.

Privacy: Life OS is the owner's own server on the tailnet. Audio never leaves the phone (only text does). The text
of instructions and check-ins goes to Claude (Anthropic) through the VPS Claude CLI. That is the standing "L1" approval
for Amber: schedule and task text, notes. Raw audio, health data and Rolodex notes are not sent unless the owner asks.

---

## Part A: Life OS (`server/watch.mjs`, `server/amber.mjs`, next free migration number, currently `0022_watch_amber.sql`)

### Tables
- `watch_devices`: `id uuid pk`, `user_id uuid default auth.uid()`, `name text (1..40)`, `token_hash text unique`
  (sha256 hex), `created_at`, `last_seen_at`, `revoked_at`. Token `lw_` + 32 lowercase hex, shown once in Settings.
- `watch_checkins`: `id`, `user_id`, `device_id → watch_devices on delete cascade`, `slot_date date`, `slot smallint`
  (minutes after local midnight: 480, 720, 960, 1200, 1440 = end of day), `answer text in ('on','off','snooze','missed')`,
  `at timestamptz`, `item_ids uuid[]` (the planned blocks of that window, from the watch), `note text` (what the owner
  said they actually did, filled by Amber), `created_at`. **Unique (device_id, slot_date, slot, at)**. RLS + 4 owner
  policies + revoke anon.
- `amber_messages`: `id uuid`, `user_id`, `device_id`, `dir text in ('in','out')`, `text text (1..4000)`,
  `ctx jsonb` (for example `{"ci_seq":17,"slot":720,"date":"2026-11-15"}`), `model text`, `status text in
  ('queued','working','done','error')`, `reply_to uuid`, `created_at`, `delivered_at`. Owner RLS as usual.
- `amber_actions`: every change Amber applied: `id`, `user_id`, `message_id`, `op text`, `target text`, `before jsonb`,
  `after jsonb`, `applied_at`, `undone_at`. This is the audit trail and the undo list.

### Auth for every `/api/watch/*` route
`Authorization: Bearer lw_<32hex>` → sha256 → a non-revoked device → service role, **always filtered by that device's
user_id**. Updates `last_seen_at`. Bad or missing token: 401 with no detail. 60 requests/min per device.

### `GET /api/watch/today?date=YYYY-MM-DD&tz=<minutes east of UTC, e.g. -300>`
The day's time blocks: the owner's `action_items` with `scheduled_start` inside that local day and `scheduled_end`
not null, sorted, at most 24.
```json
{"date":"2026-11-15","gen":"9f2c1a7b","slots":[480,720,960,1200,1440],
 "blocks":[{"id":"<uuid>","s":540,"e":600,"title":"WFF deep work","done":false}]}
```
`s`/`e` are minutes after local midnight (cut at 1440). `title` is cut to 39 characters (on characters). `gen` is the
first 8 hex characters of the sha256 of the canonical JSON of blocks + slots. `slots` comes from the owner's setting
(default above; Settings → Watch lets the owner change it).

### `POST /api/watch/checkins`
```json
{"items":[{"seq":17,"date":20261115,"slot":720,"at":1763226000,"ans":"on","ids":["<uuid>"]}]}
```
≤ 64 items. `at` is UTC epoch seconds. Insert, "on conflict do nothing". Reply `{"ok":true,"stored":n,"upto":<max seq>}`.

### `POST /api/watch/amber` (talk to Amber)
```json
{"text":"move gym to 6 pm and add 'call the bank' to tomorrow","at":1763226000,"ctx":{"ci_seq":17,"slot":720,"date":"2026-11-15"}}
```
- Stores an `in` message, replies at once `{"ok":true,"id":"<uuid>","ack":"Got it."}`, then the Amber worker runs
  asynchronously.
- When the worker finishes, it stores an `out` message (short reply, ≤ 300 characters, for the watch screen).
- If `ctx.ci_seq` or `ctx.slot` is set, the text is the owner's answer to that check-in ("what I actually did"). Amber
  saves it in `watch_checkins.note` and reconciles the plan: it marks the stories that were done, and logs what really
  happened.

### `GET /api/watch/inbox?after=<iso or empty>`
Amber's `out` messages not yet delivered (≤ 20, oldest first): `{"items":[{"id","text","reply_to","created_at"}]}`.
The phone acknowledges with `POST /api/watch/inbox/ack {"ids":[...]}`, which sets `delivered_at`. Check-in prompts
can also carry an Amber summary (for example at 08:00, "Today: 3 blocks, first WFF deep work 09:00").

### The Amber worker (`server/amber.mjs`)
- Uses the VPS Claude CLI the same way `server/ai.mjs` does: no API keys, untrusted text fenced, timeouts, and a daily
  cost cap `AMBER_CAP_DAY_USD` (default 3).
- **Model choice by task**, decided by code first, with no LLM call for routing:
  - Haiku: short, single edits (add a task, move a block, add to a note, log a check-in answer).
  - Sonnet: the default (several edits, a summary, reconciling a check-in).
  - Opus: planning or reasoning about the owner's life ("plan my week", "rebuild my routine", "what should I focus on",
    profile interviews).
  - The rule table lives in code, and every `amber_messages.model` records the model used.
- **Context it reads**: today's and tomorrow's blocks, open stories, the latest check-ins, the page "Amber profile",
  and the JUNO maps named in the instruction.
- **What it may change, applied directly** (the owner's own spoken instruction, owner decision 2026-11-15; this is an
  explicit exception to the "AI only proposes" rule and must be written into lifeos/CLAUDE.md):
  - create or update stories, including time blocks (`scheduled_start`/`end`, `planned_for`, status/done);
  - create a page or append to one (notes);
  - edit JUNO map nodes (add, rename or move a node, set a node's note), through the existing LWW path (`juno_put_map`),
    so the JUNO app syncs and a snapshot is kept;
  - set `watch_checkins.note`.
  - Every change is written to `amber_actions` with before/after values.
- **Never applied directly**: deleting anything, people or Rolodex data, and anything outside Life OS (email, posts,
  money). These become normal `proposals` (approve at `/approvals`), and the reply says so.
- **"Amber profile"** (`pages`, title "Amber profile"): routine, wake and sleep times, work blocks, gym, goals. If it is
  missing or thin, Amber's first replies ask one short question at a time to fill it in (the onboarding interview), and
  it keeps the page updated.

### UI
- Settings → Watch: devices (name, last seen), "Add watch link" (token shown once + copy), Revoke, and the check-in
  times (default 08:00 12:00 16:00 20:00 00:00).
- Today page: a "Check-ins" card listing today's slots with answer + note, the adherence % = on / (on + off + missed),
  and a 7-day row.
- `/amber`: the conversation (in and out messages) with the actions applied under each reply, each with an **Undo**
  button (restores `before`, sets `undone_at`).

---

## Part B: Android (Gateway + Companion)

### Gateway
- Setting "Life OS link": base URL + device token. The URL must be `https://`, with a host ending in `.ts.net`, an
  optional port, and no user info, path, query or fragment. The token must match `^lw_[0-9a-f]{32}$`. Both are stored
  encrypted.
- `RequestKind.LIFEOS` in `GatewayPolicy`: Auto (logged) only for these routes to the configured host:port; all else
  is denied, and redirects are refused.
  - `GET /api/watch/today`
  - `POST /api/watch/checkins`
  - `POST /api/watch/amber`
  - `GET /api/watch/inbox`
  - `POST /api/watch/inbox/ack`
- IPC: `MSG_LIFEOS_TODAY`, `MSG_LIFEOS_CHECKINS`, `MSG_LIFEOS_AMBER`, `MSG_LIFEOS_INBOX`, `MSG_LIFEOS_INBOX_ACK`,
  `MSG_LIFEOS_STATUS`. Errors: `NOT_CONFIGURED`, `HTTP_ERROR` (with status), `OFFLINE`.

### Companion
- **Plan sync:** after `hello`, every 15 minutes while connected, and after local midnight, fetch today's plan. Send
  `sched` if `gen` changed or `hello.sd` ≠ today. `sched` carries `slots` too.
- **Check-ins:** on `ci`, persist it to the encrypted queue first, then send `ci_ack{upto}`, then forward batches (≤ 64)
  with retry and backoff (keep ≤ 500).
- **Talk to Amber:** the watch sends `ptt` with `side:"amber"` (plus optional `ctx`) and streams audio as usual. The
  phone transcribes it on the phone (the same Whisper path as Translate), shows the text, sends
  `{"t":"amber_heard","text":...}` to the watch (so you see what it understood), then `POST /api/watch/amber`. It sends
  the `ack` to the watch as `{"t":"amber","text":"Got it.","id":...,"final":false}`.
- **Amber's replies:** poll `GET /api/watch/inbox` every 20 s for 3 minutes after a message was sent, then every
  5 minutes while connected. Each item goes to the watch as `{"t":"amber","id","text","final":true}`, then is
  acknowledged.
- A typed fallback on the phone: an "Amber" screen with the conversation and a text box (it uses the same path).
- Home: a "Now" line and today's adherence. Settings: the Life OS link status and a button that opens Gateway.

---

## Part C: Ember Link v1.4 (watch)

### phone → watch
| `t` | Fields | Watch does |
|---|---|---|
| `sched` | `date` (yyyymmdd), `gen` (≤ 8), `slots` (minutes, ≤ 8, default `[480,720,960,1200,1440]`), `blocks` `[{"id","s","e","title","done"}]` (≤ 24) | Stores the day's plan in `/flash/sched.bin` (the queue in `/flash/ciq.bin`; NVS is too small). Replies `sched_ok {date, gen, n}` |
| `sched.brief` (optional) | Amber's morning summary, ≤ 200 characters (from `GET /api/watch/today` field `brief`, written by Life OS at 07:55 by code, no LLM) | Shown in the first check-in of the day (08:00) instead of the block list. The phone folds it into `gen`, so a new brief re-sends the plan |
| `ci_ack` | `upto` | Drops queued check-ins with seq ≤ upto |
| `ci_cfg` | `on` (bool), `snooze` (5..60 min) | Saves them (default on, snooze 15) |
| `amber_heard` | `text` (≤ 300) | Shows "You said: …" in the Amber screen |
| `amber` | `id`, `text` (≤ 300), `final` (bool) | Shows the reply; a final reply buzzes once and wakes the screen; keeps the last 10 |

### watch → phone
| `t` | Fields | When |
|---|---|---|
| `hello` (new) | `sd` (date of the stored plan, 0 = none), `ciq` (queued) | as before |
| `ci` | `seq` (u32, increasing, kept in NVS), `date` (yyyymmdd the slot belongs to), `slot` (minutes), `at` (UTC epoch s), `ans` (`on`/`off`/`snooze`/`missed`), `ids` (block ids planned in the window since the previous slot) | each answer; re-sent after `hello` until acknowledged |
| `ptt` (new value) | `side:"amber"`, optional `ctx` `{"ci_seq":n,"slot":m,"date":d}` | hold-to-talk to Amber |
| `sched_ok` | `date`, `gen`, `n` | after `sched` |

### Watch behaviour
- **Check-in slots:** at each slot time, if the stored plan is that day's (the 1440 slot is asked at 00:00 for the
  day that just ended, so the watch keeps yesterday's plan until that is done).
  - Buzz twice, wake, and show "12:00 check-in". It lists what was planned since the previous slot (or "Nothing
    planned") and has three buttons:
    - **On track**
    - **Off track**
    - **Snooze** (asks again after `snooze` minutes, once)
  - Below them is a **Hold to tell Amber** button (voice: "what I actually did"; it sends `ptt side:amber` with the
    `ctx` of this check-in).
  - After Off track, the watch offers Hold to tell Amber straight away.
  - No answer in 5 minutes → `missed`.
  - A slot the watch slept through (more than 10 minutes late) is not asked; it is sent as `missed`.
- **Talk to Amber anytime:** the **Amber** app (or the mic button on the left of the watch face; a double press still toggles a meeting recording). It shows a big
  "Hold to talk" button, "You said: …" and the last replies.
- **Watch face:** "Now: <title> · until 11:00" or "Next: <title> 14:00", and the next check-in time.
- **Deep sleep:** wakes on a timer for the next slot.
- USB shell (tests): `sched` prints the plan, slots, queue and next check-in; `citest <minute>` pretends the clock
  reads that minute (of the plan's day; 1440 = the end-of-day slot).
