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

Candidates read at `f10b5662`, not yet checked against our binary:
- Key animator frame (`0x027df40c`): time block `*(anm+0)`, frame `+0`, cached at `anm+0x28`;
  midpoint written before the call (`runtime/src/interp_fx.cpp`).
- Shared timer `dTimer_c::_execute 0x025C5A8C` is in TIMER's draw slot: counts down twice a tick
  (`countdown.cpp`).
- Every 3D line (`mDoExt_3DlineMat1` uploads `0x025EC62C`, `0x025ED1BC`; count `+0x17C`, lines
  `+0x184`), cloth uploads per kind, `dCloth` buffers `[2]` at `+0xB0/+0xB8/+0xC0` (big-endian;
  their issue #36).
- Torch light easing `0x025615B8` (`tevstr+0x84..0x8C`); random seeds `0x101FF9D4` (3 words).
- Aim reticle `0x023D813C`/`0x020CBB70` around `mDoLib_projectViewport 0x025F1018`; Z-target arrow
  `dAttDraw_c::draw 0x024EC1C8` (position in r4).
- Plant sway slots (grass `0x0254C6C4`, tree `0x025C9948`, flower `0x02548370`, bushes `0x025D0994`).
- Weather packets beyond waves, clouds and snow (rain, spores, fog, poison fog, stars).
- `dKyr_drawStar 0x02574144` reads the camera at paint time; audio camera `0x025E1B44` keeps
  pointers to what `camera_draw` passes; audio frame callback `0x020315CC`.
- New particles drawn back along their first step (velocity `+0x34`, emitter `0x104B5730`).
- Model UBO update `0x027F5018` runs on a job thread while the main thread draws (their issue #68).

## flossbud/WWHDRecomp

MPL-2.0, no game data. Read at `0f4161c3`. "True 60": every vsync runs the game frame with ~450
process types stepped at dt 0.5 and the rest drawn twice; whole-tick-only instructions are listed in
`config/US_v0/tick_rules*.txt`. Its `symbols.csv` (194 rows, US v0, each with evidence) is imported
as Ghidra labels (`scratch/re/import_labels.py`).

What we take: its notes on per-actor state, e.g. Jabun's cave flash counted in
`draw_flashing_normal` (`0x02313e28`, timer `+0x56`) and the grappling rope's 100 points settling once
per computation with no time step.

Candidates read at `2f562805`, not yet checked against our binary:
- Restoring what a draw writes: their in-between frame journals stores to executed actors and
  .data/.bss and undoes them, excluding J3DDrawBuffer links (`0x027F093C`, `0x027F0C14-0x027F1088`,
  `0x027F1508`) and the JAI sound lists (`0x02801444-0x0280E03C`), and not the HUD's heap text
  (`src/overrides/sixty.cpp`, `docs/recompiler-design.md`).
- Real shadows are drawn only in a frame whose environment update `0x0255E854` requested them; a
  frame without the request draws a 4x4 placeholder (lead for ISSUE-007).
- Draw-time readers of the frame counter `0x101FF560` (Link damage flash `0x023DA350`, hookshot chain
  `0x02176B6C`, Lwood sway `0x021B6C94`, Jabun lamp `0x0224864C`/`0x0224868C`).
- Draw methods advancing state: Morth `0x021A6F64` (`+0x576`), FAN `0x02131FF8` (`+0x634`), Helmaroc
  tail `0x020660E0` (`+0x40A`), palette blend timer `0x0255817C`.
- `dCloth` move `0x0251D4EC` flips `+0x1C0`, so the previous tick's points are the other buffer.
- HUD layout animation (`AnimTransform` frame `+0xC`, step `0x02872DFC`), fades (`0x027ECED4`,
  `calcFade 0x025F06A8`), sun time `0x10475A68+0x1020`, lava `0x02524CA0`, particle texture scroll
  truncated at `0x02832708`/`0x0283091C`, shoreline foam btk `0x0207A05C`.

## SuperDude88/TWWHD-Randomizer

MIT. `asm/linker.ld` names a few dozen functions; already covered by the labels above.
