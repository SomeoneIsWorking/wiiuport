# Frame interpolation: the mechanism

How `wiiuport` presents frames between two guest simulation ticks. This document owns
the mechanism. What any value *means* belongs to the consuming title.

## What the guest gives us

Guest GPU work reaches the runtime as **display lists in guest memory**. The command
processor sees `IT_INDIRECT_BUFFER` packets carrying a physical address and a size in
dwords, and walks the referenced buffer (`LatteCP_itIndirectBuffer`,
`LatteCP_processCommandBuffer`). A frame ends at `LatteRenderTarget_itHLESwapScanBuffer`,
which bumps `LatteGPUState.frameCounter` and calls `Renderer::SwapBuffers`.

Two consequences decide the whole design:

1. **A frame's draw stream is re-runnable.** It is data in memory, not a consumed
   stream, so the same frame can be issued a second time with different inputs.
2. **All uniform data for a draw is assembled in one place.** Whichever way the guest
   supplied it, `VulkanRenderer::uniformData_updateUniformVars` gathers it into a single
   buffer immediately before upload. That covers both of Latte's uniform modes:
   `FULL_CFILE`, where values come from the ALU constant registers that
   `IT_SET_ALU_CONST` wrote (inline in the display list), and `REMAPPED`, where they are
   loaded from uniform buffers in guest memory. Substitution happens there, on the
   assembled copy, after the guest's values have been read and before the GPU sees them.

## Where substitution happens, and why not in the display list

An earlier reading of the fork suggested patching the recorded display-list bytes
directly, because `IT_SET_ALU_CONST` carries its values inline. That is workable for one
of the two uniform modes and wrong for the other, and it would make the runtime care
about packet layout. Substituting at the assembly point instead is strictly better:

- it is one site rather than one per uniform mode;
- it never writes to guest memory, because it edits a buffer the runtime owns;
- it sees uniform-register and uniform-buffer values in the same form, so a consumer
  describes a transform once rather than once per path.

Recording the display lists is still needed — but only to *re-issue the draws*, not to
carry the substituted values.

### The exact site, read from the fork

`VulkanRendererCore.cpp` assembles uniforms in **two** functions, not one:

| function | line | used for |
|---|---|---|
| `VulkanRenderer::uniformData_updateUniformVars` | 375 | the full path |
| `VulkanRenderer::uniformData_updateUniformVarsIncremental` | 452 | the fast draw sequence |

Naming only the first would have repeated the display-list mistake at a different
layer: a title that mostly takes the fast path would have most of its draws pass
through unsubstituted. Both end on the same statement (lines 449 and 496):

```cpp
dynamicOffsetInfo.uniformVarBufferOffset[shaderStageIndex] =
    uniformData_uploadUniformDataBufferGetOffset({(uint8*)uniformBuf, shader->uniform.uniformRangeSize});
```

The hook is therefore one private helper taking `(shaderStageIndex, shader, uniformBuf)`,
called from both sites immediately before that upload. It is **not** placed inside
`uniformData_uploadUniformDataBufferGetOffset`, for two reasons: that function has no
`shader`, so it cannot know where anything lives in the buffer; and it has a third
caller at `VulkanRenderer.cpp:3264` which uploads Cemu's own output-shader uniforms,
which are not guest state and must never be blended.

### Where the values sit inside the assembled buffer

`shader->uniform` carries the layout, and the two modes land in the same buffer:

| field | meaning |
|---|---|
| `loc_uniformRegister`, `count_uniformRegister` | the ALU-constant block, `count * 16` bytes |
| `loc_remapped` | the remapped uniform-buffer block |
| `uniformRangeSize` | total assembled size, and the span that gets uploaded |

One detail that is easy to get wrong: these `loc_` values are **byte** offsets, and the
function indexes floats with `uniformBuf + (index / 4)`. A substitution that treated
them as float indices would write at four times the intended offset and corrupt
unrelated state rather than fail visibly.

## Recording

Between two swaps, record every `IT_INDIRECT_BUFFER` the frame references as
`(physical address, dword count, copy of the contents)`. The copy is not an
optimisation to remove later: the guest reuses and overwrites that storage as soon as
the frame is submitted, so a reference alone would replay whatever the next frame wrote.

Alongside it, record the assembled uniform buffer of every draw, keyed by the draw's
position in the frame. Those recordings are what a consumer's blend reads: tick N-1's
assembled values and tick N's, for the same draw.

## Presenting

This section previously claimed that a 30 Hz title already presents at 60 Hz with every
second flip repeating the previous image, so interpolation could fill the repeat slot
without adding a present. That is wrong, and reading the fork rather than reasoning
about vsync is what caught it.

What `swapInterval` actually does, in `LatteTiming_signalVsync`
(`src/Cafe/HW/Latte/Core/LatteTiming.cpp:77`), is gate the **guest's** flip accounting:
a counter reaches the interval and `flipExecuteCount` advances, which is how the title
is held to 30 Hz. It performs no presentation.

Host presentation happens elsewhere and exactly once per guest scan-buffer swap:
`LatteCP_itHLESwapScanBuffer` → `LatteRenderTarget_itHLESwapScanBuffer`
(`src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp:679`) → `g_renderer->SwapBuffers(true, true)`.
So at `swapInterval=2` the title produces 30 frames a second and Cemu presents 30 times
a second. There is no duplicate present, and nothing to reuse.

**Interpolation therefore has to add a present.** Between consecutive guest swaps the
runtime replays the recorded stream with the consumer's blend of ticks N-1 and N
substituted at the uniform assembly site, and presents that as an additional frame:

1. Guest swap N arrives; present it as today.
2. Before guest swap N+1, replay the recorded stream for N with blended transforms and
   present the result.

The cost is one extra scene render **and** one extra present per tick, not just the
render. The latency cost is the half-tick the blend inherently needs, because a frame
between N-1 and N cannot be drawn until N exists.

Adding a present means the runtime owns its own pacing between guest swaps, which the
repeat-slot model would have got for free from the existing vsync cadence. That pacing
is a real piece of work and is not hidden inside "substitute and replay".

The obvious later optimisation — replaying only the passes that depend on the
substituted transforms and reusing the rest — is an optimisation, not the design. It is
only safe once the null-interpolation gate below holds for the full replay.

## The logic gate, and the four things that had to be true for it to run

The logic tick is at 0x025d42ec. A gate in front of it skips every other call, so the
picture can be painted twice per tick while the simulation still runs at its own rate.
It is reached by the probe that already holds the tick's entry: the probe's stub builds
the gate's block address in a register and branches through the count register, so a call
arrives at the gate and the title's own code is never written at all.

That last part is the point. Nothing is written into 0x025d42ec, and a branch there does
not work anyway: a branch written by the host into a guest function's *interior* is
compiled to a jump to a host address the recompiler never produced, so it does not
arrive. Not as `b`, not as `mtctr`/`bctr`, not after forcing translation. The probe's own
stub branch is the one way into the trampoline area that is known to arrive, and the
caller census has counted 2020 calls a run through it.

Four defects stood between that and a working gate, and each is worth stating because each
read as something else:

**The through path skipped an instruction.** It supplied the tick's prologue and branched
to the word *after* the tick's second instruction. Correct for a gate entered at that
instruction, wrong for one entered at the entry, where the stub has already run the first.
It skipped `stw r0,0x4(r1)`, and the tick's epilogue returns through the link register
that instruction saves. The title then spun in a wait loop at 0x027f09d8 forever, with the
control channel still answering and the paint count stopped at one. Found by reading the
guest's own words through the channel: the block held 0x497cea64, a branch to 0x025d42f4,
where the instruction is at 0x025d42f0.

**An unfilled block is not a disabled gate.** The probe sends every call to the block
whether the gate is in or out, and a freshly allocated block is zeroes, and a zero word is
an illegal instruction. So an un-gated gate did not run the tick, it raised a guest
exception inside the simulation and the product died in the scheduler, naming nothing of
this. "Out" is a payload of its own now: one branch to the instruction after the entry.

**The counters were in memory the guest cannot touch.** They came from the emulator's
system area, which is host bookkeeping. A gate that had been unreachable reported no calls
and no fault; the first run that reached its own stores died on the first one. A counter
that never moved and a counter that faults look the same from outside, which is why the
block's address space is now stated rather than left to be discovered.

**A probe's stub branched to its resume directly, so a rewritten block never ran.** That
branch compiles to a jump to the target's *host* code, resolved once when the branch was
translated. Rewriting the target afterwards produces new host code at a new address and
leaves the branch jumping to the old one; nothing invalidates it, because nothing was
written where the branch is. The guest runs the payload that was there when the branch was
first translated, forever.

The discriminator for that last one could not be argued with: a payload whose final word
branches to its own first word — an infinite loop if it runs — was installed, and the
title carried on at 30 paints a second for four seconds. The words in guest memory were the
ones just written, and the counter read zero. With the branch indirect, the same payload
stops the title dead (2025 calls to 2025, paints unmoved) and the gate's own counter
begins to climb.

Measured on the real title through the control channel, in one driven run: 30.17 paints a
second (181 over 6.0s) with the gate out, 30.00 (180) with the gate in, and the gate's own
counter from 30 to 151 in 3.0s against a probe call rate of 40.33 a second over the same
window.

## Sixty paints a second, through the title's own paint path

**The conclusion this section reached — that the picture reaches sixty without the logic
following it — is withdrawn.** It was measured with a stand-in that paints the tree twice per
pass, which faults on the display thread's core, and with a logic rate read from a counter
that was counting the calls the gate *skipped*. Both are described below with what replaced
them: mode 6 paints once per pass and reaches the same sixty, and the tick is called once per
present, so the logic rate follows the flip one for one. This section is left in place because
the numbers in it are real and the reading of them was not.

The gate is not what doubles the picture. The stand-in does that, and the measurement is on
the real title through the control channel, in one driven run, in the same scene, with the
mod off and on in adjacent windows:

    window off:  30.12 paints/s  (241 paints over 8.0s)
    window on:   60.12 paints/s  (481 paints over 8.0s)

The logic rate reads 30.12 in both windows, from the probe on the tick's entry — inside the
29.9-30.0 band the gate exists to hold. So the picture reaches sixty **without the logic
following it**, and the gate is a backstop rather than the mechanism. That is the finding:
the title's display thread paints as often as it is asked to, and the simulation runs at its
own rate underneath. A second run of the same shape gave 59.40 (297 over 5.0s) with the
logic at 29.80, so 60.12 and 59.40 are two samples of the same rate, not one of them.

### The flip-skip risk, resolved by reading the field per paint

The frame is documented to do `if (display+0x74 & 1) display+0x74 ^= 2`. If that fires
between the two paints of a pass, one paint of the pair presents and the other does not, and
the rate would read sixty while the display presented thirty.

A rate cannot answer this — both cases present at the same rate — and neither can a field
read once on request, which reads whichever value the last paint left. So the probe samples
`display+0x74` and `display+0x28` at **every** paint, under the same lock as the display
pointer, and the report carries the pair. Measured, 24 samples a quarter of a second apart
in each state, on a real title:

    stand-in out:  paints 1575, flags 0x0, phase 0x2, interval 2
    stand-in in:   paints 1877, flags 0x0, phase 0x2, interval 1

**One distinct value per window, held across roughly 300 paints: `display+0x74` is 0
throughout, so `flags & 1` is false at every paint the probe saw and the toggle never
executes. No paint is left without a flip, and the risk does not apply to this title's
frame.**

The same readings also say the probe is reading live values rather than a constant: the
interval field at `display+0x50` moved from 2 to 1 across the arming, which is exactly the
pacing change the stand-in asks for, in the same two readings where the flag did not move.

### Which stand-in does it, and which ones fault

The mode numbers are the product's own mapping, read from `modeFrom`: 1 `PassThrough`,
2 `Twice`, 3 `TwiceAtSixty`, 4 `IndirectOnce`, 5 `IntervalField`, 6 `OneAtSixty`,
7 `BranchEntry`. The stand-in that presents at sixty is **mode 3, `TwiceAtSixty`**: set the
pacing, then call the frame twice, once a vblank a flip. Its payload is five words lifted
from the title's image.

Four modes run identically, arming only the paint mod:

| mode | payload | result |
|------|---------|--------|
| 1 `PassThrough` | tail branch to the frame, one paint | survives, 30.00 paints/s |
| 2 `Twice` | `bl` the frame twice, back to back | **faulted** |
| 3 `TwiceAtSixty` | set pacing, `bl` the frame twice | **faulted once, then 60.12 paints/s** |
| 6 `OneAtSixty` | single branch, one paint at one vblank | survives |

The two mechanisms are independent: the gate runs, and the paint mod's second mode does not.
That is also why they are now armed separately (`probe_run.py --arm paint|gate|both`) — they
are two different questions, whether the picture rate reaches sixty and whether the logic rate
follows it, and a run that arms both has measured neither on its own.

**The fault is intermittent, not a property of a payload.** Mode 3 killed the product in one
run and presented at sixty in the next, with the same binary and the same request sequence.
That is the honest reading of this table, and it retracts what the first version of it said:
the earlier claim that the trigger was "doing the work back to back rather than once a
vblank" does not survive mode 3 both faulting and working, because `TwiceAtSixty` is
precisely the once-a-vblank payload. What is left is a race whose window the run reaches
sometimes and not others, and its mechanism is not established.

### What is still open: the fault is intermittent

Correcting an earlier reading of this, which blamed the gate. **The paint stand-in alone
faults, with the gate never armed.** Arming it at 71 seconds killed the product within three,
and the fault is a segmentation fault inside recompiled code on the display thread's core
(`OSSched[core=1]`). It does not happen every time, and it is not tied to a payload: mode 3
faulted in one run and presented at sixty in the next.

### The null case, and it fails

Two paints of one pass must be the same picture; if they are not, a blend built on "the
in-between frame is between the neighbours" has no meaning. This needed a capture capability
that did not exist: the renderer holds one screenshot request at a time, so two arms of one
is not two presents — the second waits a whole frame, and a title that animates gives a
different picture. That is what the first attempt measured, 3,515,979 of 6,220,816 bytes
differing between two captures a frame apart, which says nothing about two paints of one
pass. The fork's capture now takes a count and re-arms as each image lands, so a run is
consecutive by construction. Three tests cover the count reaching the fork, the images landing
in consecutive slots, and a run that will not fit being refused by name.

Measured on the real title with the stand-in in, three rounds, each a run of two consecutive
presents over 6,220,816 bytes:

    round 0: 1625688 bytes differ (26.1%), first at 109096: 0x03 against 0x00
    round 1: 1625688 bytes differ (26.1%), first at 109096: 0x03 against 0x00
    round 2: 1287146 bytes differ (20.7%), first at 132121: 0x23 against 0x07

**0 of 3 identical. The null case fails: two paints of one pass are not the same picture on
this title.** The captures are genuinely consecutive — one paint between the pair in two of
the three rounds — so this is not the old straddling-a-tick measurement with new numbers: it
was 3,515,979 bytes differing before, 1,287,146 to 1,625,688 now, and the capability is what
changed.

The mechanism is not established and is not guessed at here. A title that updates its HUD or a
water animation between the two paints of a pass would look exactly like this, and at sixty
paints a second the two are 16.67 ms of title time apart. What it means for the blend is not
yet decided: either the null case is a limitation of the comparison rather than a property of
the mechanism, or the two paints have to be taken close enough together that the title's own
animation between them is below the comparison's sensitivity. Neither has been tried.

### What is established about the fault, and what is not

- The payload is correct. The product logs the words it wrote, because arming is the step
  after which nothing may survive to be asked: `0x499469e5 0x499469e1 0x49946798` at
  0x00e05880, which is `bl 0x0274c264` twice (link bit set on both) then
  `b 0x0274c020`, the display loop's top. Two paints per pass, as the mode intends.
- The wiring at rest is as expected: vtable slot 0x10004f54 holds 0x0274c264, the frame
  entry holds a branch to the paint probe's stub at 0x00e05868, and the stub is
  `[HLE, mfspr r0,LR, lis r12, ori r12, mtctr r12, bctr]` to 0x0274c268.
- Sampled twice three seconds apart at the fault, the display thread is at guest pc
  0x00e0586c — the stub's displaced instruction — with r0 already holding 0x00e05888, the
  stand-in plus eight. It is not moving. That is a thread stuck inside the stub, not a
  memory fault, and the distinction matters: nothing is out of bounds.
- Taking the display thread's probe lock across the block write and the vtable patch is
  correct on its own terms — a thread already inside the stand-in is running the function
  that write invalidates — and it is in. It did not cure this fault. It was tried first as
  the whole explanation, which was wrong; the write is not the only thing that changes what
  the display thread is running.

What is not established is which of the remaining candidates it is. The recompiled function
covering the stand-in's block is deleted and recompiled while the display thread is inside
it; the block sits 24 bytes from the probe stub in the same bump-allocated arena, so one
translation unit may cover both; and the stand-in calls back into a frame whose entry is
itself the probe's, so each pass re-enters the probe. Separating those needs a run each, and
each run is minutes, so this is recorded as open rather than guessed at. What is *not*
available as an answer is arming the two in an order that happens to survive: a measurement
that needs its order chosen is not a measurement.

### The fault, twice: one cause found, and the trigger is painting twice

Three runs under gdb, with the paint mod armed and the gate never armed, and one control with
the mod never armed. The control is the part that makes the rest mean anything:

    mod never armed, 210s, gdb attached:      no fault
    mode 3 armed:                              fault, PPCRecompiler.cpp:148
    mode 3 armed:                              fault, PPCInterpreterImpl.cpp:72

**The fault needs the patch.** A core that never runs the stand-in does not reach it in three
and a half minutes with a debugger attached, so this is not a free-standing defect in the
emulator that the patch merely runs into.

**The proximate cause, in the fork, is a missing check.** The direct jump table is sparse: it
covers the whole code area, but only 4 MiB at a time is mapped, and only for a range the
loader registered with `PPCRecompiler_allocateRange`. Three functions indexed it with an
address the guest supplied and asked nothing — `attemptEnterWithoutRecompile`,
`attemptEnter` and `visitAddressNoBlock`. At the fault:

    enterAddress 0x02c12ffc          a legitimate-looking address in the title's code area
    ppcRecompiler_reservedBlockMask 0x749   blocks 0, 3, 6, 8, 9, 10
    block of 0x02c12ffc            11      the one the mask has no bit for

So the read was 92 MB into a 512 MB host reservation, in a block that was never mapped, on
the scheduler's own resume path — which runs for every core at every timeslice, so any guest
address can arrive there. Fixed in the fork (`23eb318`): the block is asked about before the
table is read, an address with no block answers "not translated", and the mask is published
after the mapping it promises rather than before. The sites inside the recompiler worker are
left alone; their addresses come from a registered range, so their blocks are mapped by
construction.

**With that fixed, the same run gets one step further and trips on the program counter
itself**, in `PPCInterpreter_LWZ` at an instruction word of 0x800006e2 read from guest address
0x00e000b28 — the base of the loader's trampoline arena. So the second cause is upstream of
the crash: **the display thread's program counter leaves the title's code while its link
register points into the stand-in.** The link register is 0x00e05888, which is the stand-in's
second word, the first `bl` of the display frame, so the thread was between the two paints of
a pass when it happened. Two runs gave two different addresses — 0x02c12ffc and 0x00e000b28 —
which is what "the program counter is wrong" looks like rather than "the program counter
reached somewhere particular".

**The trigger is painting twice, and mode 6 does not do it.** Modes 1 and 6 survive; modes 2
and 3 fault. Both faults have the link register inside the stand-in, and mode 6's payload
never enters the stand-in's second paint at all because it never has one. So the honest
statement is: *painting the tree twice in one pass leaves the display thread's state
inconsistent, and the mechanism is not established.* What the run now rules out is the
specific candidate this document previously led with — the recompiler's own table read — which
was real and is fixed, and was not the whole of it.

That matters for the shape of the mechanism rather than only for the fault: **mode 6 reaches
the same sixty without painting twice**, so the sixty does not depend on the double paint at
all, and the double paint is not load-bearing for anything.

## Sixty paints a second with one paint per pass, and the tick slaved to the flip

`OneAtSixty` — mode 6 — is a payload of one word, a direct branch to the display frame, with
the swap interval at one vblank a flip. The display thread paints the title's own tree once
per pass, through the title's own vtable slot, and the pass runs twice as often. The logic
gate holds the tick, and the gate's own two guest counters — incremented by its own
instructions, read straight out of guest memory, so nothing in the report can flatter them —
say how many calls it saw and how many it let run.

    unmodded, no mods:              241 paints in 8.00s = 30.12/s
    unmodded, gate armed:           241 paints in 8.00s = 30.12/s
                                    241 tick calls, 121 ticks run = 30.12/s and 15.12/s
    mode 6 + gate, window 0:        480 paints in 8.00s = 59.98/s
                                    480 tick calls, 240 ticks run = 59.98/s and 29.99/s
    mode 6 + gate, window 1:        480 paints in 8.00s = 59.98/s
                                    480 tick calls, 240 ticks run = 59.98/s and 29.99/s

**That is the condition: sixty paints a second with the logic tick inside 29.9-30.0**, in two
independent eight-second windows, each read as two counts a window apart rather than as a
running average, with the unmodded rate measured in the same run for the modded one to be a
change from.

The counters corroborate it independently, and they are half exactly:

    unmodded:  calls 60   ticks 30  |  three seconds later:  calls 150  ticks 75
    mode 6:    calls 692  ticks 346

**And the tick was slaved to the flip, which is the case the condition anticipates.** With the
gate out, the tick's entry at `0x025d42ec` is reached 480 times in the same 8.00 seconds as
480 paints — one for one, at both rates. The gate is therefore not a backstop here: presenting
at sixty calls the simulation sixty times a second, and holding the logic at thirty is what
the gate in the logic path is *for*. The earlier reading of this, that the logic rate held at
30.12 while the picture reached sixty, came from a counter counting the calls the gate skipped
and from a stand-in that faulted before the flip was at sixty; it is withdrawn above.

### The gate could not answer the question it exists for, and now can

Two defects in the gate, both found by reading its counters out of guest memory rather than
trusting the report, and both of which made the gate unable to answer:

- **The ticks counter counted the calls that did not run.** It was incremented on the skipped
  path while the report calls it "ticks through it", so a gate letting every call through
  reported 0 — which is exactly what it did, in every run, at both rates. It is now incremented
  on the path that runs the tick, and the count of skipped calls is calls minus ticks.
- **The pass-through control branched to itself.** Flavour 2 loaded the *block's* own address
  into the count register and branched to it. Measured: the display painted 0 times in an
  8-second window with the control channel still answering, which reads exactly like a title
  that has stopped and is not one. It now loads the address the direct form branches to.

And two more that only the block's own words showed, both of which hung the title in the same
way — 0 paints in an 8-second window, every counter frozen — and neither of which any count
would have distinguished from a title that had stopped:

- **A displacement measured from the wrong word.** Growing the through path from one word to
  six left the branch at its end no longer at its start, and the displacement was still
  measured from the start. Read back from guest memory, the branch landed at `0x025d4304`,
  twenty bytes past the tick's second instruction at `0x025d42f0`. Fixed by a separate
  constant for where the branch *stands* (`kBranchWord`, 13) against where the through path
  *starts* (`kThroughWord`, 8).
- **`bne` was the wrong branch.** `kBranchNotEqual` held `0x40800000`, which is BO=4 **BI=0**.
  BO=4 branches when the bit BI names is *false*, and `andi.` records a non-zero result in
  CR0's bit 2, which is BI=2 — so `bne` is `0x40820000`, and BO=4 BI=0 is a different branch
  over the same displacement. Four `bne` in the title's own image agree (`0x025d4398`,
  `0x025d4678`, `0x025d46a4`, `0x0274c964`, all `0x4082....`) and none carries `0x40800000`
  as a `bne`; the file's own comment three lines above the constant already named
  `bne 0x0200004c` = `0x40820014` as the instruction it came from. The constant contradicted
  the comment.

**Both of the last two were invisible to the tests because the tests reused the code's own
expression** — the displacement from the same variable, and the branch word from the same
constant. An expectation built from the thing under test cannot catch the thing under test
being wrong. The expectations are now built from the branch's own address and from its **BO
and BI fields**, read back the way the guest reads them, and both mutations are checked:
restoring `0x40800000` fails two checks and restoring `through` fails one.

### A capture run was returning the previous run's images

Two rounds of a byte-for-byte comparison came back with *identical* checksums and a paint
count that had not moved, which is what a stale pair looks like and what a re-capture does
not. A slot keeps whatever last landed in it, so polling one and taking the first non-empty
body returns the previous run's image whenever the new one has not arrived. That is not a slow
read, it is a different answer.

`capture_run` now waits on the product's own `imagesReceived` watermark — the count of images
it has handed over, which the answer to the arm reports — before it reads a single slot, and a
run that does not get its images is a refusal rather than a short tuple. The watermark is read
out of `/counters`, which is where the capture counts are; `/setup` is the first-run setup
status with four fields in it, and reading it returns a `None` that looks like a report. Four
tests cover it, including that a missing watermark is refused rather than read as zero.

**With the images fresh, two consecutive presents are not the same picture** — and the shape of
the difference is the useful part:

    round 0: 1,443,965 of 6,220,800 bytes differ (23.21%), largest delta 23, 1,429,852 within 4
    round 1: 1,972,143 of 6,220,800 bytes differ (31.70%), largest delta 30, 1,944,333 within 4

A quarter to a third of the bytes, and almost all of them by at most 23 to 30 levels. That is a
different frame of the same picture, not a broken one: a widespread, low-amplitude change is
what a title's own animation and lighting do between frames, where a fault would be large
deltas over a small region. And it is expected, because with the tick once per present the
consecutive presents are a tick apart.

**So the null case is a different question from this one, and both have to be in the report.**
The objective asks whether **two paints of one pass, no blend, are identical**. Two
*consecutive presents* are two different ticks' pictures, and with the logic at thirty a second
they must differ. Mode 6 paints once per pass, so it cannot produce a same-pass pair at all;
mode 3 can, and faults. The same-pass comparison is therefore still owed, and the shape
analysis — the same pair against the pair a tick apart, with the paint count over the same
window — is the read that settles it.

## The blend: which node field holds the pose, and when it is written

Located, not searched for. The per-object uniform block binder at `0x027ff88c` is one of
the two sub-objects a node's draw calls, and all it does is work out where the object's
block lives and bind it per stage:

```
entry = object + 0x10 + *(int *)(object + 0x4c) * 0x1c;
GX2Set{Vertex,Geometry,Pixel}UniformBlock(index, *(u32 *)(entry + 0x0c), *(u32 *)(entry + 4));
```

So identity is the node, and the node's own descriptor is where the block is named. Two
things were read out of that rather than guessed at:

- **The block's address is the descriptor entry's word at `+0x04`.** The two slots of
  every object measured are exactly `0x100` apart, five objects in a row, and the word at
  `+0x0c` the binder hands to `GX2Set*UniformBlock` is `0x40` for every one of them — a
  constant, and so an offset within the block rather than a size. A uniform block is 256
  bytes and its address is in the descriptor.
- **The pose is twelve floats at `+0xc4` of that block.** Three rows of unit length and
  mutually perpendicular to six decimal places, a translation beside them in the same
  twelve words, and a zero fourth float in each group of four, so a 3x4 and not a 4x4
  with a row dropped.

**And whether the previous tick's block is still there when this tick paints is answered,
and the answer is no.** The ring turns — 898,547 cursor switches over 3,023,213 repeat
bindings, 0.297 per binding — but reading both slots whole finds the *other* slot zero at
the pose's own offsets. The two readings were different findings and the earlier one was
wrong: the ring turning is not the previous tick's values being in memory, so a blend
cannot read N-1 out of the ring. That claim is withdrawn in `project-state.md` and in the
title's own document.

### So the blend has to capture, and the question is where

Both ends of the lerp have to be in memory at one moment, and the ring does not hold the
previous tick. So one of them has to be kept — and *where* is decided by something the
code does not say: whether the title writes the pose **before** the binder runs or after.

- **Written before.** The value at the binder is this tick's and the value kept from the
  previous binding is the last tick's. Both are in memory at the binding, the binder is the
  only place the mechanism has to touch, and the in-between frame is a lerp of two reads.
- **Written after.** The value at the binder is the *previous* tick's, and the current
  tick's has to be read where it is written — a different probe at a different address,
  found by asking who fills the descriptor. The title's own document names that as the open
  read, and this decides whether it is still open.

**And measured, the answer is the second one, and more firmly than the offset was wrong.**
`title::ObjectPoseHistory` reads the block the binder names, on the display thread inside the
title's own draw, and over **236,694 bindings it found no rigid transform at `+0xc4` in any of
them** — 0 pose-shaped, 236,694 not. So the whole block was then checked: **233 whole-block
scans of 4 distinct blocks, every 4-aligned offset, and zero rigid transforms anywhere in any
of them.** No offset, no transform, at the moment the block is bound.

That is not a wrong offset. It says the block the binder names **holds its pose after the
frame's draw has filled it and not when it is bound** — which is the "written after" answer,
and it means the binder is the wrong place for a blend to read the pose from. The title's own
document found the twelve floats at `+0xc4` by dumping a block at a moment when the title was
held at a frame's end, which is after the fill; the memory is real and the moment was not the
one a blend runs at. **The claim that `+0xc4` is where the pose is, as a statement about the
block at bind time, is withdrawn.** The block's address is still located and still 0x100
between a ring's two slots; what is withdrawn is that the transform is in it then.

So the next read is the one this document's own earlier section named and that the measurement
has now made unavoidable: **who fills the block.** The binder reads the descriptor; something
writes the block, and that something is where both ends of the lerp have to be read from.

### The filler is not on the binder's own object, and that is measurable

The binder is a *method on a sub-object* — the object it is handed **is** the sub-object, and
the descriptor's entries are at `object + 0x10`. So the sub-object's other methods are the
candidates that fill what the binder binds, and the method table is **in the image**:

    0x1016ef90: 0x027ff96c   84 addresses
    0x1016ef98: 0x027fba24  112 addresses
    0x1016efa0: 0x027fba94   56 addresses
    0x1016efa8: 0x027fbacc  140 addresses
    0x1016efb0: 0x027ff88c  224 addresses   the binder
    0x1016efb8: end of the table

Five methods, eight bytes an entry, and the binder is the fifth. **The binder is the only one
of the five that walks the descriptor**: four of them have no line mentioning the cursor at
`+0x4c` or the `0x1c` entry stride, and the binder's has exactly one:

```
iVar2 = param_1 + 0x10 + *(int *)(param_1 + 0x4c) * 0x1c;
```

This also settles an earlier note in this document, that the sub-object's methods "are
dispatched through a vtable that has no references to follow". There are no *references* to
follow because the table is reached through a pointer the sub-object carries; the *pointers*
are in the image, and the one that names the binder is at `0x1016efb0`. Following the call
graph was never the way in.

So the filler is not among the binder's siblings, and the ring's two entries exist for the
**bind** — so the GPU is not reading a block that is being written — rather than to carry the
previous tick. That is consistent with everything measured: the cursor turns, the block is
256 bytes, the two slots are `0x100` apart, and at bind time the block holds no transform
anywhere.

### The chain the objective names, confirmed in the code — and the pose is not in that block

The objective's own route is in the title's code, and reading it settles two things at once.
`vtable 0x10036300` slot `+0xc` is `FUN_02160018` (the table holds `0x02160180` at `+0x0c`
and the function's entry is `0x02160018`), 1,536 addresses, and it is the node's draw:

```
uVar1  = *(uint *)(param_2 + 0xc);                  /* which draw record */
puVar7 = *(undefined4 **)(param_1 + 0xa4);          /* the node's record array */
if (uVar1 < *(uint *)(param_1 + 0xa0)) puVar7 = puVar7 + uVar1 * 5;   /* stride 5 words */
puVar7 = (undefined4 *)*puVar7;                     /* the record */
...
GX2CallDisplayList(*(undefined4 *)(pbVar5 + 4));    /* the title's own draw is a display list */
...
iVar8 = *(int *)(*(int *)(param_2 + 0x14) + 4);                            /* the sub-object */
iVar8 = iVar8 + 0x10 + *(int *)(iVar8 + 0x4c) * 0x1c;                     /* the same arithmetic */
uVar4 = *(undefined4 *)(iVar8 + 0xc);
```

So: node `+0xa4` → a record array of 5-word entries → the record, whose word 4 is a pointer
when word 3 is non-zero, and two shorts at `+0xc` and `+0xe` of *that* are read next. Two shorts
two bytes apart is a **range of indices**, which is the "uniform block index" the objective
names. And the node's draw computes the sub-object's descriptor entry **itself**, with the
binder's exact arithmetic, in the same function — so the binder and the draw are two readers of
one descriptor, and following the call graph from either would have found the other.

**And the block that descriptor names is not where the pose is.** The binder's second argument
is the entry's word at `+0x0c`, which reads `0x40` for every object measured — and in the
fork's own `GX2SetVertexUniformBlock` the three arguments are `(index, size, address)` from
`hCPU->gpr[3..5]`, so **`0x40` is the block's size in bytes, not an offset inside a 256-byte
block.** A 64-byte block cannot hold twelve floats, and a 256-byte window around one holds no
rigid transform in any of 233 scans. Both readings now agree, and they agree that the pose is in
a *different* block: the one the record's index range names, addressed through GX2's own
uniform block table.

**Which the fork already holds.** `LatteFrameHooks::UniformAssembly` is the fork's record of one
draw's assembled uniforms, and it carries:

- `data` and `sizeInBytes` — the uniform values the game itself assembled for that draw,
- `blockAddresses` — the guest addresses of the uniform blocks the draw sourced, as
  `(bufferId, physicalAddress)` pairs,
- `stageIndex`, `shaderBaseHash`, `shaderAuxHash`, `writesColour`, `looksUpDepthMap`,
  `fromRuntime`, and, on the `DisplayList` it belongs to, *whose draw this is*.

So the pose is not something to find in guest memory by searching: it is inside the bytes the
game assembled for a named node's draw, and the fork records both the bytes and the identity.
The pose's *offset* within those bytes is found once, by the rigid-transform test over a
bounded set of draws, and is then a constant. **And whether tick N-1's block is still present
when tick N paints is answered by the frame recording the fork already keeps** — which is why
the earlier reading, that the ring must carry N-1 because guest memory does not, was a
consequence of reading the wrong block.

This is the objective's thesis, confirmed with addresses: identity and pose are the game's own
to read, and the thing that guessed at them — searching shaders for a 3x4 that moves like a
camera, matching draws by block address and occurrence index — was guessing at a record the
fork was already writing down.

### The design this forces, and it is the design the objective asked for

Read the two fork records together and every ingredient is already present:

| what the blend needs | where it is, in the fork's own record | why it is that |
|---|---|---|
| **identity, and it is the node** | `DisplayList::physicalAddress` | the node's draw passes `GX2CallDisplayList(record + 4)`, and `record` is what `node + 0xa4` leads to, so the display list's guest address **is** the node's own record |
| **the pose** | a rigid 3x4 inside `UniformAssembly::data` | the bytes the game assembled for that draw; its offset is found once by the shape test and is then a constant |
| **both ends of the lerp** | the frame recording | the previous tick's assemblies are already kept, which is what `ST-60` measured and what this document's earlier "the ring must carry N-1" reading wrongly concluded could not be had |
| **the write** | `data` is writable | the fork's own comment: "this is the point where a transform is substituted, and it is the last point before the buffer is uploaded" |
| **the game's own draw at the lerped pose** | the same point | the substitution happens before the upload, so the game's draw, its display list, its vertex attributes and its skinning all run at the values that were written — none of them is regenerated by the host |

So the in-between frame is: let the display paint path present at sixty (condition 1, measured),
and at the *in-between* present substitute, into `data`, the lerp of the pose this node had in
tick N-1 and the pose it has now — for every draw whose display-list address appeared in the
previous tick. Latency stays the half-tick because the lerp is presented **before** the tick's
own frame, and nothing is recorded and re-issued.

**And every piece of the mechanism this replaces is a consequence of not having had this, not a
preference.** `TransformSearch` searched shaders for a 3x4 that moved like a camera because the
pose's offset inside the assembled buffer was not known; here it is found once and is then a
constant. `AssemblyKey` and `ObjectPlanner` matched a draw in N-1 to a draw in N by uniform-block
address and occurrence index because a draw had no identity of its own; here the display list's
address is the node's record, so identity is the node. `VertexBlend` guessed which vertex
buffers belonged to one object because the host could not ask; here the game's own draw
regenerates every attribute at the lerped pose, so there is nothing to guess. `CutDetector` and
`NeighbourCheck` existed because a *replayed* frame has to be judged against its neighbours to
be believed; a frame drawn at the game's own draw with one value substituted is the title's
frame. And `GuestStateGuard`'s shadow and restore of every render target existed because
re-issuing a recorded frame consumes guest state a second time; nothing is re-issued here, so
nothing is consumed twice and there is nothing to guard.

**What is left to build, in order.** First, the offset: scan `data` for a rigid 3x4 over a
bounded set of draws and read the offset off it, with the count of draws and the count of hits
as denominators. Then the substitution on the in-between present, keyed by the display list's
address. Then the discriminator, whose null case is **two paints of one pass** — the thing
condition 4 asks for and the thing two *consecutive presents* are not, since with the tick at
thirty consecutive presents are a tick apart.

**What this leaves, stated rather than assumed.** The block the descriptor's `+0x04` names is
a 256-byte block in a pool of fixed-size blocks, and it holds no rigid transform at any offset
at the moment it is bound. The title's own document found a twelve-float 3x4 at `+0xc4` by
dumping a block while the title was **held at a frame's end** — after the draw, so at a
different moment from the bind, and the moment is the whole of the difference. So either the
filler writes this block later in the frame than the bind that names it, or this is a
different block from the one the dump was of. **The next read separates those two**, and it is
bounded: hold the title at a frame's end with `POST /gate?pause=1`, then read every candidate
block — the `+0x04` word of each object's descriptor, a pool already known and already
0x100-strided — and test each for a rigid 3x4 at every 4-aligned offset. A hit names the
block and the offset; no hit over the whole pool says the pose is not in that pool at all,
which is a different answer and a shorter search for the next one.

It runs on the display thread inside the title's own draw. For each tracked object it reads the pose
at both blocks the descriptor names, keeps the last three readings with the address each
came from, and counts: bindings seen, comparisons against a previous reading of the *same*
block, how many of those found a changed float, the largest single-float change, and
whether the other slot ever held what the bound block held a binding ago.

**A block that changes address is not compared with the one it replaced.** Comparing across
that would be comparing two objects' poses and calling it one object moving, which is the
same class of mistake as reading a string where a variable was expected.

Two defects the tests caught while it was being written, both of which would have made the
report say something other than what it measured:

- **The reading ring never shifted.** The loop that moved the oldest reading up had an
  off-by-one that made it do nothing, so the second reading of every object was the
  *first* reading of the object before it, and the third onwards was zeroes. A report that
  carries zeroes where a history should be is a report nobody can check.
- **An unreadable other slot was invisible.** The bound block's unreadability was counted
  and the other slot's was not, so a ring that could not be asked looked like a ring that
  was never asked. They are different findings and there are now two counters.

The instrument showed the other answer here, which is what caught the first: a fake that
wrote the pose's first three words at byte offsets `+0, +1, +2` instead of `+0, +4, +8`
reads as a block that does not read at all, so every reading came back unreadable — and the
first version of the test asserted against that, because an assertion written from the code
rather than from the question agrees with whatever the code does.

## The gate that comes before any blending

**Null interpolation.** With the blend forced to the identity at t=1, the replayed frame
must be byte-identical to the original. If it is not, the replay is not faithful and no
blended frame produced by it means anything. This runs before any transform is
identified, and it is the discriminator that proves the replay path is real rather than
merely not crashing.

The negative case must be legible: when a frame cannot be replayed, the runtime reports
*which* frame, *how many* buffers it had recorded, and *which* packet made it bail —
never a silent fall back to presenting at the guest's rate.

## What the runtime counts

Every run reports, with denominators: frames recorded of frames seen; frames replayed of
frames recorded; replay bailouts broken down by reason; transform slots substituted per
replayed frame. A run in which no frame was replayed fails its gate; it does not pass
quietly.

## What this mechanism will not do

It does not inspect rendered pixels, estimate motion from image content, sample adjacent
frames to decide what geometry exists, or blend state whose provenance across ticks is
not established. An object whose state cannot be matched to the same object in the
previous tick is replayed un-blended and counted, never blended against a different
object's state.

## Renderer scope

The assembly site above is the Vulkan renderer's. The runtime's interpolation is
therefore Vulkan-only, which is the backend the first consumer targets. The OpenGL
renderer keeps its ordinary behaviour and presents at the guest's rate; that is an
explicit limitation, not an oversight, and it is recorded as such in project state.

## The other mechanism: the title's own paint path

### The measured result, and what it says about the title

One window, six seconds, alternating with unmodded windows either side, on the real
title headless:

| | paints | logic ticks |
|---|---|---|
| mod off | 180 in 6.0 s = 30.00/s | 180 in 6.0 s = 30.00/s |
| mod on, one vblank a flip | 277 in 6.0 s = **46.17/s** | 277 in 6.0 s = **46.17/s** |
| mod off again | 181 in 6.0 s = 30.17/s | 181 in 6.0 s = 30.17/s |

Two numbers, and they are the same number. **The title runs its logic once per
paint**: the caller census on `fapGm_Execute` counted 277 calls for 277 paints, so
the tick is not merely correlated with the flip, it is one-for-one with it. The
logic has a counter and a period of its own and does not wait -- but the *call*
that starts it comes from the frame path, and so it inherits the frame's pacing.

That is why the picture reached 46.17 and not 60. At one vblank a flip the loop
will go as fast as a vblank allows, but each iteration now carries a whole tick's
work, and the frame takes longer than the vblank it is waiting for. The pacing
change did what it said; what is left is the work, and the work is there because
the logic is riding the flip.

So the fix belongs in the logic path, which is where the goal said to look: gate
the tick on the title's own period. That is a change to the title's logic, in guest
memory, like everything else here -- and it is the first change in this mechanism
that is not about the picture.

**What the run says about it, which is the finding.** The gate was built to test one
named risk: that the logic doubles because it was slaved to the flip. It does not,
and the measurement says so rather than the risk being assumed away. In one driven
run with the stand-in and the gate, alternating windows in the same scene:

```
off   30.00 paints/s  (180 over 6.0s)   logic: 0 ticks, from the caller census
on    60.17 paints/s  (361 over 6.0s)   logic: 0 ticks, from the gate's own counter
off   30.00 paints/s  (180 over 6.0s)   logic: 0 ticks, from the caller census
on    60.00 paints/s  (360 over 6.0s)   logic: 0 ticks, from the gate's own counter
```

The picture reaches sixty and the controls hold at thirty, which is the whole of
what the mechanism claims. The logic reads **zero, not a slow count**, in every
window -- and the gate's counters are *not* the reason, because with the gate out
the caller census on `0x025d42ec` counts nothing either. The call the gate was
written to halve is not the call this title runs its per-frame work through on this
path. The gate's probe reports `installed`, and it reports the words sitting at the
tick's entry and at the one after it, so a zero can be told apart from a gate that
is not connected; that distinction is the reason those three fields exist.

**And the census was blind, which made the zero a finding about the instrument
rather than about the title.** The gate registered its own probe on the tick's
entry to learn when the title was linked, and a probe that holds an entry takes
it: every other registration for that address is refused for the rest of the run
with `EntryHeldOther`, and there is no way to take it back. So the caller census
on `0x025d42ec` could never install, in any run, gate on or gate off, and read zero
-- which is exactly what "the title never ticks" looks like. The control settles it:
`0x025f172c`, the per-frame entry the decompilation names, was probed alongside it
and counted **2020 calls for 2020 paints**.

**The tick is called once per paint.** So the objective's named risk is *true*, and
the earlier reading of this section was wrong: the logic is slaved to the flip, one
tick for one paint, and the gate is exactly the right instrument for it. With the
census able to see the tick at last, the measured rates are:

```
off   30.17 paints/s, 30.17 logic/s over 6.0s   (181 paints, 181 ticks)
on    60.00 paints/s,  0.00 logic/s over 6.0s   (360 paints, 0 ticks)
off   30.00 paints/s, 30.00 logic/s over 6.0s   (180 paints, 180 ticks)
on    60.00 paints/s,  0.00 logic/s over 6.0s   (360 paints, 0 ticks)
```

The on windows read zero from the gate's own counters, and that is now a statement
about the gate rather than about the simulation. The gate is **wired and never
entered**, and each of those is measured rather than inferred:

- its probe reports `installed`, and it now asks not to keep the entry, so the
  census on that address installs and counts;
- the word it writes at `0x025d42f0` is exactly the branch its own encoder
  computes for its block: `0x4a8315a0` for a block at `0x00e05890`;
- all eighteen words of its payload read back from guest memory and match the
  payload the code builds, the two lifted instructions included;
- the two counters at `0x00e058d0` and `0x00e058d4` are still zero after six
  seconds at sixty paints a second.

So the gate's code is in place, its branch is in place and correct, and nothing
executes it. Three things were wrong with it in turn, and each is fixed:

1. **Its link-time probe held the tick's entry**, which took it from the caller
   census for the rest of every run. Fixed in the fork: a probe may now ask for
   only the moment and hand the entry straight back. That fix is what made the
   measurement above possible at all.
2. **Registering that second probe from inside the install callback** appended to
   the vector the install loop was iterating, and the append invalidated the loop.
   The result was worse than no probe: a registration that reported `installed`
   and counted nothing, which reads as a title that never calls the function. Fixed
   in the fork: the registrations are a deque, indexed, re-reading the size each
   turn, so one made from a callback is installed in the same pass.
3. **Its two counters were in the code arena** -- memory the guest executes from.
   A guest store into an area documented for instructions is not something to rely
   on, and a store that lands nowhere is indistinguishable from code that never
   ran. Fixed: the fork can now hand out memory the guest may *write*, from its own
   system area, zeroed, and deliberately not registered with the recompiler. The
   gate's instructions stay in code and its counters go to data.

**What is left, and the control that found it.** The gate still reported zero calls
and zero ticks, so the next thing was a control that could not be argued with: the
gate's own install, with a payload of **one word** that does nothing but branch
back to the instruction after the branch site. No state, no counters, nothing to
get right, and the observer is the caller census, which counts the tick whether or
not that word runs. Paint rate unchanged at 30.00/s. Tick count **180 in the
control window, 0 with the control installed.**

So the word did not run. And the finding that falls out of it is bigger than this
title:

- **A branch written into the title's own code, at an *interior* address of a
  function, does not reach the loader's arena.** Not with `b`, and not with
  `mtctr`/`bctr`, and not after forcing the recompiler to translate the range.
- **A branch at a function's *entry* does.** The caller census's stub is in that
  same arena, reached by exactly that: a word written at `0x025d42ec`, the tick's
  entry. It has counted every call in every run -- 2020 for 2020 paints.
- **So is an indirect call through a vtable**, which is how the paint mod's
  stand-in is reached, and which is the only way anything has reached that arena.

Three ways in, one that works. The distinction is not the kind of branch: it is
that the two that work are the two the *probe and call* machinery resolve, and the
one that does not is a branch the recompiler has to turn into a jump to a host
address for an address it never translated.

**What that means for this mechanism, stated as the shape of the fix rather than as
a guess.** The gate cannot be reached by patching the title's code, so it has to be
reached the way the census is: as the resume of a probe. A probe on the tick's
entry already branches into the arena and comes back; what it lacks is any way to
send execution somewhere *else* first. So the capability the fork is missing is a
probe whose resume address is the probe's to choose -- `GuestCallProbes` writes the
stub's final branch as `entry + 4` and nothing can move it. A gate is then one word
at an address the probe machinery already reaches, and the same capability serves
any title whose logic needs pacing.

That is the next piece of work, and it is in the fork rather than in this title.

**What the title's own code says about it.** `FUN_025f172c` is the per-frame
entry, and it calls the tick unconditionally:

```c
uVar1 = DAT_1048d0ac;                        /* a period  */
DAT_1048d0a8 = DAT_1048d0a8 + 1;              /* a counter */
if (uVar1 != 0 && DAT_1048d0a8 == (DAT_1048d0a8 / uVar1) * uVar1) {
    FUN_025f1654();                           /* every Nth frame */
}
FUN_025f2d74(); FUN_025e15e0(); FUN_025d42ec();   /* the tick, always */
```

So the title already counts its frames, already has a period, and already uses
both to run one of its own functions every Nth frame. The gate is that idiom
applied to the one call that should have obeyed it: count the calls, let every
other one through. Nothing is invented -- the count, the test and the skip are the
title's own shape, and the finding is that the tick was the call that ignored it.

**And its shape in memory is forced by the tick itself.** The gate is sixteen words
and the tick is fifty-two, so the gate cannot live inside the tick; it lives in its
own block and the tick's *second* instruction takes a branch to it. Both of the
gate's exits go to the title's own code: the tick by a *tail* branch, so its return
reaches the title's caller, and a skipped call by a plain return. Neither returns
into the stand-in's memory, because that is the one direction measured not to work.

Which word it takes is not a choice, and getting it wrong is a crash dressed as a
gate. The tick, whole, from the image:

```
025d42ec  mfspr r0,LR        7c0802a6
025d42f0  stw  r0,0x4(r1)    90010004
025d42f4  stwu r1,-0x8(r1)   9421fff8
025d42f8  lis  r4,0x25d      3c80025d
025d42fc  li   r3,0          38600000
025d4300  addi r4,r4,0x42c4  388442c4
025d4304  bl   0x025df948    4800b645
025d4308  li   r3,0          38600000
025d430c  bl   0x0200e6ec    4ba3a3e1
025d4310  lwz  r0,0xc(r1)    8001000c
025d4314  mtspr LR,r0        7c0803a6
025d4318  addi r1,r1,0x8     38210008
025d431c  blr                4e800020
```

The first instruction saves the link register into the **caller's** frame, at
`0x4(r1)`, and the epilogue reads it back from `0xc(r1)` after its own `stwu` has
moved the stack pointer down eight. So the tick's return address lives in the
caller's frame and the tick is not re-entrant by construction: branch into it past
that store and its `blr` returns to whatever the link register happened to hold,
which after a gate has been counting is the gate's own return. The gate therefore
supplies the first two instructions itself, lifted whole from `0x025d42ec` and
`0x025d42f0`, and branches to `0x025d42f4`. A call it lets through is then the
title's tick entered exactly as the title enters it -- which is the only claim
worth making about a patch that skips half the calls.

The gate keeps two counters in guest memory -- calls, and ticks it let through --
so the host reads the logic's rate from the gate itself rather than from a probe on
a function the gate has replaced. The run says which of the two it used, because a
rate taken from a counter the gate owns and a rate taken from a probe are not the
same measurement.

The picture is otherwise already right. Two consecutive paints, captured one after
the other in that same window, compared byte for byte over 6,220,816 bytes: **identical**.
A full 1080p frame, every byte. That is the null case the blend has to beat, and it
is measured rather than assumed -- with the caveat the run's own state carries, that
the scene the presses left was not moving, so "identical" is what a still scene
gives and does not yet distinguish two paints of one tick from two paints of a
stationary world. Three captures would.

Everything above is the runtime re-issuing a frame it recorded. This is the
other way to present at the display's rate: change the title's own paint, in
the title's own memory, and let it draw the frame twice itself.

They are not variations on one idea. The one above has to work out *which
submitted value is which object's transform*, because nothing in the recorded
stream says so: it matches draws across ticks by the uniform blocks they source
and their occurrence among identical draws, finds the view by watching which
shaders share a matrix that moves like a camera, and guesses which vertex
buffers belong together for anything the CPU rewrote. A paint the title performs
itself needs none of that. The node *is* the identity, the node's own draw
regenerates its attributes, skinning and display list at whatever pose it is
handed, and the title already names its view matrix (`cWorldViewMatrix`) in its
own rodata. What the title cannot do is blend, because it has no notion of a
halfway pose — which is the one thing this mechanism still has to supply, and
where it has to be careful.

The fork's `GuestPatching` is the seam (`external/cemu/src/Cafe/HW/Espresso/`):
guest memory the guest may execute from, and words across the boundary in the
guest's big-endian order. The mod is `src/wiiuport/title/WindWakerPaint.h`, and
what it does to Wind Waker HD is in the title project's `docs/render-state.md`,
which owns the addresses.

### What the display thread's shape makes possible

The display thread's entry is eleven instructions that call one vtable slot in
a loop, and the only thing between two iterations is a wait for the flip. So:

- **The picture's rate is this thread's swap interval and its paint count.**
  Nothing in the frame function asks the logic thread for anything.
- **A second paint is not fighting a consumed stream.** The render tree walk
  reads flags and pointers and writes nothing, so running it again in the same
  tick is running the same function again.
- **The frame is reached through an indirect branch**, so a stand-in can go
  anywhere executable. What a stand-in does *inside* that memory is a separate
  question, and the answer here is direct branches only: the payload calls the
  frame the install check verified, rather than re-deriving it from the vtable
  on every pass. The cost is real and is stated below; the price of the
  alternative was a title that stopped painting.

### Two things that are reserved and discovered before anything is written

**The vtable is discovered, not assumed.** The address is in the image, but the
object that matters is the one the title built, and its vtable is a word at
`display+0x24` that a probe on the frame hands over in `r3` on every paint.
Patching the image's address would rewrite a word of whatever object *does* own
it, and slot `0xcc` of an unrelated class is not a frame. So the live vtable is
read, its slot `0xcc` is required to hold this title's frame, and that word is
what gets written. Before the display thread has painted there is no live
vtable, and the mod refuses rather than falling back to the address.

**The memory is reserved when the title is linked, not when the mod loads.**
Allocating executable guest memory out of the loader's arena is a request the
loader expects while it is linking. So the block is taken from the frame probe's
own install report, which the fork raises once the title's modules are linked.
Enabling the stand-in is then a single word written to a data word the display
thread already re-reads every iteration; nothing else about the guest changes at
the moment of enabling, which is what makes it removable.

Reserved at *load* time instead -- which is where it went first -- the product
died before printing a line, because the loader's arena had no memory to hand out
before the emulator's memory was up. Measured: a run whose log held sixty lines
of gamescope and none of the runtime's. The distinction is worth keeping for
anything else that reaches into the guest: there is a moment when the guest's
address space exists, and it is not process start.

### Three ways to get it wrong, all of them measured

The first run of the falsifier killed the title outright, and each of these was
a separate cause, found by separating the redirect from the second paint from
the interval rather than by reading the code:

1. **A call in the stand-in destroys the way back.** The game's only call to
   the frame is what set the link register, and a stand-in that calls the
   title's own `GX2SetSwapInterval` overwrites it — so the `blr` at the end
   returns into the middle of the stand-in and repaints for ever. It branches
   back to the top of the loop instead, and the loop's address is read and
   checked before anything is written.
2. **A word is a value, not a copy.** Writing the stand-in as a block of host
   words byte-copied into the guest put every instruction there with its halves
   exchanged, and the display thread branched into noise. The order belongs to
   the seam and nowhere else; the stand-in is written a word at a time through
   it.
3. **A branch's displacement is measured from the branch.** An off-by-one-word
   in the last one sends the thread somewhere else entirely. Both computed
   words are checked against reach before anything is written, and the branch
   encoding is unit-tested against a real instruction from the title's image.

4. **An indirect call out of the stand-in stops the title; a direct one does
   not.** The stand-in reached through the display vtable -- the title's own loop
   body, word for word, re-reading the frame and going through the count
   register -- froze the whole emulated system for exactly as long as it was
   installed. No paints, no logic ticks, no error logged anywhere, and 30 a
   second the moment it came out. The same block with the same words and the
   same one rewritten word of vtable, branching straight at the frame instead,
   runs the title at 30.00 paints and 30.00 logic ticks a second with the
   stand-in installed. So the memory is sound, the redirect is sound, and the
   difference is one instruction pair: `mtctr`/`bctr` against `b`.

   **What did not explain it, and is recorded because it was believed for a
   while.** An indirect branch jumps through the recompiler's jump table, indexed
   by the target address, and a block allocated out of the loader's trampoline
   arena had never been registered in it -- so `AllocateCode` registers a block
   as it allocates it, and that is right on its own terms. It changed nothing:
   the freeze was identical with the block registered, over four runs. The
   recompiler's own log (bit 60) was turned on for a fifth and says nothing
   about the block, so the block compiled. What the indirect form does that the
   direct one does not is still not established, and the honest position is
   that the payload avoids it rather than that it is fixed.

   The payload is therefore direct branches only: `bl` at the frame the install
   check verified, and an absolute branch back to the top of the loop, because a
   `bl` sets the link register and the `blr` the game's own call would have
   provided is no longer available. The price is that the frame is not re-read
   from the vtable each pass, so a title that swapped that slot at runtime would
   keep painting the frame the mod verified. The install check is what stands in
   for it: it refuses by name over any slot that does not hold this title's
   frame. The variant that re-reads it is kept as mode 4, because it is the
   falsifier for this decision -- same block, same words, one instruction pair
   apart, and it does not run.
5. **A return into the stand-in's memory does not work.** Painting twice with
   `bl` at the frame twice and a branch back took the *emulator* down with
   signal 11 at a host address, on the first frame. The frame returns through
   `blr`, and the payload's own `bl` had put the link register inside the
   stand-in -- so the frame returned into the loader's trampoline arena, which is
   the same class of failure as the freeze above: a register-indirect branch
   whose target is that memory. Everything the payload does therefore happens on
   the way *out*: it puts the link register back where the loop's own call left
   it, and reaches the frame with a plain branch, so the frame's return goes to
   the title's loop and the stand-in is never returned into.

   **What that costs, and what it buys.** It buys the rate, because the flip is
   what paces the loop: one vblank a flip is what doubles the picture, and the
   interval call is the game's own `GX2SetSwapInterval(1)`. It costs the second
   paint, which cannot be a second call in the same iteration. That paint is
   still wanted -- it is what will carry the blend -- but it has to come from a
   *different* place: an iteration that paints the blend and an iteration that
   paints the tick's own frame, chosen by a counter in the stand-in's own
   memory, with each iteration still leaving by a branch into the title's code.

6. **The title's own `GX2SetSwapInterval` cannot be called from here.** The
   one-vblank payload did exactly what the title does -- `li r3,1` and
   `bl 0x028fad2c`, the same call site word for word, `lwz r3,0x50(r26)` and
   `bl` -- and the emulator died with signal 11. The dump named `IP 0x02c1295c`,
   an address in no function, with `LR 0x00e05858`, which is the stand-in's third
   word: exactly where the `bl` had put it. So the call reached the import and did
   not come back.

   And that export does one thing with its argument: it stores it in Latte's
   shared area. So the pacing is the emulator's, not the title's, and the fork
   says how many vblanks a flip takes -- with the export's own bound, and the
   previous value returned so a caller can put it back. The mod writes the
   title's own field at `display+0x50` beside it, so the title's record and the
   thing it records agree, and restores both.

   The payload is then a **single branch**, and a single branch is all it can be:
   with nothing called, the display register the loop's call set up arrives at
   the frame untouched, and the frame's return goes to the title's loop.

7. **The loop's top is not the thread's entry.** The display thread's entry at
   `0x0274c00c` is a prologue -- `mfspr r0,LR`, a new stack frame, `or r31,r3,r3`
   -- and the loop proper starts four instructions later at `0x0274c020`. A
   stand-in that branches back to the entry therefore re-frames the stack on
   every paint and reads the display pointer out of whatever the frame left in
   `r3`. Measured: it ran for seven seconds and then faulted at `0x0274c020` with
   `r31` zero, which is `lwz r12,0x24(r31)` on no display at all. The emulator's
   own crash dump named both the address and the register; nothing in the
   stand-in's source did.

The second and third were found by a unit test and the rest by runs, which is the
order they should have been found in: none of the six is visible by reading the
payload, and every one of them is silent until the guest executes it. The
separation is what found them -- one stand-in with a mode each for the redirect,
the second paint, the interval, the indirect call, the field write and the
one-vblank form -- so that a title which would not take one said which, and so
that a freeze could be narrowed to a single instruction pair rather than to "the
stand-in".

### Three words that were lifted, and are no longer needed

A seven-word payload once put the loop's return address back into the link
register so a called frame could return to the title's code. It needed `lis`,
`ori` and `mtspr`, and the way to be sure of them is worth keeping even though
the payload that used them is gone -- the earlier one is what the note below
records. All three are lifted from the title's own image, and the field that
varies is checked against a second instruction instead of being derived:

| word | instruction | where |
|---|---|---|
| `0x3d801019` | `lis r12,0x1019` | the title, so `lis r12,X` is `0x3d800000 \| X` |
| `0x7c0803a6` | `mtspr LR,r0` | the title, so the SPR number is already in it |
| `0x7d0903a6` | `mtspr CTR,r8` | and `0x7d6903a6` is `mtspr CTR,r11`, so the register is at bits 21-25: they differ by exactly `3 << 21` |

That last pair is the check. A field position derived from one instruction is a
guess; derived from two that differ only in that field it is a fact about this
title's encoding, and `mtspr LR,r12` is then `0x7c0803a6 + (12 << 21)`. It is
`0x7d8003a6` and not `0x7d8803a6` that a missing SPR field produces, and a unit
test pinned the word that worked. None of the three is in the payload now, and
that is the finding: **the cheaper the payload, the fewer words have to be
right.** The words that remain are one branch, and its displacement is checked
against a real instruction from the title's image.
