# Power Budget — 18 h day: step tracking + 4 h meeting recording

Written 2026-10-02. **Every number here is an ESTIMATE from datasheets. None of it is measured yet.** Replace these with real readings (see "Test plan").

## The day
| Mode | Hours | What's on | Current at battery (est.) | mAh |
|---|---|---|---|---|
| Recording | 4 | ESP32 at 80 MHz (no Wi-Fi/BLE), ES7210 + 2 mics, SD writes | 25–60 mA (likely ~42) | 100–240 (likely ~170) |
| Step tracking / idle | ~14 | ESP32 deep sleep, QMI8658 pedometer counting by itself, screen off | 0.5–2 mA | 7–28 |
| Glances | ~100 × 6 s | wake, show HH:MM, off after 5 s | ~60 mA | 6–15 |
| Sync steps to phone (BLE) | short | | | 2–5 |
| **Total** | | | | **~115 – 290 (likely ~200)** |

Upload the audio over Wi-Fi **while it's on the charger at night**, so it costs the battery nothing.

## Usable capacity
| Battery | New (≈85% of the label) | After ~1 year (≈70%) | Likely-day margin |
|---|---|---|---|
| 400mAh | ~340 | ~280 | **1.7×** new · 1.4× aged · ❌ worst case + aged fails |
| 900mAh | ~765 | ~630 | **3.8×** · survives the worst case |

## Where each number comes from
- ESP32-S3: deep sleep 7 µA, light sleep 240 µA, modem-sleep 14–20 mA at 160–240 MHz (Espressif datasheet)
- QMI8658C: built-in pedometer; accelerometer-only low-power 30 µA (QST datasheet)
- ES7210 mic ADC: 63 mW at 16 kHz (datasheet, probably all 4 channels), 10 µA powered down
- Schematic: **QMI_INT1 → GPIO21** (an RTC pin, so motion and steps can wake the chip from deep sleep). Both mics go to the ES7210, so recording needs it. The SD card runs in SPI mode and looks like it's on the always-on 3.3 V rail (idle current unknown). There's a `MOTOR` footprint (vibration). Not checked whether it's fitted.

## The 2 swing factors
1. **Recording current** is ~80% of the day's energy. Measure it first.
2. **SD-card idle current** (can't be switched off, varies 0.1–1.5 mA by brand). Over 14 h that's anywhere from 2 to 20 mAh.

## Recording settings that cut power
16 kHz, 16-bit, **mono** (one mic, other ES7210 channels powered down) · CPU at 80 MHz or lower · Wi-Fi/BLE fully off · buffer ~64 KB in PSRAM, then write to SD in big chunks · screen off · 4 h ≈ 460 MB, so any 8 GB+ card is fine.

## Test plan (≈2 days once a battery arrives)
1. Charge to 100%. Record for 1 h. Read the AXP2101 battery % and voltage. → mAh per hour of recording
2. Charge to 100%. Leave it in step-tracking sleep for 8 h overnight. Read the % again. → idle mA
3. Plug both into the table above. **If the likely total is under ~300 mAh on the 400, keep the watch.**
