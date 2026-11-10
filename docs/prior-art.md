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
- The KoRL sail packet upload `0x021625dc` and `dCloth 0x0251d864` as the cloth sites (`0x0251d864`
  checked: the draw phase's vertex copy, packet in r3).

What we do not take: its camera and model cut detection is distance thresholds (800 units, 45°,
fovy 20), which a fast follow camera can cross and a short cut need not.

Candidates read at `f10b5662`, not yet checked against our binary:
- Key animator frame (`0x027df40c`): time block `*(anm+0)`, frame `+0`, cached at `anm+0x28`;
  midpoint written before the call (`runtime/src/interp_fx.cpp`).
- Shared timer `dTimer_c::_execute 0x025C5A8C` is in TIMER's draw slot: counts down twice a tick
  (`countdown.cpp`).
- Every 3D line (`mDoExt_3DlineMat1` uploads `0x025EC62C`, `0x025ED1BC`; count `+0x17C`, lines
  `+0x184`), cloth uploads per kind (`dCloth` buffers `[2]` at `+0xB0/+0xB8/+0xC0` checked).
- Torch light easing `0x025615B8` (`tevstr+0x84..0x8C`); random seeds `0x101FF9D4` (3 words).
- Aim reticle `0x023D813C`/`0x020CBB70` around `mDoLib_projectViewport 0x025F1018`; Z-target arrow
  `dAttDraw_c::draw 0x024EC1C8` (position in r4).
- Plant sway slots (grass `0x0254C6C4`, tree `0x025C9948`, flower `0x02548370`, bushes `0x025D0994`).
- Weather packets beyond waves, clouds and snow (rain, spores, fog, poison fog, stars).
- `dKyr_drawStar 0x02574144` reads the camera at paint time; audio camera `0x025E1B44` keeps
  pointers to what `camera_draw` passes; audio frame callback `0x020315CC`.
- New particles drawn back along their first step (velocity `+0x34`, emitter `0x104B5730`).
- Model UBO update `0x027F5018` runs on a job thread while the main thread draws (their issue #68).

Read again at `19e1fcc9` (v0.5.1, 2026-11-06), not yet checked against our binary. Their v0.5.0 claims
interpolated play matches 30 fps logic; "True 60" is retired and an image-generated 60 is experimental.
- Leaks into the next step, closed as one class (`179a73ed`): every actor drawn on a hold pass is saved
  before its draw (`fopAc_Draw 0x025D4654`, size from profile `+0x10`) and put back after the paint,
  skipping `+0..+0xF8` (process and draw tags, `6bb4bf1f`, `c6fd5083`); `J3DModel::calc 0x027F4D5C`
  in a kept draw saves the joint world matrices (model `+0x2C`, jnt `+0x10`/`+0x2C`). The one-shot
  `NODRAW` request is re-applied after the save (`1e466d52`).
- Draw-slot logic skipped on hold passes: Game Over state machine `0x025499A0` (`d7d1a071`),
  `ResLoadTask::calc 0x0260C74C` (`3d2a018f`); a draw-time `cM_rnd` returns the next value without
  advancing the seeds (`1ccd83a1`).
- Camera: shake added after the blend (store `0x024F8430`, body `cam+0x248`, shakes `+0x56C`/`+0x578`),
  HD zoom addCalc once a step (`+0x730`/`+0x734`, `6bfa85cc`); letterbox `CalcTrimSize 0x024FF8A0`
  (`+0x5FC`/`+0x600`, `8cabf84f`); play window `0x10474B7C` and blended camera words restored before
  logic (`f9e1bf7d`); culling frustum from the blended fovy (clipper `0x1048CFF0`, `55efb4f1`). Cuts
  are still distance thresholds (800), no `dCamera_c::Reset` hook.
- Sky drawn from the drawn eye: `dKyr_drawSun 0x0256F1C4`, `dKyr_drawLenzflare 0x02570210`, stars
  `0x02574144`, sea spray `dKyr_drawSibuki 0x02571B1C` (`f3dd8ed8`, `db103725`).
- Draws that advance state: Puppet Ganon (`daBgn_Draw 0x0207B2CC`, parts `+0x4D0` stride `0x3F0`) and
  Jabun's cave (`draw_flashing_normal 0x02313E0C`, parts `+0x9AC` stride `0x90`) drawn from the logic
  pass's bytes; `McaMorf::play 0x025E535C`/`0x025E65FC` a no-op on hold passes; firefly is `nh`
  `0x021E1338 +0x834` (`55efb4f1`, `2c39539b`, `d4660fa7`).
- Material and scroll: vrbox2 SRTs (speed `+0x3C8`), magma `0x0258CA60`, eye texmtx
  `daPy_matAnm_c::calc 0x02413E80`, ship hull `+0x6B8` (`daShip_c::draw 0x0247255C`), fallen-tree
  alpha `dTree_packet_c::update 0x025CA6EC`, plant hit slots (grass `+0x18F0C`, tree `+0x2A9C`,
  flower `+0x35BC`), fades (`calcFade 0x025F06A8`, GC fade `0x0252F5A0`, fader `*0x101F86B4`).
- Lines and trails: `mDoExt_3DlineMat0` `0x025EA548`/`0x025EAF58`, sword blur `0x024142F0`, boomerang
  `0x020CDF78`, hookshot `daHookshot_shape_draw 0x021768B0` (count `+0xD504`), `dChain` `0x0251AE90`.
- Shadows: cascade splits `updateShadowSurface 0x027987BC`, `uploadShadowEnvironment 0x027FB678`.

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
- HUD layout animation (`AnimTransform` frame `+0xC`, step `0x02872DFC`), fades (`0x027ECED4`,
  `calcFade 0x025F06A8`), sun time `0x10475A68+0x1020`, lava `0x02524CA0`, particle texture scroll
  truncated at `0x02832708`/`0x0283091C`, shoreline foam btk `0x0207A05C`.

## SuperDude88/TWWHD-Randomizer

MIT. `asm/linker.ld` names a few dozen functions; already covered by the labels above.
