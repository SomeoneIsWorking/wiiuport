# ISSUE-009 — Draw-time side effects the in-between draw may repeat

**State:** open, none reproduced yet. **Affects:** ST-OBJECTS.

The skipped call's draw phase runs every draw method a second time per tick. ZeldaWWHDRecomp
(`docs/prior-art.md`) had to handle these in the same situation; each needs a reproduction here before
a change:

| Candidate | Where | Their symptom |
|---|---|---|
| One-shot `NODRAW` request | actor `+0x2e0` bit `0x01000000`, consumed by `fopAc_Draw 0x025d4654` | Link flickers in the talk camera (their #126) |
| Random numbers drawn in a draw | `cM_rnd 0x02019788`, `cM_rndF 0x020198d8`, `cM_rndFX 0x02019918` | flicker of random-target effects (raft light `0x02363374`) |
| Sound starts in a draw | `0x0201eba0`, `0x025e1988`..`0x025e1aa4` | doubled sounds |
| HD light-count clear | `0x0273841c` | point lights doubled |
| Draw methods with state | `d_a_bgn`, `d_a_ff`, `d_a_himo2` (rope settles per computation), `d_a_obj_ajav` (flash counted in `0x02313e28`, timer `+0x56`) | double-speed animation |
| Material animation outside an actor's draw | key animator `0x027df40c` | stepping at 30 |
| Cloth | KoRL sail upload `0x021625dc`, `dCloth 0x0251d864` | stepping at 30 (fixed for dCloth, below) |

## Checked

- **NODRAW (2026-10-14).** `NODRAW` status is set only by the talk camera's inlined `hideActor`
  (`talktoCamera 0x02508a60`; the stores to `+0x2e0` at `0x02509d2c`..`0x0250b16c`). Free talk with
  Grandma in `LinkRM` (`scratch/drawphase/talk.py`): 351 talk camera calls, 0 at the hide sites
  `0x02509d18`, `0x02509eac`, `0x0250a038`, `0x0250a1d0`, `0x0250b158`, and Link drawn on both presents
  of every tick. A reproduction needs a talk where an actor stands between the camera and the speaker.
- **Light count.** Same room, lamp-lit: consecutive presents' mean luminance pairs match (`293.08`,
  `293.04`; `292.96`, `292.96`), no alternation.
- **Sky cloud scroll (2026-10-14).** `daVrbox2_Draw` (`0x024d1d44`) adds actor `+0x3c8` to four cloud
  texture translations (one material's twice) and wraps each in `texScrollCheck` (`0x024d1c44`, into
  [0, 1]); both draw phases run it (599 draws, 3,594 checks over 300 ticks). Not doubled: the draw
  zeroes `+0x3c8` after using it, so the gated draw phase takes the tick's step and the tick's own
  adds 0 (read per present through the frame gate: the translations move once a tick). The step
  shows half a frame early, at about 0.0003 of the texture a tick.
- **Actor state a draw changes (2026-10-14).** Each actor drawn in the gated draw phase read whole
  (size from its profile) before its draw, after the draw phase and after its paint, on Outset
  (`scratch/drawphase/instruments/actor_draw_state_census.diff`): the draw phase changes only actor
  fields, the paint only process list links (once per scene change). The common fields, `+0x110`..
  `+0x1d8`, are the actor's `dKy_tevstr_c` from `settingTevStruct` (`0x025626a4`), whose point-light
  part (`settingTevStruct_plightcol_plus`) eases `mLightPosWorld` toward the light by
  `cLib_addCalc(0.5)` and draws a `cM_rndF` flicker per call: both twice a tick. Nothing visible
  reproduced yet; a reproduction is an actor walking past a torch.
- **Fireflies.** `ho` (process 187, `daFf_Draw`): its brk play is held by `MaterialInterpolation`; its
  glow eases only when it goes in or out of view, and did not change in `Omori`. `Nh` (313) recomputes
  its glow alpha from the material colour each draw.

- **Grappling rope (2026-10-14), fixed.** `Himo2` is process 446, draw `0x0216ca5c`. The rope held
  still on every other present: the draw builds it from segments execute places, which no blend
  covered; its sway also eased twice a tick. `title/RopeInterpolation` blends the hand, hook and
  segments and holds the sway to once a tick (`docs/frame-interpolation.md`). Reproduced in Hyrule
  with the hook on X: `x@90,x@10,x@10` throws it (`scratch/drawphase/pauses.py --burst 5`).

- **Cloth (2026-10-20), fixed.** The flag in sea room 43 (`TrFlag`) moved on every other present:
  dCloth steps its points once a tick and the draw phase copies them into the vertex buffer, so both
  paints of a tick drew the same shape. `title/ClothInterpolation` blends the points and normals for
  the gated draw phase (`docs/frame-interpolation.md`). The KoRL sail's own upload (`0x021625dc`) is
  not reproduced: the test save has no sail.

- **Shared timer (2026-10-20), not reproduced.** TIMER's method table (`0x101EE73C`) has an empty
  execute (`0x025c5d1c`, `li r3,1`) and a draw (`0x025c5d18`) that branches to the timer's update
  `0x025c5a8c`, which decrements the countdown `+0x10C`: every draw phase steps it, so under the gate
  a mini-game timer would run at twice its speed. Needs a timed mini-game to reproduce; the test save
  reaches none.

## Where the rest are

Stage archives (`content/Common/Stage/*.szs`, Yaz0 SARC, the `.dzr`/`.dzs` inside the `.bfres`)
indexed by the 8-byte actor names their `ACTR`/`SCOB`/`TRES` chunks place
(`scratch/re/stage_actors.py`): `Bgn` in `GanonK` room 0; `ho` in `Omori` 0, `TyuTyu`, `Otkura`;
`AjavW` in `Pjavdou` 0; `TrFlag` (cloth) in `Adanmae` and `sea` rooms 14 and 43; `SieFlag` in `sea`
rooms 8, 22, 24, 25, 37, 46. `Himo2` is the grappling hook's rope and `Ajav` flashes when hit, so
both need items this save lacks.

Names to process numbers: `l_objectName` (`{name[8], s16 process, s8, s8}`, 809 entries) at
`0x101ebfdc`. Profiles: `process_profile_definition` with the process name at `+0x8`, instance
size at `+0x10` and the actor methods (create, delete, execute, isDelete, draw) through `+0x24`.
