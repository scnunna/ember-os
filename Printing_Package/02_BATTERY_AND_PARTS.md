# Battery & Parts

## Battery options

| | v0 (recommended) | Option B (kills speaker) |
|---|---|---|
| Size code | `402728` → 4 × 27 × 28 mm | `803035` → 8 × 30 × 36 mm |
| Capacity | 400mAh | 900mAh |
| Source | Waveshare's own spec (wiki: "4×27×28 400mAh") | [HXJNLDC on Amazon, B0D4V8XBXT](https://www.amazon.com/dp/B0D4V8XBXT) — the designer's exact battery |
| Connector | must be **MX1.25, 2-pin** | "JST 1.25mm 2-wire" — designer confirms it plugs straight in |
| Case | stock (no print) | MakerWorld 900mAh case |
| Price seen | ~$10–15 | US$15 + US$8.86 ship to Canada |
| Max current | ~0.4 A typical | 0.9 A |

**How to read the size code:** `TTWWLL` = thickness, width, length in mm. `402728` = 4.0 mm thick, 27 wide, 28 long. Search Amazon.ca for the code + "MX1.25". If the listing shows a different size than the code, trust neither — ask me.

**Why the code matters more than mAh:** sellers inflate mAh. Size can't be faked. A `402728` that claims 600mAh is lying.

---

## 🚨 Polarity check — do this BEFORE plugging in (2 min)

"1.25 mm" plugs fit both ways round between vendors. The seller's own listing warns polarity varies. Reverse it and you can kill the power chip (AXP2101) instantly.

**Why:** a battery is a one-way water pipe. Hook it up backwards and the pump runs in reverse.

1. Multimeter dial → **DC V, 20 V** range (the `V` with a straight line, not the wavy `~`).
2. Red probe → the metal pin at the end of the **red** wire. Black probe → the **black** wire's pin.
3. Read it:
   - `+3.6` to `+4.2` → battery is fine, wires are what they say.
   - `-3.7` (minus sign) → wires are swapped inside the plug. **Stop.**
   - `0` or under `3.0` → dead or deeply discharged. **Stop.**
4. On the watch board, find the battery socket. Look for `+` / `BAT+` / `-` printed next to it. Red must land on `+`.
5. No marking you can read? **Send me a photo before you plug in.** Don't guess.

This is also your first multimeter use — low risk, good practice.

---

## Other parts
| Item | Have it? | Notes |
|---|---|---|
| Stock back shell, 2 screws, strap | ✅ comes with the watch | The 900 case reuses all of these |
| Small Phillips screwdriver (PH00) | ? | For the 2 back screws |
| Kapton or electrical tape | ? | Insulate any cut wire ends |
| Calipers | optional | Only needed for the v1 custom case (a ruler + photo works too) |
