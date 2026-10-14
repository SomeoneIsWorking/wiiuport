# Prior art

Projects about the same build, US v0 `cking.rpx`. We read them; we do not vendor or run them.

## TWW decomp (zeldaret/tww)

GameCube TWW, matched C++. HD's code is TWW's with most actor fields `+0x11c` and some class fields
4 bytes later. It is the reading aid for every guest function named in these docs.

## ZeldaWWHDRecomp/ZeldaWWHDRecomp

MPL-2.0, no game data. Read at `d23522eb`. A static recompilation with frame interpolation: logic at
30 Hz, the per-frame function `0x0203593c` re-run with no logic for the in-between frames, guest state
blended before the draw and restored after (`runtime/src/interp.cpp`, `interp_fx.cpp`).

Build match: its `camera_draw 0x024ffc40`, view inputs `+0xd4..+0x100`, viewCalc `0x027f55fc`,
`fopAc_Execute 0x025d475c` with `old +0x300`/`current +0x314`, `fpcM_Execute 0x025df940` and
`0x0278febc` as a hold-pass child are all ours.

What we take, each checked against the binary before use:
- Its list of draw-time side effects an in-between draw repeats: the one-shot `NODRAW` bit (actor
  `+0x2e0` bit `0x01000000`, consumed by `fopAc_Draw 0x025d4654`), draw-time `cM_rnd`/`cM_rndF`/
  `cM_rndFX` (`0x02019788`/`0x020198d8`/`0x02019918`), sound starts (`0x0201eba0`,
  `0x025e1988`..`0x025e1aa4`), the HD light-count clear `0x0273841c`.
- The key animator `0x027df40c` as the one place every material animation evaluates.
- The KoRL sail packet upload `0x021625dc` and `dCloth 0x0251d864` as the cloth sites.

What we do not take: its camera and model cut detection is distance thresholds (800 units, 45°,
fovy 20), which a fast follow camera can cross and a short cut need not.

## flossbud/WWHDRecomp

MPL-2.0, no game data. Read at `0f4161c3`. "True 60": every vsync runs the game frame with ~450
process types stepped at dt 0.5 and the rest drawn twice; whole-tick-only instructions are listed in
`config/US_v0/tick_rules*.txt`. Its `symbols.csv` (194 rows, US v0, each with evidence) is imported
as Ghidra labels (`scratch/re/import_labels.py`).

What we take: its notes on per-actor state, e.g. Jabun's cave flash counted in
`draw_flashing_normal` (`0x02313e28`, timer `+0x56`) and the grappling rope's 100 points settling once
per computation with no time step.

## SuperDude88/TWWHD-Randomizer

MIT. `asm/linker.ld` names a few dozen functions; already covered by the labels above.
