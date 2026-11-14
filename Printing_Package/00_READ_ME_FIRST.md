# Waveshare ESP32-S3-Touch-AMOLED-2.06 — Battery & Case Package

Researched 2026-10-02. Board = the **-EN "No Battery"** version (you must buy a battery either way).

---

## TL;DR — what to do

| Step | What | Print? | Speaker? | Battery |
|---|---|---|---|---|
| **v0 (do this first)** | Stock case + stock-size battery | ❌ no print | ✅ kept | 400mAh `402728` |
| v1 (only if v0 runtime is too short) | Custom "+2 mm" case I design for you | ✅ | ✅ kept | ~550–650mAh, 6 mm thick |
| ~~Option B~~ | MakerWorld 900mAh case | ✅ | ❌ **removed** | 900mAh `803035` |

**Why v0 first:** the battery is a bucket and the firmware is the hole in it. A 900 bucket is only 2.25× bigger. Good sleep code (screen off, Wi-Fi off, light sleep) makes the hole 10–50× smaller. Fix the hole first, then decide if you still need a bigger bucket.

**Why not the 900 case:** it fails your #1 requirement. The designer says the speaker "must be removed". No speaker = no 2-way talk, no sound alerts.

**Why not jump to 600:** no 600 case exists anywhere (checked MakerWorld, Printables, Thingiverse). It needs a custom design = more failure points for a POC.

---

## What's in this folder

| File | For whom |
|---|---|
| `01_LIBRARY_PRINT_SHEET.pdf` / `.html` | Hand to TPL staff. Settings for PrusaSlicer. |
| `02_BATTERY_AND_PARTS.md` | What to buy + the **polarity check** (don't skip) |
| `03_ASSEMBLY_AND_FIT_CHECK.md` | After the print: fit checklist |
| `04_DESIGN_ANALYSIS.md` | The full analysis behind these recommendations |
| `STL/` | **Empty** — you must download the files (login needed, I can't do that for you) |

## Before you go to the library (only if you do print the 900 case)
1. Make a free MakerWorld account → open the [model page](https://makerworld.com/en/models/2763002-waveshare-esp32-s3-amoled-2-06-900mah-case) → **Download → STL** *and* the **3MF**.
2. Drop both into `STL\`. Tell me — I'll measure the files (cavity size, wall thickness, weight) before you spend money. Renders lie; numbers don't.
3. Copy `STL\` + the print sheet PDF to a USB stick.
4. Call your TPL Digital Innovation Hub to book. Ask for the **longer-print printer** (normal slots are capped at 2 h incl. setup).
