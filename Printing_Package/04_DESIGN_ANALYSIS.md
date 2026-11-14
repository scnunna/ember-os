# Design Analysis — MakerWorld 900mAh case (model 2763002)

## What the design is
- Designer **bjwest9738**. Released 5 May 2026. Updated 6 Jun 2026 (screw holes opened up).
- Licence **CC BY-NC-SA**: personal use and remixes are fine. Can't sell it.
- One printed back shell (~19 g PLA) + separate push buttons. Reuses the stock screws, strap and front.
- About **4.5 mm taller** than stock. Stock watch is 13.6 mm thick → this one is **~18 mm**. (A Garmin Fenix is ~15 mm, so it's chunky but wearable.)
- **Speaker removed.** The 36 mm battery takes its space.

## Is it "exact"?
| Check | Verdict | Evidence |
|---|---|---|
| Battery fits | ✅ Good | The designer built it around this exact battery (36×30×8, connector fits, no splicing) |
| Screws | ✅ Fixed | 6 Jun update. One user: "screws fit perfectly with latest update" |
| Overall fit | ✅ Good, small sample | 3 users posted prints, all 0.2 mm / 4 walls / 15 %. One said "really nice fit". 5.0★ from 3 ratings |
| Your printer | ⚠️ **Unknown** | Every posted print used a Bambu profile. TPL runs **PrusaSlicer**. Different printer = fit can shift by ±0.1–0.2 mm |
| Thin walls | ⚠️ Watch | Designer: **Arachne is required**. Some walls are too thin for "classic" mode and will just disappear |
| Runtime | ❓ Untested | A 26 Sep 2026 comment asks how long it lasts. No answer yet |

**Plain version:** it's a proven design with a small fan club. The real risks are (1) a different printer and (2) losing the speaker.

## Tolerances you should expect
- FDM printers are about **±0.1–0.2 mm** sideways and one layer (0.2 mm) up/down.
- **Where it matters:** screw holes (fixed), button holes (tiny — most likely to be stiff), the 22 mm strap slot, and the battery pocket.
- **Why buttons are the risk:** a 3 mm button with 0.15 mm extra on every side gets stiff. A 40 mm case with 0.15 mm extra doesn't notice. → Print 2 extra sets of buttons. They cost cents.

## Runtime estimate (scaled from Waveshare's own 400mAh numbers)
| Battery | Screen full bright | Screen off, Wi-Fi on | With proper sleep code |
|---|---|---|---|
| 400mAh (v0) | ~1 h | ~3–4 h | likely a day+ (needs measuring) |
| ~600mAh (v1) | ~1.5 h | ~4.5–6 h | 1.5× the above |
| 900mAh (B) | ~2.25 h | ~7–9 h | 2.25× the above |

Straight maths from Waveshare's wiki figures, not measured. The "sleep code" column is the big unknown and the biggest win. **AMOLED tip:** black pixels are *off* on AMOLED and use almost no power. A black watch face is free battery life.

## Keeping the speaker — options ranked

1. **v0: stock case + 400mAh.** Zero print. Zero risk. Measure real runtime with good firmware first. ⭐ **Do this.**
2. **v1: "+2 mm" remix, 6 mm battery, same ~27×28 footprint.** The battery only grows in *thickness*, so it should stay clear of the speaker. (That assumes the speaker sits *beside* the battery, not on top of it. The photos will confirm it.) Expect ~550–650mAh and a ~15.5 mm watch. Needs: you open the stock case and send me photos with a ruler (or calipers) showing the battery bay + speaker. I then build the remix and check it numerically before you print.
3. ~~Move the speaker in the 900 case~~ — the battery fills the full height and width. There's nowhere to put it.
4. ~~Bluetooth earbud instead of the speaker~~ — the ESP32-S3 has BLE only, no classic Bluetooth audio. Doesn't work.

## What I could NOT verify (flagged honestly)
- Inside cavity size and speaker size: Waveshare publishes no 3D/case files. The 900 STL needs a login I can't do.
- Whether the "-EN" (no battery) box has a spare battery pocket foam/sticker.
- Battery socket polarity marking on the board — check it yourself (see `02`).

## Sources
- MakerWorld model page + 8 reviews: https://makerworld.com/en/models/2763002-waveshare-esp32-s3-amoled-2-06-900mah-case
- Battery: https://www.amazon.com/dp/B0D4V8XBXT
- Waveshare wiki (battery spec, runtime, AXP2101, 46 °C test): https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06
- Outline drawing (42 × 50.8 × 13.6 mm): https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm
- TPL 3D printing rules: https://tpl.ca/using-the-library/computer-services/digital-innovation-services/3d-printers/
