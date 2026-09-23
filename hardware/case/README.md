> **English** | [中文](README.zh-CN.md)

# BLEhound case (Design A) · Printing & assembly notes

Outer dimensions 75 × 65 × 19.6 mm, in two parts: tray + lid. Four M3 screws go in
from the bottom; the top face has only 4 LED holes. The parting line sits on the
SMA-KWE thread axis (6.0 mm above the board face), so each of the three SMA holes is
a half-circle split top and bottom; USB-C exits through a 9.8 × 4.0 cutout in the
rear wall.

![Exploded assembly of the BLEhound case: lid, PCB, tray](case_exploded.gif)

*Exploded view (OpenSCAD render): the lid on top with the embossed logo and LED holes, the PCB in the middle, and the tray with the USB-C cutout below. For a draggable 3D view open [`BLEhound_case_A_3D.html`](BLEhound_case_A_3D.html).*

## 1. Files in this directory

| File | Purpose |
|---|---|
| `blehound_case_P2S_PLA_0.2mm.gcode.3mf` | **Print directly** (Bambu P2S, PLA). Both parts on one plate, ~62 min, ~34 g |
| `blehound_case_P2S_PETG_0.2mm.gcode.3mf` | **Print directly** (Bambu P2S, PETG). ~65 min, ~36 g |
| `blehound_tray.stl` / `blehound_tray.3mf` | Tray model, in print orientation (opening up), 75 × 65 × 13.6 |
| `blehound_lid.stl` / `blehound_lid.3mf` | Lid model, already flipped (top face on the bed, bosses up), 75 × 65 × 12.0 |
| `blehound_case_A.scad` | OpenSCAD parametric source — change parameters and re-export the models |
| `preview_*.png` | OpenSCAD renders: assembled / exploded / tray / lid print orientation |
| `case_exploded.gif` | Exploded-assembly animation shown above |
| `BLEhound_case_A_3D.html` | Offline version of the design page: 3D exploded animation, drawings, review checklist (the 3D view fetches three.js online) |
| `README.md` | This file |

Slicing settings (for both `.gcode.3mf`): P2S 0.4 nozzle, the official "0.20mm
Standard" preset + 3 wall loops, 4 top/bottom layers, 20% infill, no supports,
plate type = textured PEI plate.

## 2. Printing on the Bambu P2S (no PC software needed)

1. Pick a file matching the filament loaded in the machine (PLA or PETG) and copy it
   to the root of the printer's storage card (on the P series that is the microSD on
   the side of the machine; if it has a USB port, use a USB stick instead).
2. Confirm the machine has the **textured PEI plate** installed (the matte black
   one). If you have an AMS, set the matching slot's material to PLA / PETG.
3. Insert the card → touchscreen "Files" → storage card → select the file (it shows
   the thumbnail of both parts) → Next → tick "Bed leveling" → Start.
4. The machine preheats, levels, and starts printing on its own. Glance at the first
   two layers (about 2 minutes) for warping, then you can leave.
5. When it finishes, wait for the bed to drop below 40 °C, then **take the plate out
   and flex it** — the parts pop off by themselves; don't pry them off with a
   scraper.

Prompts you might see:
- "Plate type mismatch": the machine isn't on the textured PEI plate. Swap the
  plate; or tap continue (printing PLA on the smooth Cool Plate needs a glue stick
  first).
- "Filament mismatch": it isn't Bambu PLA Basic. For other-brand PLA just tap
  continue — the temperatures are universal (220 / 55 °C).
- PETG: apply a thin coat of glue stick on the plate first, otherwise PETG sticks
  too hard to the textured plate.
- When printing PLA, open the front door or remove the top glass (a closed chamber
  gets too hot and softens PLA); print PETG with the door closed.

## 3. Hardware

| Item | Spec | Qty |
|---|---|---|
| Flat-head (countersunk) screw | M3 × 12 (14 max) | 4 |
| Heat-set insert | M3 × 4 × 5 (OD 4.6), pilot hole Ø4.0 | 4 |
| Silicone foot pad | Ø8 × 1.5 | 4 |
| SMA nut + spring washer | comes with the connector, optional | 3 |

## 4. Assembly order

1. Press a heat-set insert into the base of each of the lid's 4 bosses: with the
   soldering iron at 220–250 °C, push the insert straight down into the Ø4.0 hole
   until it is flush with the boss face.
2. Without the board in yet, fit the lid onto the tray to check the fit (the lid's
   inner rim reaches 1.5 mm into the tray).
3. Put the PCB into the tray: **tilt the USB end in first** so the USB-C receptacle
   goes into the rear-wall cutout (the receptacle only enters the wall by 0.9 mm, a
   5° tilt is enough), then lower the antenna end so the 3 SMA threads drop into the
   lower half-circles of the front wall.
4. Put the lid on, close up the upper half-circles, and align the bosses with the
   board holes.
5. Flip the whole thing over and drive the 4 M3 × 12 screws in from the bottom
   (don't use screws longer than 14 mm — they'll punch through the lid). Stick on the
   foot pads, positioned inboard of the four corners so they don't sit on the screws.
6. Tighten the SMA nuts (on the outside of the wall, which also clamps the two shells
   together) and fit the antennas. Outside the wall there is 5.0 mm of thread
   available; after the 2.2 nut, 2.8 remains for the antenna's inner nut.

The buttons (BOOT / RESET) and the SWD header are inside the case; when you need
them, undo the 4 screws and lift the lid.

## 5. Parameters you may need to tune after a trial fit (edit the top of `blehound_case_A.scad`)

| Symptom | Parameter | Default | Suggestion |
|---|---|---|---|
| Board won't go in / too loose | `clr` | 0.5 | ±0.2 |
| Lid won't snap in / too loose | `lip_clr` | 0.2 | 0.3 / 0.1 |
| SMA thread sits too high or too low in the half-circle | `sma_axis_z` | 6.0 | after soldering, measure the thread center to the board face with calipers (the Ø7.4 hole leaves ±0.5) |
| USB-C cable won't seat fully | `usb_cut_w` / `usb_cut_h` | 9.8 / 4.0 | +0.5 each |
| Want to fit ANT2 later (J2/J5/J8) | `ant2_slots` | false | true, reprint both parts |
| Want BOOT/RESET pin holes on the top face | `pin_holes` | false | true |

Re-exporting the models and slicing (OpenSCAD 2021.01 and Bambu Studio are already
installed):

```
"C:\Program Files\OpenSCAD\openscad.com" -o blehound_tray.stl -D "part=\"tray\"" blehound_case_A.scad
"C:\Program Files\OpenSCAD\openscad.com" -o blehound_lid.stl  -D "part=\"lid\""  blehound_case_A.scad
```

Slicing: open Bambu Studio → printer P2S 0.4 → import the two STLs (choose No when
asked "as a single object") → process "0.20mm Standard" changed to 3 walls, 4/4
top/bottom, 20% infill, supports off → slice → export the sliced file to the card;
or hit "Print" to send it straight to the machine.

## 6. Basis

- Board: 70 × 60 × 1.6, M3 holes at (4,4) (66,4) (4,47.1) (66,47.1); coordinates from
  the SMT placement file (x = MidX − 113.5, y = MidY + 135).
- SMA-KWE right-angle jack (external thread, internal bore): overall length 14.5,
  thread length 8.5, body 6 × 6 × 9.2, pin 3.5, foot pitch 5.1 × 5.1; the body's
  front face is 1.0 inside the board edge, the thread protrudes 7.5 past the board
  edge, and the axis is 6.0 above the board face.
- USB-C receptacle: 8.94 × 7.35 × 3.26, protruding 1.4 past the board edge
  (measured).
- 2×5 pin header: height 8.5 (the tallest part), inner clearance height 10.0; leave
  4.0 for the pin tails below the board.
