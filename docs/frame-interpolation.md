# Frame interpolation: the mechanism

How `wiiuport` presents Wind Waker HD at sixty pictures a second. This document owns the
mechanism. What any value *means* belongs to the consuming title; the addresses, the payload
and the blend policy are the title project's, and this repository keeps only the capability
to patch guest code in memory.

**This document was rewritten.** It used to be a 3,955-line investigation log for a
host-side statistical lerp that has since been deleted. What is kept here is what is still
true and still needed: the guest's structure, the paint path that ships, every measurement of
it, the six defects its instruments turned up, and the state of the blend. The narrative of
the deleted mechanism is gone with the mechanism -- its own evidence was measured, and
deleting the rows that claimed it (ST-RECORD, ST-REPLAY, ST-REPLAY-GL, ST-CAMERA, ST-NULLDIFF,
ST-PRESENT, ST-60, ST-SHADOW) deleted its measurements with them rather than quietly reusing
them here.

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

## Six defects, and what each one cost

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

### 4. A second lock on one mutex, which hung the test binary

`ObjectPoseLocator` took a second, non-recursive `m_mutex` on a path that already held it. The test
binary hung rather than failing, which is the worst way a lock can be wrong: a deadlock reports nothing
and attributes itself to whichever test ran last. One lock, one owner, one `std::scoped_lock`.

### 5. A second copy of the affine predicate

A 4x4 test with the basis in the rows and in the columns counted separately, written on the reasoning
that a 64-byte block is exactly one 4x4. It passed its own tests, it duplicated a rule that already had
an owner, and it was a *different* rule from the one the census uses -- so two instruments would have
disagreed for a reason neither could see, both calling it "the affine class". It is deleted in favour of
`title::TransformShape`'s. **And the wrong move after that was to write a third rule**; what stopped it
was asking who owns the predicate, which is the question a second copy should have raised the first time.

### 6. A payload branching into a zero-filled hole, and a probe on a word that reads the link register

Two faults in this project's own code that a gdb window reported as a fault in the guest's: a payload
branch into a zero-filled hole at `0x028fad2c` (the `GX2SetSwapInterval` call site), and a probe sitting
on the frame's first word `mfspr r0, LR`, which the frame returns through. With both fixed the register
that was wrong is right -- `lr = 0x0274c280` is the frame's own seventh word -- and **the guest still
reached the arena.** The lesson that took longest: the fault was the host's, and it moved three times
while being attributed to the title. The frame writes one word above its own allocation, and the
instrument that reported it was reading the host's memory rather than the guest's.

## What the title says once the instruments speak

With the probes installed, the binder and the gate both report:

```
binder probes at 0x027ff88c and 0x027ff9c0, installed as installed and installed:
  167959 bindings recorded, 589 objects          (first window)
  195581 bindings recorded, 590 objects          (second window)
identity: source objectAddress, 14 distinct identities, 128 tracked (identity, offset) pairs
```

**The identity is the node.** That is the objective's own answer, it was `blockSources` -- a withdrawn
source -- until this run, and the difference is one ordering in a stub. A draw's block address names an
object's ring slot and the title reuses them; the node does not.

**The pose, keyed to that identity, moves.** The census's loose class holds 10 offsets often enough and
10 of those are seen to move, best offset 60, over 14 node identities -- against 11 over 7 `blockSources`
identities before, so the denominator moved with the identity rather than the answer staying put by
accident. The loose class is the one that chooses between two answers: a test asking for three rows of
**unit** length finds a pose only when nothing is scaled, so it reports "nothing here" identically for a
field that is absent and for a field that is present and carries scale. This title's world matrix is
`T * R * S`, so the strict class is the wrong question and its 4 offsets ever in class, 0 held often
enough, is not evidence of absence.

**And tick N-1's uniform block is still present when tick N paints:**

```
16 of 16 comparisons say still present, 0 say overwritten, 0 could not be read;
8 consecutive pairs used different addresses, which is double buffering measured
```

**This reverses a withdrawal.** "Whether the other slot holds tick N-1's values is withdrawn" was
measured with the binder silent, so the two slots it compared were not the two the title was using.

**The node's own window holds transform-shaped data that does not move, and that is the answer with
denominators.** Over a 2,588-byte window at the node, the loose class holds a 3x4 at offset 100 in 7 of
8 objects, at 276 in 7 of 8, at 288 and 376 in 5 of 8 -- and **0 of them moved, with a biggest delta of
exactly 0** across 21, 21, 15 and 15 repeat comparisons. A static prop's own transform not changing while
the camera moves is what a prop's transform does. **So the chain is: the node is the identity, the draw
assembles the uniforms, and the pose is in that assembly -- which is the chain the objective named, with
each link measured.**

## The blend: where it is written, and what is still missing

### The pose is per-object, not a view matrix, and that is measured

The share of assemblies an offset appears in cannot answer this. A camera's view matrix is written once a
frame and read by every shader; a static prop's world matrix belongs to that prop. A quarter of the
frame's draws being objects fits a per-object pose at 25% exactly as well as it fits a global, and the
locator's best offset has been 60 at 24-25% in every run. **The share is not the discriminator, and
reading it as one is a guess with a denominator attached.**

The discriminator is the value. A global is *the same twelve floats* in every object; a per-object pose
is a different twelve floats in every object. `ObjectPoseLocator` keeps, per candidate offset, the value
most recently seen and compares each **new identity** against it exactly -- the twelve words, not a hash,
because this is the one answer that decides what a blend writes. Compared only across identities,
because within one object the value is expected to move between ticks and comparing an object against
itself would count every per-object pose as differing from itself.

**The value compared against must be the *last* one seen, not the first ever:** a view matrix changes
every frame, so retaining the first would make every object from the second frame onwards "differ" from
it, and the answer would be the opposite of the truth from an instrument that looked right. **The tests
that caught it are worth more than the rule**, because the first two fixtures could not have caught it at
all: a value that is always equal or never equal gives the same count whether the retained value advances
or not. The distinguishing shape is **A, B, B** -- one agreement and two differences against the value
just seen, and three differences against the first -- and with the advance removed that test fails.

On the real title, over 1,035,056 assemblies with the camera moving:

```
offset  60: 200730 assemblies, 67860 repeat comparisons, 17620 moved
           compared against 15 other objects, 6 the same value and 9 different
offset  52: 185375 assemblies, 25200 moved        6 same,  9 different
offset  56: 182874 assemblies, 17233 moved        6 same,  9 different
offset  68: 181709 assemblies, 18054 moved        5 same, 10 different
offset  36: 180632 assemblies,  1796 moved        6 same,  9 different
offset   4: 174340 assemblies,  5790 moved        6 same,  9 different
offset 104: 169483 assemblies,  2543 moved        8 same,  7 different
offset  72: 167965 assemblies,  5151 moved        5 same, 10 different
```

**No offset is shared. The most shared is 8 of 15, and the best offset reads 6 of 15.** A camera's view
matrix would read 15 of 15 the same, because every vertex draw in the scene pass would be holding the same
matrix. **The per-object chain cannot carry a camera move.**

> **This section's conclusion is withdrawn, and the reason is in the instrument rather than in the
> title.** A later run, 1,685,585 assemblies with 12,090,138 identity comparisons, reports every one
> of the eight offsets it holds as **`12 of 15` other objects reading the same value, with
> `identities: 1` and `identitySamplesCapped: yes` at every offset.** The two runs disagree -- 6 of
> 15 then, 12 of 15 now -- and the second has a far larger sample, so the second is the better
> measurement and the first was a sample of a convenience.
>
> **The fault is that the census counts offsets pooled over every identity.** `ObjectPoseLocator`
> keeps one value per *offset* and compares each new identity against it, so at each offset there is
> exactly one tracked identity (`identities: 1`) and the "other objects" are fifteen others measured
> against it. A value that belongs to **one object** is present in that object's draws and diluted
> across every other object's, so it never reaches the 20% "held often" bar -- and a value that is
> present in a quarter of the frame's draws, because it is a **pass's** view projection, reaches it
> every time. **The bar selects pass values, not poses, and no corpus would have found a per-object
> pose with an instrument that pools identities.**
>
> The fix is to count offsets *per identity*: hold an identity's offset on the identity, not on the
> offset. The discriminator is already the right question -- does this value belong to this node --
> and it is being asked of the wrong subject.
>
> **One of the three defects in it is fixed and the other two are not.** The report's `identities`
> field -- "distinct block-source sets seen holding one here" -- was `= 1` when a candidate was
> created and never moved again, so **every run reported one identity at every offset** and a reader
> sorting offsets by it was sorting by nothing. It is now **derived** from the per-(identity, offset)
> history that actually holds the information, and it is a different number from `otherIdentities`
> (the bounded sample the comparison used) and from `otherIdentitiesSameValue` (what it concluded).
> The test reads two objects into one offset and requires `identities: 2`, and is shown its other
> answer: put the field back to a constant and it fails.
>
> The other two are not fixed and are not pretended to be. The "held often" bar is still pooled over
> identities, so a per-object value is still diluted below it; and the cross-object comparison is
> still against the **previous assembly's** value rather than the same object's, which measures
> whether *consecutive draws* share a value -- a pass-value test, not a per-object one. Both are the
> same redesign, and the redesign is: keep the candidates **per identity**, hold an identity's
> offset on the identity, and answer "does this value track this node" over that node's own
> consecutive assemblies.

### The eight offsets are one array, and that is why the best offset moved between runs

The report now carries the **values**, not only the counts, because a count says how often and the
title names its own camera, so a camera is a comparison and not an inference. One run, gameplay
reached, camera moving:

```
offset 104:  0.19981, 319777, -0.638178, 0.010258,  0.76982,  -364194,  ...
offset  96: -0.253329, 0.946519, 0.19981, 319777,  -0.638178, 0.010258, ...
offset  92: 3537.66, -0.253329, 0.946519, 0.19981,  319777,  -0.638178, ...
offset  76: -194559, -0.955708, -0.212888, -0.203222, 3537.66, -0.253329, ...
```

**Laying the eight windows over each other, by (offset - lowest) / 4, they are one flat array of
seventeen words with no contradiction at any shared position.** That is re-runnable arithmetic: the
windows' overlaps agree, and the array is

```
-194559  -0.955708  -0.212888  -0.203222  3537.66  -0.253329  0.946519
0.19981  319777  -0.638178  0.010258  0.76982  -364194  -0.586921  0.64063
-0.495092  20791.9
```

A matrix written as a flat run of floats is affine at every 4-byte alignment inside it, so **one
matrix was being counted as eight candidates**, each with its own counts. That is why `bestOffset`
moved between runs -- 60 in one, 104 in the next -- for a reason that had nothing to do with the
title: it was the maximum over a run of offsets that are the same matrix.

**The fix is the collapse this repository already had.** `GlobalPoseCensus` had it; `ObjectPoseLocator`
did not, and two scans keeping their own copy of a rule that decides how many poses a report claims
are two numbers a reader cannot compare. The rule now has one owner, `TransformShape::sameShapeAs`,
and both ask it. The candidate is a matrix; the **alignments it was seen at are recorded beside it,
because they are the fact the blend turns on**:

- `assemblies` counts an assembly once for the matrix, however many of its alignments were affine;
- `alignmentsSeen` and `foldedFromAlignments` say how many places it was found and how much the
  collapse removed, so a reader can see what was folded rather than trust that something was;
- the test writes two matrices and requires two candidates, and is shown its other answer: **with
  the fold off, 8 candidates for 2 matrices.**

**And the consequence for the blend, which is the opposite of what the block route assumed.** A
pose's offset in an assembly is **not a global constant** -- but it is not arbitrary either, and
saying so precisely is what makes the blend writable:

> **A uniform block's layout is fixed.** An assembly is one shader's uniform buffer, and two draws
> of *one* shader put the same uniform at the same slot every frame. Two *different* shaders may put
> the same uniform at different slots. **So the pose's offset in an assembly is a function of the
> shader, not of the draw** -- and the eight alignments are eight shaders' layouts, not eight draws.

That is a testable prediction, and the census can now test it, because the table is keyed on
`(shader, offset)` rather than on the offset alone. It was keyed on the offset, which **pooled every
shader's layout together** -- the same class of fault as pooling offsets over identities, and for
the same reason: the instrument was keyed on the wrong subject. Two shaders sharing an offset found
one candidate, and the second's assemblies were counted against the first's layout.

The report now carries **`distinctShaders` and `distinctMovingOffsets` as a pair**, because the pair
is the measurement: one offset each is a single shader, equal counts is a value every shader shares
at its own offset, and more offsets than shaders is neither. It replaces a single `bestOffset` that
was a maximum over offsets belonging to different shaders, so it named whichever shader happened to
place its matrix lowest -- and moved between runs for that reason alone. The test writes one matrix
into two shaders and one shader twice, and is shown its other answer: **with the key back to the
offset alone, 1 candidate where there should be 2.**

**So the blend's write is per shader, and the shader is a name the draw already carries** in
`UniformAssembly::shaderBaseHash`/`shaderAuxHash`. No search, no native override of the title's
code, and no fixed offset: for a draw, the pose is at the offset *that draw's own shader* puts it
at, which is a lookup rather than a measurement.

### Measured: the camera is twelve words in a flat array, at a per-shader offset

With the table keyed on `(shader, offset)` and the bar measured against each shader's own
assemblies, the same run on the real title reports **240 of 298 candidates clearing the bar** --
against **1** before, on a whole-frame denominator no per-shader count can reach. 162 distinct
shaders, 40 distinct moving offsets. The eight the report names, with the values:

```
shader 0x1557c18f92f3bcb9  offset  12   moved 21730/21879   0.00103093, 0.694118, 1, 1, ...
shader 0x1557c18f92f3bcb9  offset  60   moved 21708/21857   0, -0.638178, 0.010258, 0.76982, -364194, -0.586921
shader 0x8cecd19741c6c1c7  offset   4   moved   202/21950   0.005, 2455, 0.00103093, 0.694118, 1, 1
shader 0x8cecd19741c6c1c7  offset  84   moved    80/21864   0.64063, -0.495092, 20791.9, -0.498249, -0.767781
shader 0xb7252004aba21c10  offset  76   moved    98/235     0, 0.443678, 0.768473, 0.461084, 0, -2.81476
shader 0x44f85a8fe341045c  offsets 0, 228, 284               held, never moved
```

**The same matrix is in two shaders, one word apart.** Laying the windows over each other by
(offset - lowest) / 4, offsets 4 and 12 agree at four of six printed words with the second starting
two words after the first -- one array, two shaders' layouts. And offset 60's words
(`-0.638178, 0.010258, 0.76982, -364194, -0.586921`) are indices 5..10 of the seventeen-word array
this document already recorded, with offset 84 continuing it at 13..18.

**So the answer to "which twelve words of the assembly is the pose", for the camera:** the camera is
twelve consecutive words in a flat run of floats in the title's assembled uniforms, and where that run
lands is a property of the shader that read it. It is at **offset 12 in shader `0x1557c18f92f3bcb9`**
(21,730 movements of 21,879 comparisons) and at **offset 4 in shader `0x8cecd19741c6c1c7`** (202 of
21,950). The other three offsets named are the same array at other alignments, or matrices that
never moved.

**And the same run separates the per-object pose from the shared ones, which is what condition 2
asks for.** The report's `otherIdentitiesSameValue` is the discriminator, and with the per-shader
key the numbers separate cleanly:

```
shader 0x8cecd19741c6c1c7  offset  4   0 of 15 other objects read the same value   202/21950 moved
shader 0x1557c18f92f3bcb9  offset 12   0 of 15                                     21730/21879 moved
shader 0x1557c18f92f3bcb9  offset 60   3 of 15                                     21708/21857 moved
shader 0x8cecd19741c6c1c7  offset 84   8 of 15                                        80/21864 moved
shader 0xb7252004aba21c10  offset 76  12 of 15                                         98/235 moved
```

**A camera's view matrix reads 15 of 15.** These read **0 of 15** -- every object at that offset holds
its own twelve words, and two different shaders hold *the same* twelve words at offsets 4 and 12.
**That is the per-object pose, measured, with the addresses**: twelve words at **offset 12 in shader
`0x1557c18f92f3bcb9`** (21,730 movements of 21,879 comparisons, in every one of that shader's
assemblies) and at **offset 4 in shader `0x8cecd19741c6c1c7`** (202 of 21,950). The offset is 76 for
a value 12 of 15 objects share, which is a pass's, not an object's.

**What the discriminator is, stated precisely, because its limit is part of the answer.** It compares
an offset's value **against the value at the same offset in the immediately preceding assembly**, not
against the same object's earlier value. So "0 of 15" is exactly *consecutive objects read different
twelve words at this offset*, which is the per-object signature; and it is not a per-object *temporal*
test, which would ask whether one object's own value changes with that object. The two agree for a
value that belongs to one object -- a prop standing still reads the same value every time and its
neighbour reads a different one -- and disagree for a value the title recomputes per object per tick
in a way that happens to repeat across neighbours. **That case is not excluded by this measurement and
is named here rather than glossed.**

### The blend, built: a per-shader lookup, and a write at the title's own draw

Two units, and the window between them is the whole mechanism.

**`PoseByShader` — the lookup.** One byte offset per shader, keyed on the base **and** aux hashes,
fed from the census's own candidates by `POST /pose` and read by `GET /pose`. Three refusals, each
with its counts beside it, because a blend that writes twelve words at a wrong offset corrupts a
value that is not its own:

| refusal | the measurement that triggers it |
|---|---|
| a pass's value, shared by 12 of 15 objects | moving it would move the light with the object |
| a value that never moved | it is a basis matrix, not a pose |
| a shader the table has never seen | the draw is left exactly as the title wrote it |

**`PoseBlend` — the write.** On an in-between paint, for a draw whose shader the table knows, hold
the title's own twelve words from the previous tick and write `held + 0.5 * (current - held)` into the
assembled buffer, which the title is about to transform with. No native override, no patch to the
title's code, nothing on the player's disc: the assembled buffer is the runtime's own memory.

**Three faults, each found by a test and each shown its other answer:**

- **Two overloads, and the compiler picked the wrong one.** `onAssembly(…, uint32_t offset, …)` and
  `onAssembly(…, uint64_t shader, …)` differ in whether the third argument is an offset or a hash, and
  a call with a 32-bit literal matched the offset form exactly while the 64-bit hash did not. A call
  that meant a shader was read as an offset past the end of a buffer. **The two are one method and a
  differently-named one now** (`onAssembly` and `onAssemblyAtOffset`): a rule about an argument's
  width is not something to hand to overload resolution.
- **The held copy was refreshed only when nothing was written**, so the held pose froze at the value
  it was first given and every later lerp was measured from that: a midpoint between tick 0 and tick
  N rather than between N-1 and N. **That is a wrong place which looks right** — the picture still
  moves, at the wrong speed, and no counter says a fault. The test walks three in-between paints and
  requires 5, 15 and 25; with the refresh removed it reads 5, 10 and 15.
- **`held` counted assemblies, not pairs**, and the blend's hit rate was divided by it — so it was
  lerps per *assembly*, and a frame with one object and a thousand draws read as a thousand pairs.
  The pair count is the map's size and the counter is `refreshed`, under their own names, and the
  rate is `lerped / inBetweenKnown`: of the in-between draws that **could** be blended, how many
  were. A table full of held poses and no lerps is the exact shape of a blend that looks installed,
  and this is the field that says it is not.

**The window is the observer's own.** `AssemblyRecordedListener` is handed a *copy* — a
`RecordedUniformAssembly` whose `data` is a vector the observer owns — so a listener wanting to
change what the title is about to transform with would be changing a copy the draw never reads, and
would report having blended. **`AssemblyBeforeDrawListener` is a separate seam, called with the
guest's mutable buffer before the copy is taken**, and the blend is registered on it. The census stays
on the recorded listener, so **the measurement never sees a value the blend wrote**: a census reading
a lerp would be a census of this project's own arithmetic.

**The two halves are one document with two named sections**, composed from the two owners' own
`writeTo` rather than by splicing their rendered text — `body.substr(0, body.size() - 2)` is how a
report grows a second one, and this project has paid for that once.

### So the whole of condition 2 is answered with addresses, from the addresses alone

- the identity is the **node** -- `0x027ff88c` and `0x027ff9c0`, 195,581 bindings over 590 objects;
- the pose is **twelve words in the title's assembled uniforms**, not in the node's own 2,588-byte
  window (0 of 23,210 transform-shaped windows there move) and not in the uniform block the draw
  sources (23, 14 and 0 in the class across the three most-used blocks, 0 moving);
- **at an offset that is a property of the shader that read it** -- 12 in `0x1557c18f92f3bcb9`, 4 in
  `0x8cecd19741c6c1c7`, 76 in `0xb7252004aba21c10` for the pass's value;
- and **tick N-1's block is still present when tick N paints**, 16 of 16, so the lerp has both ends.

### Measured on the title: 1,240 lerps, 100% of the in-between draws — and a defect the falsifier found

`scratch/frame-loop/pose_blend_run.py`, gameplay reached, camera moving, the stand-in in the mode
measured at 59.99 paints a second:

```
POST /pose offered 299, took 64, refused 235
  the first refusal: compared 0 assemblies, so nothing said whether it moves

armed window, 8 s:   assemblies 1,526,356 -> 1,662,256   (+135,900)
                     held 0 -> 78,  firstSight 78,  refreshed +2,544
                     inBetweenKnown 1,240,  lerped 1,240  (100% of the in-between draws)
```

**So the blend writes 1,240 × 12 = 14,880 words in eight seconds, on the title's own in-between
paint, at every in-between draw that could be blended.** And the denominators are the ones that make
it a measurement rather than a total: 1,240 of 1,240, over 78 held pairs out of 135,900 assemblies.

**A later run of the same harness wrote nothing at all.** Same title, same presses, same mode, same
seed of play: `held` 0, `lerped` 0, `withoutShader` 29,440 of 29,440 — not one draw of any of the
56 shaders the table held, where the first run had blended 1,240 times. `POST /pose` took 64
candidates in the first run and 56 in the second, so the table was nearly the same size both times
and covered a disjoint set of the game's draws.

### Why the game's most-drawn shaders have no candidate at all — measured, by name

The run asked the census about the scene's four most-drawn shaders by name, and the answer is **not a
refusal**:

```
0x6669a23d03806414:  0 candidates of 300 considered, 41,136 draws
0x2802e519ac163806:  0 candidates of 300 considered, 23,700 draws
0x5ae6d5fe34beb432:  0 candidates of 300 considered, 13,055 draws
0x842a19b509f8b91a:  0 candidates of 300 considered,  6,381 draws
```

**The census does not find anything in the shaders the game draws with most.** Not a candidate that
was refused, not one below the bar — none at all, out of 300 candidates over 162 shaders. And the
reason is in the class's own entry condition: `onAssemblyRecorded` returns before scanning when
`assembly.data.size() < kPoseWords`, so **an assembled buffer shorter than 12 floats — 48 bytes —
cannot hold a 3x4 and is never examined.** The report's own `unscanned` count is the place that fact
belongs and it is not a candidate at all: a buffer too short to hold the shape is not a buffer the
scan looked at and found nothing in, and the two are different answers.

**And refeeding them offers nothing at all, which settles the question.** The per-shader feed exists
precisely for this case -- a candidate refused on first sight can be admitted by offering it again
once it has drawn enough -- and pointed at the sixteen shaders the blend could not place it offered
**0 candidates, took 0, and the table held 78 where it held 78.** So staleness is not what is wrong
with the scene's most-drawn shaders: there is nothing to re-offer, because the census has never
found anything in them. **That is the difference between a candidate that is late and one that is
absent, and only the per-shader query tells them apart.** A fifth run in the same session, with the
loop: `POST /pose` took 78 of 283, refused 111 as a pass's value and 94 as a value that never moved,
and **lerped 7,149 times of 7,149 in-between draws, 85,788 words in eight seconds**, with 42,478 of
56,972 assemblies unplaceable. The stand-in-off arm wrote nothing across 39,013 assemblies.

**So the picture is two censuses, not one.** The shaders whose uniform buffers are long enough carry
a per-object pose in those uniforms, and the blend writes it — **74 shaders accepted of 297 offered,
1,415 lerps of 1,415 in-between draws, 16,980 words in eight seconds, and 1,404 tick's-own paints
held unwritten beside them.** The shaders the game draws with most carry their transform somewhere
this mechanism has not looked, and the one place left is the **vertex attribute stream**: the census
this project already has, which reports `objectsOffered: 0`, `nodesTracked: 0` and
`drawsWithoutPosition` equal to all 1,388,163 draws. That is a blind instrument reporting zeros, not
an instrument that found nothing.

### The four are two different things, and half of them are not short at all

**Everything above rests on a sentence the report could not support**: that those shaders' assembled
buffers are *under 48 bytes* and so were never examined. `tooShortForAPose` is a **whole-run total**,
and `GET /pose?shader=` counted *candidates*, not that shader's assemblies — so "0 candidates of 300
considered" said the census held nothing that belongs to this shader and nothing about whether the scan
ever looked at one of them. The two readings call for opposite next steps, and only one of them was
ever a fact.

Asked per shader, on the real title, gameplay reached and walked (2026-10-04):

```
0x6669a23d03806414   60361 assemblies      0 too short   60361 without sources   272 bytes   can hold a pose
0x5ae6d5fe34beb432   12080 assemblies      0 too short   12080 without sources    64 bytes   can hold a pose
0x2802e519ac163806   40727 assemblies  40727 too short        0 without sources    32 bytes   cannot
0x842a19b509f8b91a    5625 assemblies   5625 too short        0 without sources    16 bytes   cannot
```

**Two are too short, and two are refused at a different gate entirely.** `0x6669a23d03806414` — the
most-drawn shader in the game — carries **272 bytes** of uniforms, which is five 3x4s, and *can* hold a
pose. Every one of its 60,361 assemblies was refused because `assembly.blockSources` was empty, and
that is a statement about the **shader**: `LatteBufferCache_collectUniformBlockSources` walks the
shader's own `list_remappedUniformEntries_bufferGroups`, so a shader that names no uniform block returns
none however many blocks the draw binds.

**And the gate that refused them was not needed.** `identityOf` takes the node the title's own binder
published and falls back to the block sources only when there is no node — so an assembly with no blocks
*and* a node is exactly as pairable as one with both, and the gate was refusing the title's
most-drawn shader for a field its own identity function does not use. It now refuses only what cannot be
paired at all, no node **and** no block source, and the renamed counter says so
(`assembliesWithoutAnIdentity`).

**What that buys, measured on the same title after the narrowing** (2026-10-04, gameplay reached and
walked, `/pose?shader=` on the rebuilt runtime):

```
0x6669a23d03806414  84274 assemblies  0 too short   87 unidentified   84187 scanned, all without a block   272 bytes
                    8 candidates of 596, where before the narrowing it had 0 of 414
                      offset   0: moved 2 of 2 comparisons, 1 of 15 other objects read the same   <- per-object
                      offsets  4, 52:                                                 1 of 15     <- per-object
                      offsets 32, 132, 216, 156, 152:                                   8 to 14 of 15  <- shared
0x5ae6d5fe34beb432  12282 assemblies  0 too short  210 unidentified   12072 scanned, all without a block    64 bytes
                    0 candidates: scanned, and the affine class held nothing at any offset
0x2802e519ac163806  58622 assemblies  58622 too short   --                                      32 bytes  cannot hold a pose
0x842a19b509f8b91a   5595 assemblies   5595 too short   --                                      16 bytes  cannot hold a pose
```

**So the answer is two answers, and neither is the one this file asserted.** Two of the four are
genuinely too small: a shader declaring 32 or 16 bytes of uniforms cannot be applying a 3x4 from them
whatever it binds, so those draws are already positioned when they are issued and a blend that leaves
them alone is right, not incomplete. The other two take their uniforms from the **ALU constant bank**
rather than from any block, which `uniformData_updateUniformVars` copies into the same assembled buffer
the blend already writes -- and **that sentence was wrong, and the run that followed is what said
so.** What those 272 bytes hold is now visible: **eight windows, five of them shared
by 8 to 14 of 16 objects — globals, a camera or a projection — and three per-object** (offsets 0, 4 and
52, where 1 of 15 other objects reads the same), with movement counted over **2 comparisons**, which is
the same small-denominator caveat every candidate in this project carries and is stated beside it rather
than divided away.

**Two runs of the narrowed gate, and only the numbers move.** `0x6669a23d03806414`: 84,274 assemblies /
84,187 scanned / 8 candidates of 596, then 60,749 / 60,647 / 8 of 545. `0x5ae6d5fe34beb432`: 12,282 /
12,072 / 0, then 12,298 / 12,054 / 0. The two short shaders unchanged in both, at 58,622 and 40,855 and
5,595 and 5,655 assemblies all too short. **The shape is the finding and the counts are the window.**

### And the blend refuses every one of them, which is the answer the change was for

**Feeding the census's own candidates back through the table, on the real disc at gameplay, with the
stand-in painting twice a tick (1,210 paints) and the camera moving.** The whole census, then each
of the sixteen shaders the blend reports it cannot place, by name:

```
POST /pose offered 386, took 71, refused 315
  0x6669a23d03806414:  4 candidates of 32108 scanned, 272 bytes,  table took 0 of 4
  0x5ae6d5fe34beb432:  0 candidates of  8764 scanned,  64 bytes,  table took 0 of 0
  0x2802e519ac163806:  0 candidates of     0 scanned,  32 bytes,  table took 0 of 0
  0x842a19b509f8b91a:  0 candidates of     0 scanned,  16 bytes,  table took 0 of 0
  0xb00a68512e38669a:  6 candidates of  2065 scanned, 368 bytes,  table took 0 of 6
  0x3a8d0f380931d09b:  0 candidates of  1261 scanned,  48 bytes,  table took 0 of 0
  0xff71dcd2ad4defdc:  1 candidates of  1261 scanned,  80 bytes,  table took 0 of 1
  refusals: shared 210, still 116, none for want of another object
```

(A second run: 385 offered, 77 taken, 308 refused; 214 shared, 105 still. **The counts move, the
shape does not.**)

**So the sentence this section carried a moment ago -- "for the largest share of the frame the pose is
inside the buffer" -- was wrong, and the run that followed is what said so.** The buffer *is* where
those uniforms are assembled, and the census now reads it: 32,108 assemblies, 272 bytes, four
candidates found. **The blend refuses all four, and across the whole feed it refuses 210 as a pass's
value -- the same twelve words as twelve or more of fifteen other objects -- and 116 as a value that
never moved.** What a constant-bank shader carries at that size is a **camera or a projection**: shared
by nearly every object, or static. **There is no per-object pose in it, so narrowing the gate bought
the census a clear look and the blend nothing.** That is the opposite of the claim, and it was worth
the run: before, those 60,361 assemblies were *not examined*, and the report said so only because the
per-shader outcome was added; now they are examined and the answer is measured rather than assumed.

**And the per-shader feed is what makes the question askable at all.** One unscoped `POST /pose` for
four runs, and the blend's own list then said the scene's most-drawn shader was unplaceable with
nothing to say why. Asking by name returns that shader's own outcome and its own refusal
independently, and the two cases separate: a shader the census found and the table refused (the
camera above) is a different problem from one the census never looked at (the 16-byte and 32-byte
shaders, whose scans are zero), and only the per-shader route can tell them apart.

**What is left for the frame's largest share is therefore the vertex bytes, and this is now measured
twice over rather than asserted once.** The short-buffer claim that rested it was half wrong; the
constant-bank claim that replaced it was wrong too, and the feed above is what said so — the values
there are a camera and a projection, shared or static. So the frame's largest share has **no per-object
pose in its uniforms at all**, by measurement rather than by inference. The instrument that would say
more is the vertex-attribute census, and it is still blind (`nodesTracked: 0`), so **that** is the next
step and this document does not claim to know its answer.

**The instrument change this needed, and why it is the same shape as what was there.** The per-shader
counts are kept in `ObjectPoseLocator`'s own keyed table beside `m_byShader`, counted at the gate that
refuses rather than in one place per gate, and reported two ways: a bounded `shaderOutcomes` list in
`json()` and an `assemblyOutcome` object on `GET /pose?shader=`, carrying `largestUniformBytes` and
`canHoldAPose` so the threshold is readable rather than known. `1039 checks, 0 failures`
(`eachShadersOwnAssembliesSayWhatBecameOfThem`, `anAssemblyWithNoBlockButANodeIsScannedRatherThanRefused`).

**And the aux hash, which made the same question unaskable.** The census keys its candidates on the
(base, aux) pair; the route took one sixteen-digit hash and passed `0` for the other, so it matched the
pair exactly and answered "nothing" for any shader whose aux hash it was never told — and "nothing" is
the one answer a caller cannot tell from a real absence. **An aux hash of zero now asks about every aux
hash of that base hash**, for the candidates and for the outcome alike.

**That is the honest gap and it is large.** This run: 134,786 assemblies, of which **131,918 were
draws whose shader the table could not place — 98%.** Run three, whose table caught more of the
scene, was 63%. The mechanism blends what it can place and leaves the rest exactly as the title drew
it, which is the correct behaviour for a write it cannot justify, and it is **not** yet 60 Hz motion
for the whole picture.

**A fifth measurement in the same session: `POST /pose` took 74 of 297, refused 109 as a pass's value
and 114 as a value that never moved, none for want of another object, and lerped 1,415 times at 100%
of the in-between draws.** The stand-in-off arm wrote nothing across 70,152 assemblies, so the guard
holds in the arm that found it broken.



```
third run, armed, 8 s:   assemblies +53,681
                         held 1,007 pairs,  refreshed 33,548,  firstSight 1,007
                         inBetweenKnown 17,673,  lerped 17,673 -- 100% of the in-between draws
                         wordsWritten 212,076  (17,673 x 12)
                         withoutShader 20,133 of 53,681  -- 63% of draws were placeable
third run, stand-in off: assemblies +24,819,  inBetweenKnown 0,  lerped 0,  wordsWritten 0
```

**The stand-in-off arm is the guard's evidence on the title, in the arm that found it broken.** With
the stand-in out the blend wrote nothing at all across 24,819 assemblies, and `inBetweenKnown` is
zero because `installed()` is false. Before the fix that same arm reported lerps on paints it had
been told were the tick's own.

**The refusal breakdown, of the 255, says nothing about the scene — and that is the correction.** Of
295 candidates offered, 40 taken and 255 refused: **202 as a pass's value (the same twelve words as
12 or more of 15 other objects) and 53 as a value that never moved, none for want of another object
to compare against.** I read that as "the census finds the scene's shaders and the table refuses
them", and **that reading was wrong.** The per-shader query says the census has **no candidate at
all** for the four shaders the game draws with most, out of 300 considered. So the 297 offered are
candidates the census has for *other* shaders, and the refusal counts describe those. **A breakdown
of the candidates a census does have cannot answer where the ones it lacks are**, and the two
questions needed two different questions to the instrument.

**And a correction to what the first run seemed to show.** That run reported "none of the scene's
shaders is one the table holds". It should have read **none of the sixteen most-used of 162** — the
list is capped, the harness said so, and the third run's 17,673 lerps are the proof that the other
146 are covered. A capped list read as a whole list is the exact mistake this project keeps making,
and it was in a sentence I wrote.

**So the mechanism is proven and it is not reliable, and the reason is in the table's lifetime.** The
census is cumulative: it has found 162 distinct shaders since boot, of which the per-object signature
admits 56 to 64. The game's *current* draws use 16 shaders it names and 146 it does not, and whether
any of the 56 admitted ones is among the current draws is a property of when the table was fed
relative to what the game happened to be showing. A run that reaches gameplay a second sooner, or
turns a corner, and the table is full of shaders the game has left behind.

**This is a coupling defect in the design and not in the arithmetic.** The blend needs a table of
*the shaders being drawn now*, and it has a table of *the shaders the census has ever seen and
admits*. The two agree only by luck. Three ways out, and the one to build is the first:

1. **The table is fed from the census continuously, and the census expires.** A candidate whose
   shader has not been drawn for some seconds is dropped, so the table holds what is being drawn. This
   is a per-candidate timestamp and a bound, and the report carries how many were expired.
2. **The blend asks the census directly per draw** rather than a snapshot. The cost is a lookup per
   draw in a map that is being written, which is a lock on the display thread's path.
3. **The census reports per shader and the table is fed per shader on demand.** A `GET /pose?shader=…`
   the harness calls for the 16 shaders it has actually seen. This is the smallest change and it is
   driven by the measurement rather than by a guess: **the blend's own unplaced-shader list names the
   shaders that need an answer**, so the harness can ask for exactly those.

**The run also gives the next measurement, and it is the one that decides between them**: of 299
candidates offered, 56 to 64 were taken and 235 to 243 were refused, and the first refusal is
reported as *"compared 0 assemblies, so nothing said whether it moves"*. **So the scene's shaders are
being found and refused, not missed** — and the refusal breakdown, which the report counts and the
harness only prints when nothing was accepted, is where the 235 are named.

**The run also found two things by asking questions the first version of the instrument could not.**

**One: `withoutShader` equal to `assemblies` said the blend was not happening and not why.** The
first run reported 576,432 assemblies and 576,432 unplaced — a total, which says a blend is not
working and names nothing. **The blend now counts the draws it could not place by shader**, bounded
to 16 and ranked by frequency, because a ranked list says *which shaders* and a total does not:

```
0x6669a23d03806414 aux 0x0: 25,882 draws      0x686be36828313d88 aux 0x79: 1,240
0x2802e519ac163806 aux 0x79: 15,700           0x3a8d0f380931d09b aux 0x3c9:   620
0x5ae6d5fe34beb432 aux 0x0:  6,738           ...
```

**Two: the table was fed once, early, from a census that had been accumulating since boot.** A
shader the game drew at the title screen and never drew again is in that table and will never be
drawn again. The run discriminates staleness from a key mismatch by refeeding: **refeeding took 0 of
300 and the table held 64 where it held 64, and the blend's held pairs went on growing — so the table
was not stale, and the keys match.** That is worth stating because the opposite was the obvious
suspicion, and the obvious suspicion was wrong.

### The defect the falsifier found, and the fix

The falsifier arm is the same run with the stand-in **off**, so the display paints once per tick and
there is no in-between paint at all. It reported:

> the blend wrote on paints where it had been told there was no in-between paint

**That is a real defect and it is the kind a single arm cannot find.** `inBetweenPaint()` is the
paint counter's parity. With the stand-in doubling the paints, odd is the in-between. **With one paint
per tick the counter still climbs and the parity still alternates, so every second of the game's own
frame read as an in-between paint and the blend wrote a midpoint into it.** The picture would have
been wrong, and the counters would have said 100% of the in-between draws — because from the blend's
own point of view they were.

The fix is the one `inBetweenPaint()`'s own comment names and deliberately does not enforce:
**`installed()` is the guard, and it is asked rather than inferred.** A blend that derived "is the
stand-in on" from the paint rate would be a second rule about what the stand-in is doing, and the
stand-in is the thing that knows.

The unit test is in `paint_tests.cpp` rather than beside the blend's own tests, because installing a
stand-in needs a guest to install it into and the fake guest is here. It drives the parity explicitly —
**even, odd, even, odd from the first blend draw**, written out rather than assumed, because a test
that read the other parity would pass for the wrong reason — and then takes the stand-in out and
requires four paints of which two read an in-between parity to write nothing. **With the guard
removed, draws 1 and 3 read 35 and 55 instead of 40 and 60 and `lerped` goes to 4.**

### What the RE session settles, and what it rules out

The RE session's findings, recorded here because they change where the missing pose is looked for, and
they are the reason one search is **not** being run:

- **HD has no GameCube-style J3DModel draw-matrix double buffer.** No `+0x94`/`+0x98` swap indexed by
  `+0xb0` anywhere in 2.36M instructions. **The natural place to look for "the pose the object is
  drawn with" does not exist on this title**, and a search for it would have been a search for a
  structure that is not in the binary.
- **HD draws on its logic thread and paints on a separate display thread** (`fpcM_Management`
  `0x025df948`: execute then draw; the frame at `0x0274c264` paints), and the GameCube painter is an
  empty stub. So the pose is filled before the display thread sees it -- consistent with the uniform
  buffer holding it, and inconsistent with anything the display thread recomputes.
- **J3D uploads its uniform buffers through one bind helper** (for example `0x027f1a30`) from a
  **28-byte-entry table, index word at `+76` and pointer at `+28`.** That table is where the buffers
  this mechanism reads are filled, and the RE session is running a caller census over the seven bind
  functions to find which draw code fills them and from which object.

**What that leaves for this side.** The gap is precisely that the scene's most-drawn shaders produce
uniform buffers shorter than 48 bytes, so whatever fills them does not put a 3x4 there -- and the bind
helper's table is where whatever they *do* put is named. **This document does not edit the RE
session's sections and does not duplicate the census it is running.** The coordination is that the
interpolation side has measured *which* shaders are missing and *that* the uniform buffer is too short
for a pose, and the RE side is measuring *where* the missing values are written.

### And the measurements: the blend running, the guard, and what is still missing

**And one more defect, found by the measurement rather than by reading:** the per-shader denominator
was keyed on the **base hash alone**, so shaders differing only in their aux hash were summed --
**45,475 assemblies attributed to one "shader" that is three**, and a denominator three times too
large is a bar nothing clears honestly. It is keyed on the pair now, and the test writes two shaders
sharing a base hash and requires the second's own denominator; with the base alone it fails.

### The J3D bind census, run: seven of its entries could never install, and the rest is the draw code

The RE side's recorded step was a caller census over the J3D functions that hold
`GX2Set*UniformBlock`, and it named seven entries. **Two of them could never have installed, and not
because an address was wrong**: `0x027ff88c` and `0x027ff9c0` are the two binder entries the standing
`UniformBlockCensus` probe already holds, every first word matches the image, and a registration that
holds an entry refuses every other one for it for the rest of the run with `EntryHeldOther` with no way
to take it back. Rerun with eight entries no standing probe holds, each verified against the image
first (8 of 8), on the real disc with gameplay reached and walked:

```
0x027f16e8 installed  141084 calls   0x027f112c x125400, 0x027f11d8 x15600, 0x025e88d8 x84
0x027fe118 installed   13104 calls   0x025ec340 x11232,  0x0257ec98 x1248,   0x0246e38c x624
0x027ffb48 installed   13104 calls   0x025ec518 x11232,  and six more at 312 each
0x0282d098 installed    5702 calls   0x0282daa0 x5702
0x027f1fa8, 0x027ff75c, 0x027cc78c, 0x027d3960   installed, 0 calls
```

**The census is checked against the static answer it was built to replace.** `0x027fe118` has ten
direct `bl` call sites in the image across seven functions; the three the run saw are exactly three of
them, and the ones the title did not draw from are *absent* rather than counted zero.

**What it adds.** Across the J3D range the three setters are called 84 times from 20 functions, and a
whole-image `bl` scan finds direct callers for 3 of the 20 — so 17 are reached only through a pointer,
which is the whole reason this instrument exists. And the descriptor list is **two shapes**: the
binder's sub-object reads `object + 0x10 + *(u32 *)(object + 0x4c) * 0x1c` with the address at `+0x04`
and the size at `+0x0c`, while `0x027f16e8` reads `base + cursor * 0x1c` with the list at `+0`, the size
at `+0x14` and the address at `+0x1c`. **One call binds three blocks**: `0x027fe118` issues nine setter
calls in three stage-triples from three 28-byte lists on the same sub-object -- `+0x10` indexed by the
word at `+0x4c`, `+0x1c` indexed by `+0x58`, and `+0xc4` indexed by `+0x100` (`0x027fe148`,
`0x027fe1ec`, `0x027fe290`), each read at `+0x04` for the address and `+0x0c` for the size -- and
`0x027f16e8` issues fifteen. So "the block
the draw sourced" is at most one of the blocks the draw bound — which is what the two-shape finding and
the three-block finding each say, from the instructions rather than from a count.

### And the named address is the name, not the matrix

The comparison against `0x10163bb4` came back with 48 bytes that are not a matrix in either byte
order: `4.74064e+30, 1.6199e+25, 2.36887e+20, ...` little-endian, and no better big-endian. **That
address holds the string `cWorldViewMatrix[0]`, not the matrix it names.** The name is how the
uniform is *looked up*; the matrix is wherever the registration writes it, which is exactly what
the per-draw uniform table gives. The earlier reading of that address -- "91 of 96 words non-zero" --
was the name's own bytes, and the withdrawal of the "running guest's memory is not the disc image's"
blocker stands, because the name coming out of the image's own module and being read at the same
address in the running guest is the identity that was missing. **What the address does not give is
the matrix, and that is a correction rather than a new blocker.**

### The vertex-attribute census offers nothing, and a zero that offers nothing is not a finding

The same report carries it:

```
drawsSeen 1388163, drawsWithoutPosition 1388163, objectsOffered 0, nodesTracked 0,
blendable 0, identical 0, unpairedShapes 0, schedule perFrame
```

**Every draw it saw had no position attribute, so it offered no object and tracked no node.** That
is a blind instrument reporting zeros, and zeros beside a denominator are the shape of a finding
without being one: the census ran (`schedule: perFrame`), it saw 1.4 million draws, and it found a
position in none of them. It was the deleted vertex blend's falsifier, and it was not kept honest
when the mechanism went.

### Three places the view matrix is not, each with a denominator

A per-object census reads what each draw assembles, and it cannot answer where a *global* uniform is
written, because a global is bound once and read by every shader. `title::GlobalPoseCensus` reads a named
range of guest memory twice, a frame apart, and names every 4-aligned offset where `TransformShape`'s
affine class holds in **both** readings **and moved** -- a static prop's world matrix is the same for
ever while a camera's is different in every frame, which is the difference that matters. The sliding
window collapse matters too: a value repeated across windows is one pose, not several, so the report
counts poses rather than matches.

**The camera was moving for every run below**, gameplay reached and confirmed from the runtime's own
reports rather than from a frame count.

```
the per-draw assembled uniforms   every stage, every object      0 shared by all objects
module .data and .bss             768,055 windows / 3,072,264 bytes
                                  23,210 in the affine class, 0 moved, 0 poses, 3 rounds
0x15800000                        2,097,141 windows / 8,388,608 bytes
                                  0 in the affine class, 0 moved, 0 poses, 3 rounds
```

**The second row is the one to read carefully: 0 windows in the affine class at all, over 2.1 million.**
The reader answered -- it refused nothing, so the 8 MB is guest memory as far as the product is concerned
-- and a region with 23,210 transform-shaped windows elsewhere reads as having none here. **That is what
a range of zeroes looks like**, and it is a statement about where GX2's uniform blocks are *not*, not
about the camera. The scan says both numbers and does not pick one.

### The title names it, and the name was in a table nobody was reading

**`cWorldViewMatrix[0]` is at `0x10163bb4` and `cWorldViewProjectionMatrix[0]` at `0x10163d00`.** Read
from the running guest through `GET /memory` -- 91 of 96 and 78 of 96 words non-zero -- and computed
from the ELF independently as `0x10000000 + (0xa5c3f4 - 0x8f8840)`, which agrees to the byte. The names
are a flat NUL-terminated table followed by `uBlurOffset`, `uOneMinusNearDivFar` and
`cToyCam_Saturation1`.

So the camera was never hidden. It was sitting in a name table that no instrument was reading, while the
host was measuring which 3x4 moved like a camera. **That withdraws the long-standing blocker "the running
guest's memory is not shown to be the disc image's" in the direction it pointed:** the names come out of
the image's own module and the running guest reads at the same addresses, which is the identity of the
two that was missing.

### The block address was a physical one, read as a guest one

**This is the fault the next read was blocked on, and it is in the value, not in the search.**
`GX2Set*UniformBlock` takes a virtual address; the emulator writes `memory_virtualToPhysical` of it
into the uniform-block register and reads it back as `memory_base + physicalAddr`. Only the physical
form reached an observer, and everything since has compared it against a value in the other address
space:

- `UniformBlockAddress` compared each word of the binder's descriptor record against the draw's
  *physical* addresses, and reported **`no word at all`** over 142,682 exact per-object pairs. Two
  addresses for one block, in two spaces, compared like with like cannot hit, and no corpus fixes it.
- Every scan that read a block's *contents* read a range that was never the block's. "No rigid
  transform in 233 whole-block scans" was a true statement about a wrong address.

The fork keeps the guest address per (stage, slot) as the title sets each block and hands over both,
with `LatteFrameHooks::PhysicalBytes` as the matching reader for the physical form. The membership
test now compares the guest address, which is the only form that can hit, and the test passes a
*different* number in the physical slot on purpose -- a test that passed the same number twice would
pass whether the comparison was across spaces or within one. **The test is shown its other answer:
5 of 837 checks fail with the comparison broken, 0 with it in place.**

### Measured on the title: the test fires, and it cannot yet name the word

One driven run, gameplay reached, the camera moving, 1,685,585 assemblies and 1,106,045 bindings
(`GET /blocks`, the `blockAddress` section; the raw report is kept beside the harness):

```
bindings 1106045, records held 2553, evicted 0
assemblies 1685585, of which with a record 1552183 (92.1%)
addresses 4456584, distinct 4028, refused 0
size words 4456584, slots the guest wrote 1121588, distinct written addresses 3409
expected size 768; the leading written guest address 0x47eee100, seen 390161 times
wordHits: word 1 (offset 4)  229631 hits, share 0.1479
          word 2 (offset 8)  229631 hits, share 0.1479
          no other word hit at all
addressWord: null, refused "severalWordsReadSoNoneIsDistinguished"
bestWordShare 0.1479 against a bar of 0.5
```

**So the test fires: from no word at all to 229,631 hits.** The comparison was the fault and the
comparison is fixed. Three things are still true of the answer, and none of them is the code's fault:

1. **Two words tie exactly** -- 229,631 each, to the hit -- so the route refuses to name one. And
   looking at a record explains why: `words: [0x3e634210, 0x3e634300, 0x3e634300, 0x40, 0x40, ...]`.
   **Words 1 and 2 hold the same value**, so a value comparison cannot separate them by anything.
   Words 3 and 4 are both the size. The record repeats itself, and the two repeats are equally the
   address. Naming one of them would be a coin toss with a number attached.
2. **14.8% is a share, not a hit rate.** A record is held per object (2,553 held) and compared
   against every draw that names that object, while a draw usually sources a block some *other*
   object bound -- the sea's per-frame blocks, a pass's shared values. So the share says how often
   the block a draw sources is the one this object bound, and it is not a measure of the address
   being wrong.
3. **The block is 768 bytes, not 64.** `expectedSize: 768` is the record's own size word, and a
   768-byte block holds a 3x4 with room to spare -- which is the answer to the arithmetic slip this
   document once repeated, that "a 64-byte block cannot hold twelve floats". Twelve floats are 48
   bytes, 48 <= 64, and 64 is exactly one 4x4.

**What this unblocks, precisely.** The write location is a *range* and the title names it: the
leading written guest address `0x47eee100` with the record's own size 768 gives `0x47eee100` to
`0x47eee400`, and the leading address overall is `0x47ef6c00`. The next measurement is a scan of
those ranges for a transform-shaped twelve-word window that moves between two frames a frame apart
-- a range, not a search, and not a guess about where the title keeps things.

## The blend, now that the block is a range the title names

The mechanism follows from what is measured, and it is not the mechanism that was deleted.

**Write the block.** Between tick N-1 and tick N the host lerps the twelve words of the pose in the
uniform block the draw sources, the second paint reads the lerped block, and the tick's own frame
reads the block the title wrote for it. Every draw, every skinning pass, every attribute fetch and
every display list is produced by the title's own code at the lerped pose, because the title's own
draw is what reads the block. Identity is the node, so the write is per node and the twelve words
are that node's.

**Why this and not the assembly site.** The deleted mechanism wrote into the *host's* assembled
uniform buffer and re-issued a recorded draw stream -- which is why it had to match identity by
block address and occurrence index, plan partners, and guard every render target. Writing the block
needs none of that: the title already says which block belongs to which object, in a record the
binder writes from the object, and the objective's own chain says so.

**The four things it needs, and the state of each.**

1. *The block's guest address, per node.* **Measured.** The fork hands it over; the record's word 1
   or word 2 names it, and a record repeats the value so the two words cannot be told apart by value
   -- which does not matter, because a blend writes the block and the block's address is the same
   number either way.
2. *The pose's offset within the block.* **Being measured** by the range scan. The block is 768
   bytes, which holds a 3x4 twelve times over; the offset is what says which twelve.
3. *Tick N-1's words still present when tick N paints.* **Measured: 16 of 16**, with 8 consecutive
   pairs on different addresses -- the title's double buffering, so N-1's block is a different
   address from N's and both are readable.
4. *A place to put the in-between frame in order.* **The paint path already has it:** the stand-in
   runs the title's own frame body twice, and the second call is where the lerped block must be in
   place and the first is where the title's own is. So the in-between frame is the tick's own frame
   at the lerped pose, presented before the tick's own -- the order condition 3 asks for, and a
   half-tick of latency rather than a whole one.

**What it needs that is not free, said plainly.**

- **The title's own block has to be put back.** A lerped block that stays lerped is the next tick's
  problem. This is twelve words per object, saved and restored around the second paint, and it is
  *not* the `GuestStateGuard` that was deleted: that shadowed every texture subresource the runtime
  wrote, because a replay re-issued the whole frame. A blend writes twelve words into one block and
  reads them back.
- **A block the title reuses between objects.** The binder hands a pool out as objects come and go.
  A block one object owned at N-1 may be another's at N, and a write keyed on the node's record
  would then be writing into the wrong object. The node's own record names the block *for that
  node*, and the measurement says a draw sources the block it is in the middle of -- so the write
  is keyed on the node, and the case to measure is a node whose block changed between N-1 and N.
- **Per-tick failure is not a crash.** A block the host cannot write, or a pose offset it does not
  have, is one object drawn at N for that one frame, counted by reason. The deleted mechanism's
  skips and their reasons are the pattern; the numbers are the reason the mechanism needed them.

### Measured: the block does not hold it, and that redirects the mechanism

The range scan, on the real title, gameplay reached, the camera moving, the three most-used written
blocks of each round (the report is kept beside the harness):

```
negative control, the module's .data and .bss, 0x1018c0c0 + 3072264:
  768055 windows, 23210 in the affine class, 0 moved, 0 poses
written address 0: 0x3e638c00, seen 27253 times (32.2%)
  181 windows over 768 bytes, 23 in the affine class, 0 moved, 0 poses
written address 1: 0x3e638b00, seen 26750 times (31.6%)
  181 windows over 768 bytes, 14 in the affine class, 0 moved, 0 poses
written address 2: 0x453f0f00, seen 2297 times (2.7%)
  181 windows over 768 bytes,  0 in the affine class, 0 moved, 0 poses
```

Two rounds, identical numbers to the window, so the title was in a steady state and this is not a
sample of a transient. **The block does not hold a transform that moves.** 181 twelve-word windows
over 768 bytes, 23 of them shaped like a transform, and not one of them changed between two readings
a frame apart.

**So "write the block" is wrong, and the section above is withdrawn rather than amended.** The pose
was never in the guest's block: it is in the **assembled** uniform buffer -- the runtime's own copy,
built by `uniformData_updateUniformVars` from the block and the ALU constant registers, and handed to
the observer at `UniformAssembly::data`, which is writable and is the last point before upload. That
is where the per-object census found it at offset 60, with 10 offsets moving and the best at 60 in
every run.

**And that is a better place, not a worse one.** `UniformAssembly::data` is:

- **per draw**, and the draw is the title's own -- the objective's "blend at the game's own draw";
- **writable at exactly one moment**, after the guest's values are read and before the GPU sees them;
- **already carrying the node's identity** through `CommandStreamIdentity` (see "Draw identity" below);
- and it needs **no guest memory write at all**, so nothing has to be put back afterwards.

**The mechanism, restated against the measurement.** The host holds, per node, the twelve words the
node's draw assembled at tick N-1 and the twelve it assembles at N. On the **second** paint of the
tick -- the in-between frame, which the paint path's stand-in already makes possible -- the
assembly hook for that node's draws writes the lerp into that draw's own assembled buffer, and the
title's draw then uses it. The block is not touched, the title's own next tick sees its own values,
and every skinning pass, attribute fetch and display list is the title's own code at the lerped pose.

**What that still needs, and it is one thing.** *Which twelve words of the assembly is the pose.* The
census says offset 60 is the best of 10 offsets that move, in 200,730 of 823,431 assemblies, and
that the value at it is **per-object** -- 6 of 15 other objects read the same value, so a view
matrix is not what it is. Ten moving offsets and one best offset is not a pose until the others are
told apart: an assembly holds a model's matrix, a normal matrix, a texture matrix and a pass's view
projection, and each of those is shaped like a transform. The discriminator is the *node*: the
offset whose value is a function of the node and moves with the node, and which of the ten is the
one every object draw of that node agrees on.

**Native overrides are authorised, and the honest use of that here is narrow.** The blend needs no
override of the title's code: the title reads its own uniforms, and the assembled buffer is the
runtime's own memory. What an override *is* good for is the one thing a memory write cannot do -- see
a value the title computes into a register rather than through a block, which is a title behaviour
rather than a value. That stays a measurement, not a design.

### What the next read is: a range, not a search

The view matrix is a global, so it is in none of the three regions above -- that is the answer, not the
obstacle. The remaining place a pose could be written is **through the registration, by `FUN_02786520`
across 21,488 addresses**, and that is unmeasured. `LatteFrameHooks::UniformAssembly` already carries the
guest block addresses each draw sourced; those were withdrawn as an *identity* -- a block address names a
ring slot, not an object -- **but an identity and a pointer are different uses of the same word, and as a
pointer the range is where the assembled bytes came from.** The data-area scan then stops guessing
`0x15800000` and scans the block the draw actually sourced.

### What the mechanism will not do

- It will not interpolate anything. The picture is the title's own frame twice over, so the picture rate
  is doubled and the motion is not. Nothing moves at sixty until the blend is built.
- It will not hold the display to sixty on its own. The display thread paints as often as it is asked to,
  and the emulator's flip pacing is what asks; `display+0x50` is the title's record of what it asked
  for, not a thing that opens the gate.
- It will not survive a payload that outgrows its reservation, and it is not supposed to: `payload()`
  refuses, and the test enumerates every mode.
- It is not the OpenGL renderer. The assembly site is the Vulkan renderer's, and OpenGL presents at the
  guest's own rate.

## Draw identity: joined through the command stream

**Owner:** `title/CommandStreamIdentity`. **Inputs:** the binder probe (`UniformBlockCensus`, guest
thread) calls `bind(node)`, which records the node at GX2's current write position
(`LatteFrameHooks::GetCommandWritePosition`: buffer start, end and write pointer, host addresses).
**Output:** `objectAt(packet)` on the Latte thread, where `packet` is the draw packet being executed
(`UniformAssembly::packet`, `DrawPrepared::packet`, set by `LatteFrameHooks::DrawPacketScope` in the
command processor's draw handlers). The answer is the last bind written before that packet in the
same buffer, or zero.

**Why not a slot.** The binder runs while the guest *writes* the command buffer; the draw runs when
Latte *executes* it, after more binds. The single-slot `ObjectIdentityScope` this replaces returned
the last bind at execution time. Measured on the title (gameplay, paint mode 13): of 1.83M binds only
187k were followed by a draw-time read before the next bind, 423k reads came after 2-7 binds and 19k
after 8-63, so at most ~34% of bound nodes were ever named to a draw and burst draws took a later
node's identity. Every per-node measurement taken before this change (pose offsets, "per-object"
refusals, coverage) rests on that misattribution and has to be retaken.

**Invariants.** Records live per buffer; a bind whose write pointer goes backwards restarts that
buffer; a buffer whose range overlaps a newer one is dropped; at most `kBuffers` (64) are kept. GX2
submits the guest's buffer by address (`IT_INDIRECT_BUFFER_PRIV`, no copy), so the write pointer and
the executed packet are the same host addresses.

**Measured after.** 75.3% of draw-time lookups named (4.70M of 6.24M), 0 binds outside a buffer;
the rest are draws in buffers with no bind (`lookupsWithoutBuffer` 1.42M, `lookupsBeforeFirstBind`
0.12M). With the correct identity `POST /pose` took 31 of 482 candidates and refused 390 as values
shared across objects (was 71 of 386 and 210), so the uniform "per-object pose" candidates are
camera/pass values, not poses.

**Vertex byte order.** GX2 resolves each attribute's `endianSwap` before the draw, and the title's
32-bit float attributes are `SWAP_U32` (big-endian). The census and the history read them as
little-endian, so 65% of position components failed the plausibility bar and no layout was named.
Both now decode through `title/VertexComponent`. Measured after: 5 of 8 layouts named (strides 28,
32, 48, 64, 96), and the history positions 687k of 1.47M draws (was 0). It still pairs no node across
frames: a node draws several differently sized meshes per frame and its eight samples fill within
one frame, so every node reads `unpairedShapes` or `identical`. That sampling is the next defect.

**Tests:** `tests/cxx/command_stream_identity_tests.cpp`; the census and history tests drive draws
through `tests/cxx/command_stream.h`.

## Where the pose is: model-views in ALU constants, CPU quads in vertex bytes

The model renderer's vertex constants come from a 200-byte shader context (constructor
`0x02873d70`): a 4x4 at `+0x00`, 3x4s at `+0x40` and `+0x70`, the bound program at `+0xa8`.
`0x02874024(ctx, key)` looks a uniform key up in the program's table (`program + 0x25c`) and returns
its register.

| key | uploaded by | what |
|---|---|---|
| 0 | `0x02874038`, 16 words from `ctx+0x00` | projection: 1 distinct value per window |
| 1 | `0x02874074`, 12 words from `ctx+0x70` | model-view: ~30 distinct values per paint per context |
| 2-7, 0x11 | `0x0288285c` | material: texgen rows, colours |
| 9, 0xe-0x10 | `0x02880f1c` | texture matrices |

Key 1 is uploaded only while `ctx+0xb6` is clear, and `0x028740e4` (program change, from the
material set `0x02883ab4`) is the only gameplay writer that clears it; the upload runs inside the
material applies `0x02880f1c` and `0x02880e90`. **Key 1 is a model-view, not the camera's view.**
One context uploads about 30 distinct matrices a paint, often in runs of 3-4; HUD and 2D elements
carry translation z = -989.1. No static store to `ctx+0x70` exists in the renderer
(`0x02870000`-`0x028c0000`); `0x0288285c` only reads it, so it is written through a pointer and
which object it belongs to is not known. `CommandStreamIdentity` does not name these uploads: its
node changes every 4 uploads, unrelated to the matrices.

Static geometry (`6669a23d03806414`, `6a3a79e768f17158`, `48d25cda84f07f19`, ...) is stored in
world space and reads unchanged vertex bytes every frame while the camera moves. Moving particles,
sea waves and sky clouds are CPU-written into `ca2d0854ee6b264d`'s positions every frame
(`POST /draws?frames=60`, standing and walking). The `PoseByShader` per-object uniform search
refuses key 1 as "shared" because the same registers carry every object's model-view.

### The view blend: retired

A blend of key 1 per context, holding the tick's own upload and writing the midpoint at the
register file on the in-between paint, paired unrelated model-views: on in-between paints the
hearts moved and HUD fragments were drawn in the world (`POST /paint` mode 13, gate on, walking),
and with the blend off the HUD was intact while the quad blend still ran. It was deleted with the
fork's `OnAluConstants` hook. Blending key 1 needs the identity of the object whose model-view each
upload carries, which is open: `0x02874d54` calls `0x02880f1c` with its object at `+0x3c`, but the
callers of `0x02880e90` (`0x02879694`, `0x02879d60`, `0x0287a6b8`, `0x0287b8d4`, `0x0287be14`,
`0x0287c740`, `0x0287cc80`) pass stack matrices, and who fills `ctx+0x70` needs a runtime watch on
the address.

### The quad blend

**Owner:** `title/QuadBlend`, fed by `guest/BufferWriters`. `ca2d0854ee6b264d`'s draws are the
quads the particle, sea-wave and sky-cloud writers fill on the CPU each paint: four corners at stride
20, a big-endian `32_32_32_FLOAT` position then 8 bytes of UV, in one of the object's two
alternating buffers. Each write is recorded with the same object's write before it. At the draw, the
fork's `OnDrawPrepared` offers the buffers with the vertex count read by index (Latte's read size,
92 bytes for a quad, would make a fifth vertex of the UVs); the blend hands back a copy whose
positions are `interp::midpoint(before, now)`, and the guest's buffer is not written.

With the gate in, the title writes the same bytes on both paints of a tick. **The write that changes
an object's bytes is the tick's first, and is blended; the repeat is drawn as written.** Paint parity
does not say which paint that is: in one run of four the quads' in-between writes repeated the own
paint's while the view's moved, so the gated tick landed between the paint's quad writes and its view
upload. In that phase the quads trail the view by one paint. A particle's age is the same on both
writes of a tick, so `GuestObject::continuesAs` takes an equal age as the same object.

3D lines (stride 152, 10-12 vertices, a second three-float attribute) are longer than the 80 bytes a
write is checked by and are refused as `longerThanWritten`.

**Measured** (gameplay, mode 13, gate on, standing then walking, gated period): 5,007 blends, all
moved; 4,992 repeats; 32 first sights; 12,456 line buffers refused; 0 without a position, ambiguous
or unblendable.

Evidence: the GX2 HLE's caller histogram (`GX2SetVertexUniformReg` link register, offset, size,
distinct values; a temporary fork-side counter, not kept). A Ghidra caller search by the name
`GX2SetVertexUniformReg` returns only effect passes and misses the whole `0x0287xxxx-0x0288xxxx`
renderer, whose calls go to the import stub `0x028fadac`.

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
memory. `GET /blocks` reports the uniform-block census; `GET /draws` the per-vertex-shader census of draws
whose vertex bytes the title rewrote; `GET /pacing` the intervals between displayed frames with p50, p95,
p99 and longest. `POST /recordings?frames=K` keeps the next K frames' assemblies and `GET /recordings`
returns them, for offline questions the counters cannot answer.

The channel's route table is asserted against its own dispatch, and a route that was deleted is asserted
still deleted: a refusal that advertised a route the channel does not serve would send a reader to a
channel that does not answer.
