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

**Measured on the title, this is not yet done.** The fork change is pinned at `7980661` and the first
run against it is the next measurement, so "the record word is the address" is fixed in the code and
unmeasured on the title, and every downstream statement that rested on "no word at all" is suspended
rather than withdrawn. The arithmetic that produced it was sound; its input was in the wrong space.

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
