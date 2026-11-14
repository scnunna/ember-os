# Assembly & Fit Check

## At the library, before you leave
- [ ] No stringy hairs blocking the screw holes, button holes, USB-C slot, TF-card slot
- [ ] Supports snap off cleanly. Inside walls not torn
- [ ] Got the **buttons** (tiny — they're the parts that get lost)
- [ ] Press the case on a flat table: it shouldn't rock (rocking = warped, back face won't seal)

## At home — dry fit first, battery OUT
1. Unplug USB. Remove the 2 back screws. Keep the stock shell — it's your fallback.
2. Put the printed buttons in their holes. Press each one: you should feel the board's button **click**. No click = button too short or too tight.
3. Lay the printed shell on the board, no battery. Screws should go in by hand-feel, no forcing. (The designer's 6 Jun 2026 update fixed tight screw heads — make sure your file is the latest.)
4. Check the USB-C cable plugs in fully with the case on.

## Battery fit
5. Do the **polarity check** in `02_BATTERY_AND_PARTS.md`. Then plug in.
6. The battery must sit flat. **Never press the shell down onto a battery to make it fit.** A squeezed LiPo can swell or catch fire.
7. Screw it shut. Power on. Check the battery reading shows up (AXP2101).

## Option B only — the speaker
The 900 case has no room for it. Designer says "wires can be cut at the PCB".
Better: **unplug it** if it has a connector. If you must cut, cut at the **speaker end** and tape each wire separately. That keeps the PCB end usable for the v1 case later.

## Log it
Tell me: did it fit, any button stiffness, total thickness (ruler). It goes in the mistakes / speed logs.
