# Frame interpolation: the mechanism

How `wiiuport` presents Wind Waker HD at sixty pictures a second. This document owns the
mechanism. What any value *means* belongs to the consuming title; the addresses, the payload
and the blend policy are the title project's, and this repository keeps only the capability
to patch guest code in memory.

The in-between frame is the title's own draw phase run at midpoint inputs ("The game-side
in-between frame"). The paint path, the logic gate and the guest structure they rest on come first;
the GPU-side mechanisms tried before are summarised where their RE findings are still used.

## What the guest gives us

Guest GPU work reaches the runtime as **display lists in guest memory**. The command processor
sees `IT_INDIRECT_BUFFER` packets carrying a physical address and a size in dwords, and walks the
referenced buffer (`LatteCP_itIndirectBuffer`, `LatteCP_processCommandBuffer`). A frame ends at
`LatteRenderTarget_itHLESwapScanBuffer`, which bumps `LatteGPUState.frameCounter` and calls
`Renderer::SwapBuffers`.

Two consequences decide the whole design:

1. **A frame's draw stream is re-runnable.** It is data in memory, not a consumed stream, so the
   same frame can be issued a second time with different inputs.
2. **All uniform data for a draw is assembled in one place.** Whichever way the guest supplied it,
   `VulkanRenderer::uniformData_updateUniformVars` gathers it into a single buffer immediately before
   upload. That covers both of Latte's uniform modes: `FULL_CFILE`, where values come from the ALU
   constant registers that `IT_SET_ALU_CONST` wrote inline in the display list, and `REMAPPED`, where
   they are loaded from uniform buffers in guest memory.

The second consequence is what makes a blend possible at all: there is one site at which every
uniform a draw will use is in one form, after the guest's values have been read and before the GPU
sees them. **It is also why the deleted mechanism wrote there and not in the display list** -- a
display list carries its values inline and in a second, buffer-loaded form, so a substitution there
would need one site per uniform mode and would have to care about packet layout.

## The mechanism: the title's own paint path

`title::WindWakerPaint` does not draw anything. It makes the **title's own display thread** paint
twice per tick, so every draw, every skinning pass, every attribute fetch and every display list is
produced by the game at the pose the game holds -- and there is no host-side copy of the frame to
keep in step with one.

### How it reaches the frame

The title's display frame lives in a vtable slot: `0x10004e88` is the display object's vtable,
slot `0xcc` (`0x10004f54`) holds the address of `0x0274c264`. The mod reads that slot, writes an
eleven-word stand-in into executable guest memory obtained from
`RPLLoader_AllocateTrampolineCodeSpace`, and puts the stand-in's address into the slot. The display
thread's loop at `0x0274c00c` -- eleven instructions, no callers, no callees, never returning -- then
branches to the stand-in where it would have branched to the frame.

**The eleven words are lifted verbatim from this image**, not hand-derived:
`819f0024 800c00cc 7c0903a6 7fe3fb78 4e800421 4e800421 4e800020`, as `kDisplayFrameFirst` and the
loop's own dispatch words beside it. The *forms* of the two address words are lifted too -- `lis` and
`ori` -- with the immediates being the address the mod read out of the slot before rewriting it, and
the two address words are checked by **reconstructing** the frame from them rather than by asserting
two constants.

`display+0x50` is set to 1: the field the title's single `GX2SetSwapInterval` call at `0x0274bafc`
passes, and the same field `0x0274c874` tests afterwards to decide whether to wait for the flip.

### Which stand-in, and what the modes are for

The mode number is the product's own mapping, so a run names what it did. The thirteen modes are
each a *different claim*, and the ones that fault are kept for the same reason the falsifier is
kept: a run that paints is not evidence that a run that does not was the right shape.

| mode | name | what it claims |
|---|---|---|
| 1 | `PassThrough` | The redirect alone changes nothing; a run where this does not hold the title's rate says the redirect is at fault, not the second paint. |
| 2 | `Twice` | Two paints, the title's own two vblanks a flip. |
| 3 | `TwiceAtSixty` | Two paints, one vblank a flip. |
| 4 | `IndirectOnce` | Re-reads the frame from the vtable through the count register. **Does not run**, and is kept because it is the falsifier for that choice: same block, same words, same one rewritten word of vtable, and the only difference from mode 1 is `mtctr`/`bctr` against `b`. |
| 5 | `IntervalField` | Not a paint count at all: the interval field alone, nothing else changed. |
| 6 | `OneAtSixty` | One paint, one vblank a flip, reached by a plain branch so the frame's return goes to the title's loop. The rate the mechanism is for: the flip paces the loop, so one vblank a flip is what doubles the picture, and the second paint is what carries the blend. |
| 7 | `BranchEntry` | The control for *how* a stand-in is reached: a direct branch at the frame's entry rather than an indirect call through the vtable. A falsifier with a known-good control in the same mechanism -- payload, interval and rate unchanged, the only difference the kind of branch. |
| 8 | `TailTwiceAtSixty` | Two paints where only the *first* is a call; the second is a tail branch, so the frame's return goes to the title's own loop and the payload never sets the link register. Separates "the frame is not re-entrant" from "the second `bl`'s link register is the problem", and is the more faithful shape. |
| 9 | `RestoreDisplayTwice` | Two paints, with a field the first paint's path changes saved before it and put back before the second, so both paints run the path the first one chose. **Every word is the frame's own, verbatim.** |
| 10 | `SamePhaseTwice` | Two paints at the same phase of the flip. |
| 11 | `ObjectivePayload` | The objective's own eleven words, as written. |
| 12 | `LoopDispatchTwice` | The loop's own dispatch, reached twice, with the frame re-read from the vtable. |
| 13 | `LoopFrameLiteralTwice` | The same dispatch with the frame carried as a **literal** in the payload. **This is the one that paints at sixty.** |

### The interval field alone does not open the gate, and that is measured

A run that wrote `display+0x50 = 1` and left the emulator's flip pacing alone read `interval 1` while
the paints ran at **29.8 a second** (1,553 to 1,687 paints over 4.5 s). **The title's own record of
the interval it asked for is a statement about the gate, not a thing that opens it** -- the display
thread's rate follows the emulator's flip pacing, and the field only records what was asked for. Both
are set for the painting shapes now, and the arming refuses if the pacing change does not take, because
a field saying one while the flip still takes two vblanks is a claim nothing backs.

### The rate, measured with the reservation fixed

Adjacent 8.00 s windows on the real disc, through the control channel, in one driven run:

```
unmodded, first window:                    240 paints = 30.00/s
unmodded, second window:                   241 paints = 30.12/s
mode 13, first window:                     480 paints = 59.99/s
mode 13, second window:                    478 paints = 59.73/s
```

**The loop's pass sets the rate, and the stand-in adds a paint inside a pass rather than a pass.**
480 paints over 240 passes is 240 passes in 8.00 s, against 241 unmodded. The pass rate is unchanged:
the stand-in does not add a pass, and a rate measured without that fact is not a rate.

The second window's 29.87 passes a second is **below the 29.9 band** and is reported as such rather
than dropped: another repository's build was running on the same machine at the time
(`zelda3d_app -j2`, 2,327 s old, visible in `ps` during the run). The first window's 30.00 is in band.
**One of the two windows is in band, one is below it, and the confound is named instead of the
unfavourable sample being discarded.**

### The flip-skip risk, resolved by reading the field per paint

The frame is documented to do `if (display+0x74 & 1) display+0x74 ^= 2`. If that fires between the two
paints of a pass, one paint of the pair presents and the other does not, and the rate would read sixty
while the display presented thirty.

**A rate cannot answer this** -- both cases present at the same rate -- **and neither can a field read
once on request**, which reads whichever value the last paint left. So the probe samples `display+0x74`
and `display+0x28` at **every** paint, under the same lock as the display pointer, and the report
carries the pair. Measured, 24 samples a quarter of a second apart in each state, on the real title:

```
stand-in out:  paints 1575, flags 0x0, phase 0x2, interval 2
stand-in in:   paints 1877, flags 0x0, phase 0x2, interval 1
```

**One distinct value per window, held across roughly 300 paints: `display+0x74` is 0 throughout, so
`flags & 1` is false at every paint the probe saw and the toggle never executes. No paint is left
without a flip, and the risk does not apply to this title's frame.** The same readings also say the
probe reads live values rather than a constant: the interval field moved from 2 to 1 across the
arming, which is exactly the pacing change the stand-in asks for, in the same two readings where the
flag did not move.

## The logic gate, and the finding it was built for

The objective's conditional: *if the logic rate doubles, it was slaved to the flip; gate it in the
logic path and re-measure.* **It doubled.** With the probe refusal gone (below), the gate at
`0x025d42ec` installs and counts, and the tick runs one-for-one with the paint:

| window | paints | paint rate | tick calls | call rate | ticks run | tick rate |
|---|---|---|---|---|---|---|
| unmodded | 241 | 30.12/s | 241 | 30.12/s | 120 | 15.00/s |
| mode 13, first | 474 | 59.24/s | 474 | 59.24/s | 237 | 29.62/s |
| mode 13, second | 480 | 59.99/s | 480 | 59.99/s | 240 | **30.00/s** |

Two instruments, one number, and the gate's counter is read straight out of guest memory rather than
from its own report: 90 calls in 3.0 s on the host probe and 90 on the gate's own guest counter, and 45
ticks run -- **30.0 calls a second in, 15.0 ticks a second out.**

**The gate in the logic path is what holds the logic at thirty**, and the second window's 30.00 is
inside the 29.9-30.0 band. The first window's 29.62 is below it and is reported rather than dropped.
The harness's own control reads 15.00/s unmodded, and that is the gate doing its job rather than the
title running at fifteen -- which is why the control exists: the same warning that fires at 15.00
unmodded fires at 30.00 with the picture at sixty, and the only difference between them is the paint
rate.

**This reverses two claims this project made about the gate, and both were wrong the same way.** That
"the gate is a backstop, not the mechanism" and that "the logic stays at thirty, so there is nothing to
gate" were both read off runs in which the gate's probe had been refused, and the gate therefore
counted nothing while the title carried on. **A gate that was never installed looks exactly like a gate
that was not needed**, which is the whole argument for reporting an instrument's own installation
state beside its counts.

## The null case is a discriminator, not a uniformity

Condition 4 asks that the null case -- two paints, no blend -- be identical to one paint, and that a
blended paint differ from both and be nearer each than they are to each other. The null arm, two
consecutive presents, two rounds, with the stand-in painting twice per pass and nothing blended:

```
round 0:  53,634 of 6,220,800 bytes differ (0.86%), largest delta 3, all 53,634 within 4
round 1:  52,911 of 6,220,800 bytes differ (0.85%), largest delta 3, all 52,911 within 4
```

**Against a genuinely different frame of the same title, in the same run shape: 32.45% and 34.90% of
bytes differ, with largest deltas of 164 and 221.** So the null case is not merely "the two images are
the same" -- it is on the same side of a measured line as one paint, and two orders of magnitude away
from a different frame. That is the null arm, with the denominator (6,220,800 bytes), the magnitude
(largest delta 3 against 164) and the comparison stated.

**Why it is this small now and was 32-39% before:** the logic is gated to thirty while the picture is
presented at sixty, so consecutive presents alternate between the two paints of one tick, and with
nothing blended those two paints are the same tree at the same pose. **The earlier 32-39% figures were
a different frame, because the logic was not gated and the tick was not running at all.**

**The positive arm cannot run yet, and is not claimed.** It needs a blend, and there is none.

## Four defects, and what each one cost

Every one of these is a defect in code this project wrote, or in the fork it pins. All are fixed, and
each is recorded because the failure mode is the reusable part.

### 1. A reservation of seven words against an eleven-word payload

`kMaxWords = 7` was the size of the block the mod reserves from the loader's arena, and modes 12 and 13
need eleven. **The write path had no bound against the reservation**, and the arena is a bump allocator
shared with every other module, so a payload that outgrows its reservation does not fail -- it writes
over whatever module asked for memory next. Measured, in one run, from the harness's own report:

```
paint mod's block   0x00e05880      11 words, 0x5880 - 0x58ac
logic gate's block  0x00e0589c      inside it, at the stand-in's seventh word
```

The gate's counter stub landed in the stand-in's **second** call, so the stand-in's second paint ran
the gate's pass-through code and branched to the tick instead of painting. The run that produced this
reported `mode 13 with the gate halving: 480 paints in 8.00s = 59.99/s; 0 tick calls and 0 ticks run`
-- **and the sixty was one paint and half a gate.** A rate measured with the second paint calling into
someone else's stub is not a rate.

The fix is at the cause, in two parts and one owner. `payload()` now refuses a payload that does not
fit, beside the refusal it already gives a branch that cannot reach -- a stand-in that cannot be built
correctly must not be built. And `paint_tests.cpp` enumerates every mode and asserts each one's payload
fits, so a mode added later that outgrows the reservation fails the build rather than a display thread
that may not survive the arming. The reservation is renamed `kReservedWords`, because a reservation and
a description of the largest payload are different numbers and only the first is true.

**The test has been shown the other answer.** With the reservation put back to seven, the same binary
fails `mode 12 (loopDispatchTwice) is built where the loop is in reach`, `mode 13` likewise, and
reports `9 failures`; with it at eleven, `0 failures`. An instrument that has only ever agreed is not
evidence.

### 2. A probe refusal that silenced every instrument with a frame

The display frame probe fault had been diagnosed correctly: an entry whose first instruction reads the
link register computes a different value in the stub than it does where it stands, because a call sets
the link register. **It was fixed by refusing to install such a probe** -- and `mfspr r0,LR` is the
first instruction of every function with a frame. So the refusal took out the very probes this work
depends on:

| probe | entry | consequence |
|---|---|---|
| the binder | `0x027ff88c` | **0 bindings** over 823,431 assemblies |
| the second binder | `0x027ff9c0` | 0 bindings |
| the logic gate's tick | `0x025d42ec` | **0 tick calls** with 240 paints in the window |

The report said `installed as unknown and unknown` and read `identity: source blockSources` with 0
objects tracked, and **both read as a statement about the title rather than about the instrument.** The
gate's zero was reported as evidence that the tick address is not executed; that reading is withdrawn.
It was a refused probe.

The fix is at the cause, and it is an **ordering**: the stub now runs the displaced word *before* the
HLE call, so the link register is the one the caller left -- the value the instruction computes where it
stands -- and the instruction is correct. The refusal is deleted, `Installation::EntryReadsLinkRegister`
with it.

### 3. The dispatch matched the stub's base, not the call's address

**Which exposed a second defect, and that is why the ordering alone was not enough.** `Dispatch` found
its registration by comparing the interpreter's program counter against the **stub's base**, which worked
only because the HLE happened to be the stub's first word. Moving it to the second word silenced every
probe in the product at once, and the symptom was the paint mod refusing to arm with *"the display
thread has not painted yet"* -- on a title painting thirty times a second. The registration now carries
`hleAddress` and the comparison is on that. `kHleWordIndex` is the displaced word's index, named rather
than assumed.

### 4. A payload branching into a zero-filled hole, and a probe on a word that reads the link register

Two faults in this project's own code that a gdb window reported as a fault in the guest's: a payload
branch into a zero-filled hole at `0x028fad2c` (the `GX2SetSwapInterval` call site), and a probe sitting
on the frame's first word `mfspr r0, LR`, which the frame returns through. With both fixed the register
that was wrong is right -- `lr = 0x0274c280` is the frame's own seventh word -- and **the guest still
reached the arena.** The lesson that took longest: the fault was the host's, and it moved three times
while being attributed to the title. The frame writes one word above its own allocation, and the
instrument that reported it was reading the host's memory rather than the guest's.

## The GPU-side pose search: deleted

A pose blend at the GPU (`PoseBlend`, fed by `PoseByShader` from the `ObjectPoseLocator`,
`UniformBlockCensus`, `NodePoseLocator`, `DrawAttributeCensus`, `VertexPoseHistory` and
`GlobalPoseCensus` instruments, with draws named by `CommandStreamIdentity`) looked for each object's
pose in the assembled uniforms and vertex bytes and wrote midpoints there. Once draws were named
correctly, 390 of 482 uniform candidates were values shared across objects (camera and pass values),
and no vertex layout paired across frames. It was deleted for the game-side mechanism below. What it
established about the title:

| guest | what |
|---|---|
| `0x027ff88c`, `0x027ff9c0` | the node's uniform-block binders: ~590 nodes, ~196k binds per window; they run while the guest writes the command buffer, before Latte executes the draw |
| uniform blocks | double-buffered: tick N-1's block is still readable when tick N paints (16 of 16), at a different address |
| vertex attributes | the title's 32-bit float attributes are `SWAP_U32` (big-endian) |

## Where the pose is: layout panes in ALU constants, CPU quads in vertex bytes

The ALU-constant uploads through `0x02874024(ctx, key)` (a key looked up in the program's table at
`program + 0x25c`) belong to the **NintendoWare layout library** (`nw::lyt`): the HUD, menus and
other 2D screens. The context is its `DrawInfo` (constructor `0x02873d70`): a 4x4 projection at
`+0x00`, a 3x4 view at `+0x40`, the pane's model-view at `+0x70`, the bound program at `+0xa8` and
the model-view-loaded flag at `+0xb6`.

| key | uploaded by | what |
|---|---|---|
| 0 | `0x02874038`, 16 words from `ctx+0x00` | projection: 1 distinct value per window |
| 1 | `0x02874074`, 12 words from `ctx+0x70` | the drawing pane's model-view |
| 2-7, 0x11 | `0x0288285c` | material: texgen rows, colours |
| 9, 0xe-0x10 | `0x02880f1c` | texture matrices |

| guest | what |
|---|---|
| `0x02877100` | `Pane::Draw(pane, drawInfo)`: if visible, `DrawSelf` (vtable `+0x94`), then each child's `Draw` (`+0x8c`) |
| `0x028766cc` | `Pane::CalculateMtx` (vtable `+0x84`): the pane's global 3x4 at `pane+0x48` |
| `0x028771b8` | `Pane::LoadMtx` (vtable `+0x9c`): copies `pane+0x48` to `drawInfo+0x70` and clears `+0xb6` |
| `0x0287d2cc` | `Picture::DrawSelf`: `LoadMtx`, then the quad by kind (`0x0287b180`, `0x0287c358`, `0x0287d1c4`) |
| `0x02883ab4` | material set (vtable): program change `0x028740e4`, then the material constants |

Key 1 is uploaded once per pane, at its material apply (`0x02880f1c`, `0x02880e90`), while
`+0xb6` is clear. **Every key-1 upload in gameplay is a layout pane**: a call-chain census
(`WIIUPORT_CALLER_CENSUS=02874074:7c0802a6,...`, `GET /callers`, walking) put all 62,703 calls, in
16 chains, under `Pane::Draw`'s recursion. A pane's identity is `r3` at `LoadMtx`. HUD panes carry
translation z = -989.1. `CalculateMtx` writes the global matrix in place, so `LoadMtx` has no static
store to `ctx+0x70` and Ghidra had not disassembled it.

**The 3D world's camera view is not in these constants**; it is in the model renderer's view
blocks, below. Moving particles, sea waves and sky clouds are CPU-written into
`ca2d0854ee6b264d`'s positions every frame (`POST /draws?frames=60`, standing and walking).

### The 3D camera: per-model view blocks, double-buffered

HD draws models with NintendoWare `g3d` behind a J3D-shaped wrapper (resource offsets relative to
themselves, shaders `wii_pipeline.sharcfb`). The camera reaches the shaders through a **view
block** per model per view, an entry of `0x23c` bytes in an array `{count, entries}`:

| guest | what |
|---|---|
| `0x104b45f8` | the camera's view 3x4, the J3DSys-style global every model reads |
| `0x027f55fc` | a model's calc: cycles its buffer index (`+0x6c`, modulo the buffering count), calcs world matrices (`0x027de8a0`), then its view blocks (`0x027f53cc`) |
| `0x027f53cc` | per view: `0x027fda54` with the camera view, the model's lights, then `0x027fdff4` |
| `0x027fda54` | the view setter: view 3x4 to entry `+0x74`, projection x view to `+0xa4`, projection to `+0x1fc`, lights to `+0xe4`.. |
| `0x027fdff4` → `0x027fb678` | the commit: `+0x4c` = `+0x48` (the slot to bind), `+0x48` flipped (the slot to fill next), then queues the upload job at `+0x54` (`0x027f9ae0`) |
| `0x027fb880` | the upload (vtable `0x1016ef54` `+0x24`): members 0 view (3 vec4, offset 0), 1 projection x view (4 vec4, `0x30`), 2-7 lights, 8 projection |
| `0x027f16e8`, `0x027ff88c` | shape draw and binder: bind the slot at `entry + 0x10 + index * 0x1c` (buffer `+4`, size `+0xc`) with `GX2Set*UniformBlock` |

Entry construction is `0x027fb40c`/`0x027fd838`: two buffer slots at `+0x10` and `+0x2c`, index
`+0x48` = 0 and `+0x4c` = 1. **Every commit binds the other slot**, so the slot a model's draws bind
changes exactly once per tick, and the slot not bound holds that model's view from the tick before.
That is the pairing a camera blend needs, by identity: entry and slot, no value matching. Members 0
and 1 are linear in the view, so their midpoints are the midpoint view's own. The upload's byte
layout in the GPU buffer is not yet measured.

`0x027fb678` commits every double-buffered g3d block, not only view blocks. Counted by kind (a probe
on the commit, since deleted), 5 s walking on Outset, 22.4 ticks/s:

| vtable | entries seen | commits per tick | slot bytes |
|---|---|---|---|
| `0x1016ef54` (view) | 426 | ~199 | 512 |
| `0x1016ef84` | 2090 | ~726 | 64 |
| `0x1016efb4` | 38 | ~19 | 768 |
| `0x1016efe4` | 1563 | ~188 | 768 |
| `0x1016f014` | 1 | 1.0 | 672 |

The one `0x1016f014` entry commits once per tick, which ties commits to ticks. All kinds together
commit about 0.3 MB per tick; view blocks are about 100 KB of it.

### The view blend: retired

A blend of key 1 per context, holding the tick's own upload and writing the midpoint at the
register file on the in-between paint, paired different panes' model-views: on in-between paints
the hearts moved and HUD fragments were drawn in the world (`POST /paint` mode 13, gate on,
walking). It was deleted with the fork's `OnAluConstants` hook. Key 1 was never the camera; a pane
blend would key on the pane at `LoadMtx`.

### The CPU-written quads: writers

`ca2d0854ee6b264d`'s draws are quads the particle, sea-wave and sky-cloud writers fill on the CPU each
paint: four corners at stride 20, a big-endian `32_32_32_FLOAT` position then 8 bytes of UV, in one of
the object's two alternating buffers. 3D lines are stride 152, 10-12 vertices. The writers, for the
game-side blend of each:

| writer | guest | what |
|---|---|---|
| particle commit | `0x02825158` | called by the ripple draw and every JPA draw executor; vertex store `+0xe0`, buffer stride `0x254`, flip `+0x950` |
| vertex-buffer flush | `0x027b5e94` (`lwz r12,0x140(r3)`) | buffer vertices at `+0x140` |
| sea waves | flush returns to `0x02575488` | packet in r30, wave index in r23, waves at `+0xa0` stride `0x38`, counter `+0x24` |
| sky clouds | flush returns to `0x02576ff4`, `0x02577040` | flip in r31, card buffer stride `0x254` |
| 3D lines | double-buffer helper `0x027ff1d8` (`stwu r1,-0x20(r1)`), returns `0x025edb2c`, `0x025ed110` | |

A GPU-side quad blend (`title/QuadBlend`, fed by probes on the three writers) blended the copy of each
buffer at the draw. It was deleted with the fork's vertex-replacement hook: it is the wrong side of the
game for the reason below, and blending a written vertex cannot tell what the writer derived it from.

Evidence: the GX2 HLE's caller histogram (`GX2SetVertexUniformReg` link register, offset, size,
distinct values; a temporary fork-side counter, not kept). A Ghidra caller search by the name
`GX2SetVertexUniformReg` returns only effect passes and misses the whole `0x0287xxxx-0x0288xxxx`
renderer, whose calls go to the import stub `0x028fadac`.

## The game-side in-between frame

Direction: the in-between picture is drawn by the title's own draw phase, run a second time with
midpoint inputs, rather than blended per value at the GPU. A GPU-side blend only covers the values
it identifies (the view block also carries view-space lights; billboards, culling and particles are
derived in game code), and it cannot list what it missed.

The tick is TWW's `fpcM_Management` (`f_pc_manager.cpp`), which already separates advancing the
world from deriving its picture:

| guest | TWW | role |
|---|---|---|
| `0x025d42ec` | `mDoMain` frame callback | the gated tick: `fpcM_Management(0, 0x025d42c4)` |
| `0x025df948` | `fpcM_Management` | `MtxInit` `0x0200fac4`, `fpcDt` `0x025de024`, `fpcPi` `0x025e0ee4`, `fpcCt` `0x025ddac4`, then Ex, Dw, callback 2 |
| `0x025df5c0(0x025df940)` | `fpcEx_Handler(fpcM_Execute)` | actor logic: advances state |
| `0x025de37c(0x025df908, 0x025de2cc)` | `fpcDw_Handler(fpcM_DrawIterater, fpcM_Draw)` | actor draw: derives the picture |
| `0x025d4654` | `fpcDw_Execute` | one process's draw method (`*(process+0xf0)`) |
| `0x025e2de0`/`0x025e2e5c` → `0x025e2bf4` | `mDoExt_modelUpdateDL`-style | per model: save the J3D view `0x104b45f8`, copy in the view object's camera (`+0x84`, or `*(+0x48)`; projection from `+0x4c`/`+0x164` into `0x104b470c`), model calc `0x027f55fc`, restore |

Runtime census (gameplay, walking): every model calc and view pass (`0x027f55fc`, `0x027f53cc`,
`0x027fda54`) runs under `0x025d4654`, called from actor draw methods; none from the paint. The
default view object is `*(*(0x101f95d0 + 0x1024))`, the second entry when `+0x1020` > 1.

**One thread runs both.** The display loop `0x0274c00c` calls the frame `0x0274c264`, whose
`0x02034ffc` takes the display lock (`display+0x18`), runs the sead task tree (`0x02746790`, which
reaches the tick through `0x020359c8` → `0x025f172c`), then executes the draw lists through the
render manager `*0x101f86e8` (`0x0272a8c4`, `0x0272ad80`). With the gate skipping the tick, the
stand-in's second frame re-executes the lists the last draw phase built.

**A draw phase is self-contained.** `0x025de37c` is `fpcDw_Handler`: BeforeOfDraw `0x025f03c4`
resets the draw list (`0x0252f264` on game info `+0x5d30`; game info is `0x025200d4()`), the
iterator draws every process, AfterOfDraw `0x025f03f0` finishes (`0x0252e388` on `+0x60ec`). A
second run replaces the lists rather than adding to them.

**The camera is derived in the draw phase from five inputs.** HD's `camera_draw` is `0x024ffc40`,
TWW's with the `view_class` fields 4 bytes later: perspective `0x028e9948(fovy +0xd4, aspect +0xd8,
near +0xcc, far +0xd0)` into `+0x104`; `lookAt 0x025f1eac(+0x144, eye +0xdc, center +0xe8, up +0xf4,
bank s16 +0x100)`. It also refreshes the sead `LookAtCamera` the render layers read (matrix at
`this`, vptr `0x101450f8` at `+0x30`, pos `+0x34`, at `+0x40`, up `+0x4c`;
`doUpdateMatrix` `0x0274ccc4`), which the paint path updates again from the same pos/at. Camera
execute (in Ex) writes eye/center/up/bank/fovy; nothing downstream needs more.

**The gate's skipped call runs the draw phase.** `LogicGate::payload` words 14-39: a frame that
saves the link register the stub's `mflr r0` left in r0, sets the skipping word (counters `+8`),
`MtxInit` `0x0200fac4`, `fpcDw_Handler(0x025df908, 0x025de2cc)`, both through `ctr`, clears the
skipping word, then returns.

**A draw phase is not pure: the scene's draw advances the tick.** HD's `dScnPly_Draw` is
`0x025af8a0`, TWW's (`d_s_play.cpp`) in the same order: the scene's view (`0x0276c9b4`), collision
`Move` (`0x0200e558`), `Bgsp` `ClrMoveFlag` (`0x024ee9c8`), stage-change requests, then under
`!dMenu_flag() && pauseTimer == 0` vibration, grass/tree/wood/flower execute, `Bgsp` `Move`,
particle `calc3D`/`calc2D` (`0x025a81a0`, `0x025a8148`) and the frame counter `0x101ff560`++, then
`MassClear`, `calcMenu`, and only then the actor draw loop (`0x025b0234`) and the grass, tree and
attention draws. Run twice per tick, all of that ran twice: the frame counter rose 41 over 20 ticks
(`scratch/drawphase/counter.py`). The pause guard is not usable for the skip: its else branch,
`dVibration_c::Pause`, cancels rumble and camera-shake patterns.

So the scene gate (`LogicGate::scenePayload`, a standing probe at `0x025af934`,
`addi r3,r3,0x26a4`) sends a skipped call's scene draw from its view straight to the draw loop;
no register set between the two is read after the loop, and the epilogue reloads r25-r31 from the
frame. After it: the counter rose 50 over 50 ticks, and idle Link with interpolation off repeats
exactly on in-between paints (`0, 21742, 0, 19169` pixels changed; `133` and `83` before, from the
doubled particles).

Draw methods that still advance state, from the decomp (874 draw functions scanned): the Z-target
cursor's animation (`dAttention_c::runDrawProc`, in the scene's attention draw), Puppet Ganon's
smoothing (`d_a_bgn`), fireflies (`d_a_ff`), the grappling rope (`d_a_himo2`) and Jabun's cave
flash timer (`d_a_obj_ajav`). On the in-between paint these run a second time per tick.

**Ordering.** A draw phase can only show a tick it has, so the picture lags one tick: the tick's own
draw phase shows the midpoint of the previous and current inputs, and the in-between frame's draw
phase shows the current inputs. Shown in order: mid(n-1, n), n, mid(n, n+1), n+1.

**Actors.** HD's `fopAc_Execute` (`0x025d475c`, asserts name `actor->current.pos`) copies `old` =
`current` before the actor runs: `old` at `+0x300`, `current` at `+0x314` (pos, then angle), TWW's
offsets plus `0x11c`; `shape_angle` is `+0x328` (`lha 0x32a`, its y, is the most-read halfword of the
group). An actor the title did not execute this tick has condition bit `0x2` (`+0x2e4`) and a stale
`old`. `fopAc_Draw` is `0x025d4654`.

**The mechanism** (`title/DrawInterpolation`): with the gate in, the tick's draw phase runs with
each camera's eye, center, up, fovy and bank, and each executed actor's `current.pos` (from `old`)
and `shape_angle` (from the previous tick's draw), at their midpoints; AfterOfDraw puts the tick's
own values back. The skipped call's draw phase draws them as they are.

**Measured** (`scratch/drawphase/pan.py`, gameplay, mode 13, gate on, right stick held, 5
consecutive presents per arm): the image's horizontal shift between presents was `0, -124, 0, -124`
px with interpolation off and `-64, -64, -60, -60` with it on. Counters over the run: 117 camera
blends of 118 ticks (1 first sight), 17,278 actor blends, 2,074 actors not executed, 0 unblendable,
unreadable or failed writes.

**Models.** HD's `J3DModel::calc` is `0x027f4d5c`: base TR matrix at model `+0xc8` (base scale
`+0xbc`), each joint's mtx-calc object through vtable `+0x24`, then the skeleton's world matrices
(`0x027db1d4`). The skeleton is model `+0x2c`; its world matrices are `*(skeleton+0x10)`, `0x30`
bytes each, u16 count at `+0x2c`. The model's view pass `0x027f55fc` (only in a draw phase) then
multiplies them by the camera. Measured over the walk (gate on): of the models drawn in the tick's
draw phase, 11,133 of 12,099 had their world matrices calculated in that draw phase
(`mDoExt_modelUpdateDL` style); the rest (the seagull's `daKamome_setMtx` style) in execute, so an
actor's blended `current.pos` cannot reach them.

So at the view pass the mechanism blends the world matrices themselves, with no per-actor knowledge:
the skipped call's draw phase records each model's base `B` and joint matrices `W` (the tick's own);
the next tick's draw phase writes `B_mid * mid(B_prev^-1 W_prev, B^-1 W)` per joint, the animation
blended in model space and the placement apart. `B_mid` is `mid(B_prev, B)`, unless that model's base
differed between the tick's draw and the skipped call's: then it was set in the draw phase from an
actor's blended placement, is the midpoint already, and is used as it is. Morphs and any per-joint
callback are inside `W`, so they are blended too.

**Measured** (`scratch/drawphase/idle.py`, Link idle, camera still, 5 consecutive presents per arm,
pixels changed in the centre box): off `5233, 133, 4870, 83`, on `2832, 3116, 2701, 2373`. Walk
(`scratch/drawphase/run.py`): 11,958 model blends, 628 with the base from the draw, 0 unblendable,
unreadable or failed writes; frames render without seams.

Not interpolated, so still stepping at the tick rate: anything a draw method takes from state other
than these inputs (material and texture animation, particles positioned in execute), and camera cuts
and actor teleports, which blend across the cut for one present. The game has no cut flag:
`dCamera_c::Set` is called every frame by event cameras and `Reset` by a handful of actors, so a cut
is not told apart from a fast move.

## The deleted mechanism, and where its evidence went

The shipped mechanism used to be a host-side statistical lerp. It identified the camera by **searching
shaders for a 3x4 that moved like one**, matched each draw's identity across ticks by **uniform-block
address and occurrence index**, guessed which vertex buffers belonged to the same object, blended what
it found value by value, and re-issued a recorded frame under a guest-state guard's shadow and restore of
every render target.

**Condition 5's seven named items are not seven things.** Deleting `interp/TransformSearch.h` first broke
the build not in the runtime but in `interp/SharedTransforms.h` and `interp/AssemblyKey.h` -- both of
which condition 5 names as well. Walking the include closure from there took 41 files: the seven named,
the eleven the closure forced, the two light-map units (`MapPassValues` and `LightLookUp`, which are
what "the light-map rebase fixes" names), and the shared-value lerp.

**The shell's presentation-mode decision changed and its reason did not.** A title that presents more than
once a frame needs each present on its own vblank -- FIFO puts each present on its own vblank, and
immediate or mailbox would show them back to back and drop one. That was read from the interpolator and is
now read from the paint mod, because the paint mod is what presents more than once per frame. A route
that keeps a name keeps an answer, and a decision that keeps its reason keeps its shape.

**Evidence is deleted with the evidence's mechanism, not reused.** The twenty thousand words this
document used to carry about objects left un-blended, partners found by values, turns taken as
midpoints, pixel stages, shadow volumes, hair meshes, ripple rings, cloud cards and neighbour checks were
measurements of a mechanism that no longer exists. They are in the history, and no row of
`docs/project-state.md` claims them any more. **What remains is stated as it is: a picture rate of
59.99 a second measured through the guest's own path, a logic rate of 30.00 held there by a gate, a
located pose with a located identity, and no blend.**

## Renderer scope

The mechanism is the Vulkan renderer's. The assembly site the deleted mechanism used was
`VulkanRenderer::uniformData_updateUniformVars`; the paint path reaches the guest's display thread, which
is renderer-independent in principle, but the hooks that observe a frame's end and its present live in
`LatteFrameHooks`, which is the Vulkan path's.

## What the runtime counts

`GET /counters` reports how far each runtime submission got -- submissions, packets the command processor
walked, draws issued -- and what the recorder took from the guest's own frames. `GET /paint` reports the
paint mod's own state, its mode, whether the stand-in is installed, the swap interval asked for, the
pacing that took, and its paint count. `GET /logic` reports the gate's own counters, read from guest
memory. `GET /draws` the per-vertex-shader census of draws
whose vertex bytes the title rewrote; `GET /pacing` the intervals between displayed frames with p50, p95,
p99 and longest. `POST /recordings?frames=K` keeps the next K frames' assemblies and `GET /recordings`
returns them, for offline questions the counters cannot answer.

The channel's route table is asserted against its own dispatch, and a route that was deleted is asserted
still deleted: a refusal that advertised a route the channel does not serve would send a reader to a
channel that does not answer.
