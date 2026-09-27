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

### The second paint has to be a call, and the second call is the fault

Mode 8 is the discriminator for the fault that kills `Twice` and `TwiceAtSixty`: paint once by
`bl` and the second paint by a **tail branch**, with the interval call, the vtable rewrite and the
block all unchanged. One word differs between the two payloads -- the second frame branch with its
link bit clear -- and a mutation that puts the link back fails a test asserting exactly that.

Measured on the real title, and it answered:

    at rest:      1683 paints, display 0x43e08af8 (interval 2, phase 2)
    armed mode 8: installed True (tailTwiceAtSixty), block 0x00e05898, 1683 paints
    92.1s:        gate: 0 calls, 0 ticks through it; 1683 calls at its probe

**Mode 8 does not fault -- and it paints nothing: 1683 paints at 63.0s and 1683 at 92.1s, zero
paints in twenty-nine seconds.** The reason is structural, and it is why this shape was tried. The
frame returns through the **link register**, and a tail branch does not change it, so the second
`b` re-enters a frame whose own return goes to the same link register -- the payload word that
branched there. The payload is never left and the pass never returns to the title's loop. Mode 6
works precisely because it branches *once*, leaving the title's own `bctrl`-set return address as
the one the frame uses.

So **the second paint must be a call**, and the second call is what faults. That is narrower than
"painting twice breaks the product", and it is the exact shape conditions 3 and 4 need.

### The frame's first five instructions, and a fix that did not fix it

The frame's own body was disassembled, and its second instruction is the whole of what it does with
the display pointer:

```
0x0274c264  0x7c0802a6  mfspr r0,LR
0x0274c268  0x9421ffe8  stwu r1,-0x18(r1)
0x0274c26c  0x93c10010  stw r30,0x10(r1)
0x0274c270  0x93e10014  stw r31,0x14(r1)
0x0274c274  0x9001001c  stw r0,0x1c(r1)
0x0274c278  0x7c7e1b78  or r30,r3,r3
0x0274c27c  0x4bffedd9  bl 0x0274b054
0x0274c280  0x807e0018  lwz r3,0x18(r30)
```

**The frame moves the display pointer into `r30` on entry and dereferences `r30` for everything** --
`display+0x74`, the field the objective names, is read as `lwz r0, 0x74(r30)` at `0x0274c2c4` --
while treating `r3` as a scratch register it overwrites repeatedly. So `r3` is not the display
pointer after a paint, and that is measured from the image rather than inferred.

Mode 9 is the repair, and the word is **the title's own**: `or r3, r30, r30` (`0x7fc3f378`), which
the frame itself uses five times in its own body, once immediately before each of its own `bctrl`
calls. Mode 9's payload is four words -- paint, restore the pointer, paint, back to the loop -- and
it deliberately has no `li r3,1` and no swap-interval call, since putting a one in `r3` in front of
the frame is putting a one in the display pointer.

**It does not fix it.** Measured on the real title, arming mode 9 over a run that had reached
1,845 paints at rest:

```
at rest:      1845 paints, display 0x43e08af8 (interval 2, phase 2)
armed mode 9: installed True (restoreDisplayTwice), block 0x00e05898
then:         the product stopped answering
```

So **the pointer is not the cause**, or not the whole of it, and the `r3`/`r30` reading above is a
true statement about the frame that does not reach the fault. What that leaves, stated rather than
assumed:

- **The frame is not re-entrant.** It holds per-pass state -- `li r31, 0x1` and
  `rlwinm r31, r31, 0, 0x18, 0x1f` at `0x0274c2c8` is one -- and the second entry continues from
  whatever the first entry left, in the display object and in its callees at `0x0274c038`,
  `0x0274a5ec` and `0x0274b054`. A frame that assumes one pass per call would do exactly this.
- **Something the first pass leaves outside the frame.** The paint walk is in the callees, and any
  of them holding a cursor in the display object or in a global is the same fault seen from further
  out.

### The backtrace, and it is not the guest's fault

The harness had to drive the **capture** path to make the fault reproduce under a debugger -- a mode
3 arming with no capture survives under gdb and kills the product under `null_pair.py` -- so
`gdb_run.py` grew a `--capture` count that arms and reads frames after the mod is held. Every read
in that loop can fail the same way, because the product being gone *is* the outcome the harness
exists to catch, and an exception out of the loop skipped the teardown and took gdb's buffered
backtrace with it. The log it prints as `gdb.log` is a path it never opens; the output is
`run.log`.

With mode 9 armed and four captures requested, the fault reproduced, and this is what it is:

```
Thread 70 "OSSched[core=1]" received signal SIGSEGV
0x00007ffebcb7177d in ?? ()
    0x7ffebcb7177d: movbe 0x3c(%r13,%r13...)   -- movbe 0x3c(%r13,%rax,1),%eax

OSSched[core=1]  hCPU=0x7ffe8538d7a0
  guest pc=0x00e05884  lr=0x02747c84
  r0..r7 = 0274a508 0e275a10 10008000 0000015c 0e275a24 0e275a28 0e275a30 44213980
```

**The guest is not lost.** Its program counter is `0x00e05884` -- the stand-in's own block, which is
where the title's `bctrl` on the rewritten vtable slot lands, and where it should be. Its link
register is `0x02747c84`, in the title's display loop. `r0` is `0x0274a508`, a callee of the frame.
None of that is a guest that has wandered; it is a guest doing exactly what the stand-in asks.

**The fault is a host instruction.** `movbe` is how Cemu's recompiler reads a big-endian guest word
out of its own code cache, and the faulting form indexes it by `%rax` off the cache base in `%r13`.
**One number beside it is not evidence and was nearly written up as if it were.** The harness also
prints `(rsi << 2) / 0x400000`, and it reads `0x7ffe792` -- which looks like a block index four
orders of magnitude outside the table. It is not: `rsi` at the fault is `0x7ffe79218be5`, a **host**
address, and the expression is the harness dividing it by a constant. It says nothing about the
recompiler. The guest-state line above is evidence; that one is arithmetic done on a pointer.

**The obvious suspect was checked and it is not the cause.** The natural next suspect is the direct
jump table, which is indexed by `enterAddress / 4`, and `PPCRecompiler_readJumpTableEntry` is the
guarded accessor -- it calls `PPCRecompiler_hasJumpTableBlock`, refusing an address at or past
`PPC_REC_CODE_AREA_END` and a block at or past the address space's block count. Three reads of that
table in `PPCRecompiler_visitAddressNoBlock` are **not** guarded, and they looked exactly like the
fault: guest code in the **loader's trampoline arena** is mapped and executable, is not in the
recompiler's code area, and would index a table that does not have it.

They are inside `#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION`, and that macro is `0`. **The compiled
path was already guarded** -- its quick read-only check calls the accessor and refuses, and a second
`hasJumpTableBlock` call under the lock was added by the fork earlier for the same reason. A patch
routing the dead reads through the accessor was written, found to add a duplicate declaration to
live code, and **reverted**: it would have been a change to the emulator justified by a code path
that does not compile.

So the jump table is cleared and the fault is elsewhere in the host. The access itself is small and
in bounds, which is the finding that redirected everything: `%r13 = 0x7ffed4000000`, `%rax = 0x15c`,
address `0x7ffed4000198` -- **base plus 0x198** -- and

```
0x7ffed0ffa000-0x7ffed4000000  0x3006000  rw-p    <- a 48MB region
0x7ffed4000000-0x7ffed4010000  0x10000   ---p    <- the base register points here
0x7ffed4010000-0x7ffed4100000  0xf0000   rw-p
0x7ffed4100000-0x7ffed4e00000  0xd00000  ---p
```

**And `%r13` is not the code cache at all -- it is `memory_base`, and the whole reading changes.**
`BackendX64.cpp:1647` emits, in every generated function's prologue:

```c
// MOV R13, memory_base
x64Gen_mov_reg64_imm64(&x64GenContext, REG_RESV_MEMBASE, (uint64)memory_base);
```

and every guest load and store is emitted as a direct access at
`REG_RESV_MEMBASE + register + imm` -- `x64Gen_movBEZeroExtend_reg64_mem32Reg64PlusReg64` and its
siblings, with no page check of any kind. So `movbe 0x3c(%r13,%rax,1),%eax` is **a guest load at
guest address `0x15c + 0x3c = 0x198`**, and `memory_base` is the base of a **4 GB `PROT_NONE`
reservation** (`MMU.cpp:132`, `MemMapper::ReserveMemory(nullptr, 0x100000000, P_RW)`, which is
`mmap(..., PROT_NONE, ...)`), into which the guest's ranges are mapped. The `rw-p` / `---p`
alternation is that reservation and its mapped ranges; the `---p` page at the base is the part of
the reservation the guest has never touched.

**So the previous two readings of this fault are both withdrawn.** It is not a released code-cache
block, and the padding overflow is not its cause. It is a **guest load from guest address 0x198** --
a near-null dereference on the second pass, faithfully translated, with the program counter and link
register exactly where the title put them. The direct emission is not a bug: it is how a recompiler
is supposed to reach guest memory, and it is correct for every page the guest has touched.

That fits every observation, which is why it is worth the two withdrawals:

- **Modes 6 and 8 survive** because neither reaches a second pass, so nothing new is asked of the
  guest.
- **Modes 2, 3 and 9 all fault** because the second pass does reach a dereference of a small value
  as though it were a pointer -- `lwz` at `0x3c(rA)` with `rA = 0x15c` -- and the first pass never
  got there.
- **The frame's own `bctrl` chain is the likely route**: it loads a call target from the display
  object (`mtspr CTR, r0` after `lwz r0, 0x74(r30)` and two more) and dispatches through it, so a
  display pointer that is not the display on the second entry turns into a small value used as a
  call target or a base.

### The frame's own dispatch chain, and three payloads that did not fix it

**The frame's first eight words, read out of the listing's own bytes** (`q_both_searches.py`, via
`instruction.getBytes()` -- this Ghidra binding's `Memory.getBytes` returns zeros, and a search
built on it would find nothing while looking as though it had worked):

```
0x0274c264  0x7c0802a6  mfspr  r0                <- SPR 8, the link register, into r0
0x0274c268  0x9421ffe8  stwu   r1,-0x18(r1)
0x0274c26c  0x93c10010  stw    r30,0x10(r1)
0x0274c270  0x93e10014  stw    r31,0x14(r1)
0x0274c274  0x9001001c  stw    r0,0x1c(r1)
0x0274c278  0x7c7e1b78  or     r30,r3,r3           <- the display pointer
0x0274c27c  0x4bffedd9  bl     0x0274b054
0x0274c280  0x807e0018  lwz    r3,0x18(r30)        <- the first sub-object
```

**What this corrects.** The listing previously shown here started at `0x0274c278` and so **omitted
the entire five-word prologue**, which means the `lwz` it showed at `0x0274c288` is the image's
`0x0274c280`. The offsets past the first eight words are *not* re-verified by that read, and the ones
before it are now measured rather than quoted. The first load is `display+0x18` into `r3` -- and a
later section, which reads all 85 instructions, shows that is **not** where the call targets come
from, so the "`display+0x18` not `display+0x24`" reading that was briefly written here is withdrawn.

The prologue does say something about the objective's payload: **the frame's first instruction
clobbers `r0` with the link register**, storing it at `0x1c(r1)`. `r0` is therefore not a register
the frame preserves, and a stand-in that expects `r0` to still hold something of its own across the
call is expecting a register the callee's first instruction overwrites.

The rest of the dispatch, as listed before:

```
0x0274c28c  lwz   r12,0xd4(r10)
0x0274c290  mtspr CTR,r12
0x0274c294  or    r3,r30,r30
0x0274c298  bctrl
0x0274c29c  lwz   r12,0x24(r30)
0x0274c2a0  lwz   r0,0xdc(r12)
0x0274c2a4  mtspr CTR,r0
0x0274c2a8  or    r3,r30,r30
0x0274c2ac  bctrl
...
0x0274c31c  bl    0x0274c038
0x0274c338  or    r3,r30,r30
0x0274c33c  bl    0x0274b06c
0x0274c340  addi  r3,r30,0x80
0x0274c344  bl    0x02760e58
0x0274c390  lwz   r0,0xe4(r11)
0x0274c394  mtspr CTR,r0
0x0274c3a0  lwz   r0,0x1c(r1)
0x0274c3a4  lwz   r30,0x10(r1)      <- and back out
```

**Every one of the frame's calls is a `bctrl` through a target loaded out of the display object** --
`display+0xd4`, `display+0xdc`, `display+0x6c`, `display+0xec`, and one more at `0x0274c390` -- with
the argument rebuilt as `or r3, r30, r30` each time. **There is no callee reachable by a name in the
frame's own words** except the three direct `bl`s, and the paint itself is one of the `bctrl`s.

And one field gates two of them:

```
0x0274c2c4  lwz    r0,0x74(r30)
0x0274c2cc  rlwinm. r12,r0,0x0,0x1f,0x1f     <- bit 0
0x0274c2d4  beq    0x0274c2e4
0x0274c2d8  rlwinm. r0,r0,0x1f,0x1f,0x1f     <- bit 31
0x0274c2dc  beq    0x0274c2e4
0x0274c2e0  li     r31,0
0x0274c2fc  cmpwi  r31,0
0x0274c300  beq    0x0274c320                  <- skips 0x0274c304..0x0274c31c
0x0274c304  lwz    r10,0x24(r30)
0x0274c308  lwz    r0,0xec(r10)
0x0274c30c  mtspr  CTR,r0
0x0274c310  or     r3,r30,r30
0x0274c314  bctrl
0x0274c318  or     r3,r30,r30
0x0274c31c  bl     0x0274c038
...
0x0274c38c  stw    r0,0x74(r30)                <- and the frame writes the field
```

So `display+0x74` is not the flip counter the objective's note assumed. It is what the frame
**branches on at entry** and **writes on exit**, and it decides whether a virtual `bctrl` and a call
to `0x0274c038` happen at all.

**Three payloads, three hypotheses, three faults.** All measured, all on the real title, all armed
over a run that had reached 1,845 or 1,983 paints at rest:

| mode | hypothesis | result |
|---|---|---|
| 9 | the second call needs `r3` back, from the frame's own `r30` | faults |
| 10 | the second call needs `display+0x74` back, from the frame's own words | faults |
| 8 | the second paint can be a tail branch, not a call | paints nothing |

Mode 10's payload is worth keeping for what it is: **every word in it is the frame's own, verbatim**
-- `lwz r0,0x74(r30)` from `0x0274c2c4` and `stw r0,0x74(r30)` from `0x0274c38c`, both on the
frame's own `r30` -- so it introduced no encoding of its own and still did not fix the fault. Its
test also caught a fourth thing: a first version checked word 1's branch displacement against the
*block's* address rather than *its own*, which makes the displacement four bytes long and lands four
bytes past the frame. The same mistake the gate's counters were once read for, in a payload.

### The five call targets, read, and they are the same on both paints

The probe that already sits on the frame now reads all five at every paint and keeps the last two,
so a pass's pair is visible side by side. **The offsets are on `*(display+0x24)`, not on the
display** -- the frame does `lwz r10, 0x24(r30)` and every target load is `0x??(r10)`. A first
version read them as `display + 0xd4` and got four zeroes and one `0x00400000`, which is what a
wrong base looks like and not what a title with no call targets looks like.

Read through the base, on the real title, at the last two paints:

| field | value | |
|---|---|---|
| `*(display+0x24)+0x6c` | `0x02747818` | |
| `+0xd4` | `0x0274c67c` | |
| `+0xdc` | `0x02034ffc` | |
| `+0xec` | `0x020350c4` | |
| `+0xe4` | `0x0274c874` | **the address the objective names as the flip-wait test** |

**All five are code addresses in the image, and all five are identical on the two paints**, as are
`display+0x74` (0 on both) and the phase field `display+0x28` (2 on both). So the second paint finds
the same call targets the first one did, and **the display object is not where the second paint
diverges.** That is the hypothesis this measurement was built to test, and it is refuted by it.

**What this leaves, and it is now a short list.** The guest's near-null dereference at address
`0x198` on the second pass is not in the display's fields, its flag, its phase or its call targets.
It is in state the frame's *callees* hold or leave: the five named callees are `0x0274b054`,
`0x0274a5ec`, `0x0274c038`, `0x0274b06c` and `0x02760e58`, and the frame calls each of them once per
paint. So the bounded next measurement is a probe on those five, reading the guest's registers on
entry, and the question is which of them is entered on the second paint and with what.

That is the same instrument at five more addresses, and it is the last place the divergence can be
that the disassembly points at: after them the frame returns, and the payload's only remaining act
is the branch back to the title's loop.

### What the objective's own payload shape implies about `0x198`

The objective's payload is `819f0024 800c00cc 7c0903a6 7fe3fb78 4e800421` twice then `4e800020`,
and read as instructions that is `lwz` of the vtable slot, `mtctr`, `bctrl` -- twice -- then a
branch. **The payload loads the frame's address out of the vtable rather than carrying a literal**,
and the mode numbers in this file all reach the frame by a literal `bl`.

That matters because the faulting instruction is *itself a guest load*, and a load off a register
that holds a small value lands at a small address. `0x198` is `0xcc + 0xcc`: the payload's own
`lwz r, 0xcc(rX)` with `rX` holding `0xcc`, or any `lwz` at offset `0xcc` off a register holding a
small number. So the shape of the fault is consistent with **a vtable pointer that is not the
vtable** -- the register the load is based on being a small value rather than `0x10004e88`, which is
what the mod's own report shows (`vtable` and `liveVTable` both `0x10004e88`).

That is a reading, not a measurement, and it is recorded as such: nothing here has shown the register
that held `0xcc`. What would test it is the same probe at the *payload's* first word rather than at
the frame's, reporting the guest register the `lwz` is based on. The paint mod writes the payload and
knows its address (`block`, in the report), so a probe there is the same instrument one word earlier
in the same instruction stream -- and it is the one measurement that would say whether the
vtable-derived path the objective specifies and the literal-address path this file's modes use fail
for the same reason or for two different ones.

### The objective's own eleven words, and what they depend on

Built exactly as the objective specifies, as mode 11 -- its group of five twice, then a branch to
the loop:

```
819f0024  lwzu   r3,0x24(r30)    the display's sub-object, updating r30
800c00cc  lwz    r12,0xcc(r0)    the vtable's slot 0xcc
7c0903a6  mtspr  CTR,r12
7fe3fb78  or     r31,r3,r3
4e800421  bctrl                 the frame, reached through the vtable
```

Ten of the eleven words are verbatim, and the test asserts them word for word rather than
asserting that the payload differs from another one, which a payload with an invented encoding would
also satisfy. The eleventh is the objective's `4e800020` as a form, with its displacement computed:
a `b` with a fixed displacement reaches one address, and the stand-in's block is handed out at run
time.

**It does not fault, and it paints nothing.** Armed on the real title over a run that had reached
1,854 paints at rest: 1,854 at 68.0s and 1,854 at 97.1s, the gate reading 1,854 calls at the probe
and nothing in it, and the capture refused with no image reaching its slot in 25 seconds.

**And the reason is not a missing register -- it is that the payload's second word reads the slot
the mod has just rewritten.** The vtable the mod reports is `0x10004e88`, and the frame is at slot
`0xcc` of it:

```
0x10004e88 + 0xcc = 0x10004f54
```

**`0x10004f54` is the address the objective itself names as the slot to rewrite with the stand-in's
address.** So `lwz r12, 0xcc(r0)` -- `r0` holding the vtable, which is the register convention the
objective's own `0xcc` displacement implies -- reads the stand-in's address, not the frame's.
`mtspr CTR, r12` then takes the stand-in, and `bctrl` calls the stand-in again.

**That is a measured self-reference, and it is a contradiction inside the objective's own
specification.** Condition 1 asks for two things at once: *reach the frame by rewriting vtable slot
`0xcc`*, and *re-read the frame from the title's own vtable*. Those are the same word. The payload
re-reads slot `0xcc` and gets the stand-in, so a stand-in that reaches the frame through that slot
calls itself.

It also fits the measurement precisely, and where the fit is informative. The run did not fault and
did not paint: 1,854 paints at 68.0s and 1,854 at 97.1s. A self-call would recurse, and a
recursion that never reached the frame would leave the paint count exactly where it was -- which is
what happened, and is a different signature from the `0x198` load that the literal-`bl` modes fault
on. **So the two payload families fail differently**, and the objective's is the one that does not
reach the frame at all.

The first word has a second problem of its own: `lwzu r3, 0x24(r30)` presumes `r30` holds the
display, and the update form leaves `r30` advanced by `0x24`, so the second group would read
`display + 0x48` rather than the place the first read.

**What the mod has that the payload does not: the frame's address, before the rewrite.** The mod
reads the vtable out of the running display on every arming, reads slot `0xcc` to know what the
title was going to call, and checks the frame's entry word against the image before it will install
anything. So it knows `0x0274c264` as the original contents of the slot it is about to overwrite,
and that value -- not the rewritten one -- is what a payload reaching the frame through the vtable
needs. The contradiction is therefore resolvable from inside the mechanism, and the resolution is
one value, not a new mechanism: the payload's call target has to be the slot's *original* contents.

**And supplying it costs a word the objective's payload does not contain.** The payload's only load
is `lwz r12, 0xcc(r0)` -- a fixed displacement off a register the stand-in does not own. To make
that load return the mod's saved value, the mod would have to put the frame's address somewhere the
payload reaches, and every way of doing that needs either a different displacement (`lwz r12, 0xd0(r0)`
to a neighbouring slot the mod writes), an absolute load of a literal, or a PC-relative
materialisation (`lis`+`ori`). **All of those are derived words, not lifts**, and this project has
measured what a derived word costs twice: a branch displacement worked out by hand landed four bytes
past its target and was caught only because a test happened to notice, and an `or` encoding worked
out from the manual matched *nothing* in nine megabytes of PowerPC.

**So the two routes are mutually exclusive as they stand, and the measurements say which works.**

- **The literal-`bl` route** -- modes 2, 3, 9 and 10 -- reaches the frame every time and faults on
  the second call, on a guest load at `0x198`, with the guest otherwise healthy. The display object
  is cleared: its flag, its phase and all five of its call targets are identical across the two
  paints, and the divergence is in state the five callees hold.
- **The vtable route** -- the objective's own eleven words -- cannot reach the frame at all, because
  the slot it reads is the slot the mod rewrote.

**And the one shape that reaches sixty does neither.** Mode 6 is a single `b` at the frame with the
swap interval at one vblank a flip, and it measures 59.99 and 60.12 paints a second against 30.12
unmodded, in adjacent windows, twice. **The sixty comes from the flip interval, not from painting
twice** -- so the picture rate does not need a second pass through the display frame at all, and the
fifty-nine-to-sixty measurement stands on its own without the second paint.

**The conclusion this section reached is withdrawn**, because the evidence under it is not what it was
taken to be. It said the two routes were "mutually exclusive" and that conditions 3, 4 and 5 were
therefore "in tension on this title". That rested on the fault being the display frame's own
non-re-entrancy -- on a guest `lwz` at `0x3c(rA)` inside the title's code. The run below puts the
faulting instruction **outside the title's RPX entirely**, so "the frame is not re-entrant" was never
measured; it was assumed from where the fault surfaced, and the surface was the emulator's interpreter
rather than the frame. Nothing here establishes a tension, and the in-between frame is not shown to be
out of reach. What is left is a located fault with a named address and no named cause yet.

### The fault, located properly: the interpreter, and not the title's own code

Two independent runs of mode 3 under gdb, each with the capture workload that the fault needs, both
faulting the same way:

```
#0  ppcMem_readDataU32 (hCPU=..., address=1763)   at PPCInterpreterImpl.cpp:72
#1  PPCInterpreterContainer<...>::PPCInterpreter_LWZ (hCPU=..., Opcode=2147682018)
                                              at PPCInterpreterLoadStore.hpp:285
#2  PPCInterpreterSlim_executeInstruction       at PPCInterpreterImpl.cpp:1257
#3  coreinit::__OSFiberThreadEntry              at coreinit_Thread.cpp:1365
```

**Three facts, and each one contradicts something this file has said.**

**It is the interpreter, not recompiled code.** The whole previous reading of this fault -- the
`movbe 0x3c(%r13,%rax,1)` in a generated prologue, `%r13` as `memory_base`, the address
`0x7ffed4000198` -- was taken from a thread that `guestpcs.py` believed was in recompiled code. It
was not; the fault is `PPCInterpreterSlim_executeInstruction`, the JIT's *fallback*. Per this
project's own rule that the interpreter is a bounded fallback used only after the recompiler reports
a block cannot be compiled, **the second pass is running where the recompiler declined**, and that is
a fact about the patch rather than about the title's draw path. The `lwz` handler passes
`(rA ? hCPU->gpr[rA] : 0) + imm`, so with `imm = 0x6e2` and `rA = 0` the guest read guest address
`0x6e2` with `r0` contributing nothing.

**The instruction gdb named does not survive its own second run, so it is not evidence.** Across two
runs of identical code the backtrace reported the effective address as 1762 and then 1763, the opcode
as `0x800006e2` and then `0x800306e2`, and the guest program counter as three different values. One
fixed instruction cannot be all of those, so the `0x6e2` displacement and the `r0 = 0` are both read
off a frame that has already been unwound, and neither is a measurement.

**What can be checked without gdb is the image, and it answers.** Searching the analyzed program's
instruction text for a load at `0x6e2`:

```
scanned 29,408 functions, matching on the full instruction text
  0x6e2(r0):  0 instructions in 0 functions
  0x6e2(  :   2 instructions in 2 functions   -- lbz r0,0x6e2(r31) at 0x021c54e0
                                                -- lbz r12,0x6e2(r3) at 0x021c5cb8
  control, 0x3c(:  3,047 instructions in 1,608 functions
```

The title's code contains **exactly two** loads at displacement `0x6e2`, both `lbz`, neither off
`r0`, in `FUN_021c54e0` and `FUN_021c5cb8`. A search that found none would be indistinguishable from
this one, which is why the control is there and why it finds three thousand.

**So the fault is not the title's draw path** -- not because the faulting instruction was located
outside `cking.elf`, which the gdb numbers no longer support, but because the shape the fault was
reported with is not a shape the title's code contains, and because the fault arrives through
`PPCInterpreterSlim_executeInstruction`, the JIT's *fallback*. **The second pass is running where the
recompiler declined**, and that is a fact about the patch rather than about the title's draw path.
Which of the two remaining causes it is, and where `FUN_021c54e0` and `FUN_021c5cb8` sit relative to
the display thread, are not yet measured.

### The guest is the image, and the debugger was reading it backwards

The section this replaces said the running guest's memory did not match the disc image's. **That was
a byte-order artifact, and the image is the guest.** The product's own accessor settles it --
`GET /memory` goes through the fork's `GuestCallProbes::GuestBytes`, which returns null unless every
byte of the range is mapped guest memory (`GuestMemoryRead.h:26-31`), so it is a read that refuses by
reason rather than one that returns whatever the host had there:

```
  the frame, guest 0x0274c264, as the product reads it
    4a6b961c 9421ffe8 93c10010 93e10014 9001001c 7c7e1b78 4bffedd9 807e0018
  the frame, guest 0x0274c264, as gdb's x/8wx read it
    1c966b4a e8ff2194 1000c193 1400e193 1c000190 781b7e7c d9edff4b 18007e80
  every one of the eight byte-reversed: True
```

**gdb reverses every word it prints for big-endian guest memory.** Eight words, eight reversals, no
exceptions -- and one of them is the frame's own `or r3,r30,r3`, which no coincidence produces. So
every guest word this project's debugger has read was byte-swapped, and the conclusions drawn from
them are withdrawn: that the running guest differs from the image, and that the faulting opcode is
absent from the title's RPX.

**The one word that genuinely differs is the mod's own probe**, and it is a branch because that is
what a probe is. `GuestCallProbes::Install` writes
`memory_writeU32(registration.entry, RelativeBranch(entry, stubAddress))` and keeps the image's word
inside the stub, which the arena dump shows at its own offset: `040004e4` (the HLE `bl`),
`7c0802a6` (the displaced `mfspr r0`), `3d80027f 618cf890` (the resume address), `7d8903a6`
(`mtctr r12`) -- exactly `WriteStub`'s layout, and the displaced word is the image's. So the frame's
first word at run time is `0x4a6b961c`, a relative branch into that stub, where the frame's real first
instruction waits.

**And the vtable slot, read by the product, agrees with the objective's arithmetic:**

```
  guest 0x10004f4c:  0274c00c 00000000 0274c264 00000000 0274c67c 00000000
                     vtable+0xc4  vtable+0xcc = the frame   vtable+0xd4
```

`0x10004e88 + 0xcc = 0x10004f54` holds `0x0274c264`, and `+0xd4` holds `0x0274c67c`. That is also why
the mod installs at all: it refuses unless slot `0xcc` holds `kDisplayFrame` and the loop's top holds
its expected first word, so **the mod working is itself the evidence that the guest is the image.**

`arena.py` now reads `/xb` and assembles big-endian, and its check moved to the frame's *second*
word -- the first is the probe's, and cannot judge the read.

### The frame's display fields, measured rather than quoted

Two claims have been made about this function from its first eight words: that all five call targets
come from `*(display+0x24)`, and then that this is "wrong by an offset" because the first load is
`+0x18`. Both are claims about a prologue. The frame sets `r30` to the display on its sixth word and
restores it from the stack on the way out, so every load off `r30` anywhere in it is a display field.
All 85 instructions, `q_together.py`:

```
  distinct display offsets READ off r30: 5
    display+0x18   1 read,   first at 0x0274c280
    display+0x24   5 reads,  first at 0x0274c288
    display+0x28   1 read,   first at 0x0274c35c
    display+0x4c   2 reads,  first at 0x0274c2d0
    display+0x74   2 reads,  first at 0x0274c2c4
  distinct display offsets WRITTEN through r30: 6
    display+0x28   1 write,  first at 0x0274c378
    display+0x74   1 write,  first at 0x0274c38c
    display+0x78   1 write,  first at 0x0274c350
    display+0x7c   1 write,  first at 0x0274c354
    display+0x80   1 write,  first at 0x0274c368
    display+0x84   1 write,  first at 0x0274c360
  loads off a register that is not r30: 8
    0x0274c28c lwz 0xd4 off r10      0x0274c2b4 lwz 0x6c off r10
    0x0274c2a0 lwz 0xdc off r12      0x0274c308 lwz 0xec off r10
    0x0274c390 lwz 0xe4 off r11      (three restores off r1)
```

**Both claims were half right and the second was wrong, and this settles it.** `+0x18` is read once,
into `r3`, early. `+0x24` is read **five** times, and every one of the frame's five call targets
(`+0xd4` off `r10`, `+0xdc` off `r12`, `+0x6c` off `r10`, `+0xec` off `r10`, `+0xe4` off `r11`) is
loaded through a register the `+0x24` chain supplies. **The mod is right**: `kFrameTargetBaseOffset =
0x24` with `kFrameCallTargetOffsets{0x6c, 0xd4, 0xdc, 0xec, 0xe4}` is exactly the frame's own access
pattern, confirmed against the image rather than assumed. The `display+0x18` correction is withdrawn.

The two fields the objective names are confirmed too: `+0x74` is read twice and written once at
`0x0274c38c`, which is the documented `stw r0,0x74(r30)`, and `+0x28` is read once and written once --
`kPhaseOffset = 0x28`. The four counters the frame keeps are `+0x78`, `+0x7c`, `+0x80`, `+0x84`, and
`kCounterOffset = 0x78` is the first of them.

The query carries its own falsifier, because a parse that finds nothing where something is known to be
is a defect and not a finding: the first version of it put the displacement *after* the bracket, found
zero loads in a function full of them, and the check on a word the image and this file agree on
(`lwz r3,0x18(r30)`) said so.

### The objective's payload hands the frame the wrong pointer, and both of its defects are now named

The eleven words were built verbatim and measured: no fault, no paint, 1,854 paints at 68.0s and 1,854
at 97.1s. With the frame's own access pattern now measured rather than quoted, the reason is exact.

**The frame takes the display and dereferences `+0x24` itself:**

```
0x0274c278  or    r30,r3,r3        the display pointer, from r3
0x0274c280  lwz   r3,0x18(r30)     the display's +0x18
0x0274c288  lwz   r10,0x24(r30)    the call-target base -- read five times in the function
0x0274c28c  lwz   r12,0xd4(r10)    a call target, off that sub-object
```

**The payload's first word does that `+0x24` dereference itself, in the caller:**

```
819f0024    lwzu  r3,0x24(r30)    the call-target base into r3, and r30 := r30 + 0x24
7fe3fb78    or    r31,r3,r3        the base into r31
4e800421    bctrl                  the frame
```

So `bctrl` calls the frame with `r3` = the **sub-object**, not the display. The frame's first act is
`or r30,r3,r3`, which makes `r30` the sub-object, and from there every field it reads is
`sub-object+0x18`, `+0x24`, `+0x74` rather than the display's. **`+0x24` is read five times and each
read feeds a `bctrl` target**, so a stand-in that hands over the wrong level of indirection does not
merely read the wrong fields -- it dispatches through targets read out of whatever the sub-object
happens to point at. That is a direct route to the arena finding below.

The `lwzu` form compounds it: the update leaves `r30` advanced by `0x24`, so the payload's second group
reads `display + 0x48` rather than where the first read. **The objective's two groups are not two
passes over the same tree; they are two passes over two different addresses.**

**So the payload has two independent defects, and neither is a missing register:**

1. **Word 1** pre-dereferences the call-target base and passes it to a frame that wants the display.
2. **Word 2** (`lwz r12,0xcc(r0)`) reads the slot the mod has just rewritten, so `mtspr CTR` takes the
   stand-in's own address and `bctrl` calls the stand-in again.

Both were predicted by this file's own measurements before they were run, and the run agrees. **A
payload with eleven words has one call target and one call site; this one has two of each and they
disagree.**

### Where the second paint actually goes: into the loader arena's data

The fault's program counter is in the loader arena every run -- `0x00e0006a8`, `0x00e000768`,
`0x00e000e28` across three. That range is not mystery: `MEMORY_CODE_TRAMPOLINE_AREA_ADDR` is
`0x00E00000` with a 2 MiB size (`MMU.h:145-146`), it is what `RPLLoader_AllocateTrampolineCodeSpace`
hands out from (`rpl.cpp:86`), and it is registered with the recompiler wholesale at init
(`PPCRecompiler.cpp:759`) because the loader does put real code in it.

**And its base is not code.** Read as bytes, because of the reversal above, the first 256 bytes:

```
0x00e00000  04 00 01 96  04 00 02 78  04 00 02 8d  04 00 02 91      HLE calls, one per entry
0x00e00010  04 00 02 96  04 00 02 9b  04 00 02 9f  04 00 02 a5
0x00e00020  04 00 02 b0  04 00 02 b3  04 00 02 b7  04 00 02 bf
0x00e00030  04 00 02 c2  04 00 02 ff  4e 80 00 20                  then `bctr`
0x00e00038  6e 6e 5f 61  63 74 2e 46  69 6e 61 6c  69 7a 65 5f 5f   `nna_act.Finalize__Q32_2nn3actFv`
0x00e00060  00 00 00 00  ...                                          zeros, at least 0x100 bytes
```

`0x0400xxxx` is cemu's HLE call encoding -- `1u << 26 | hleIndex`, the same shape `WriteStub` writes for
a probe's dispatch -- so **the arena's first `0x38` bytes are the HLE function registry's code, what
follows is its symbol names, and then zero padding.** The mod's stand-in is at `0x00e05898`, about
22 KiB past that base, and the probes' stubs sit in the same area: the dump shows one at `0x00e058b4`
holding `040004e4`, the displaced `7c0802a6`, the resume `3d80027f 618cf890`, and `7d8903a6`.

**So a branch into the arena lands in the registry table or the zeros after it and executes data as
code.** That accounts for the whole signature with no appeal to the display's state: a range registered
as executable is translated as instructions, and the recompiler's behaviour there is not its behaviour
on real code. The display object being cleared is what you see when the frame was never properly
entered, not a cause.

**What is still unknown is which branch goes there.** The candidates are narrow: the stand-in's own
control flow after the frame returns, or one of the frame's five `bctrl`s. The five targets come from
`display+0x24`, measured identical across paints, so the targets themselves are not the divergence --
which makes the mod's own payload the place to look, and the objective's payload, which dispatches
through a sub-object's fields, a candidate for exactly this.

### The registrations print came back empty, and neither reading is built on

`InstallRegistered` never clears `s_registrations` -- it installs, hands a non-holding entry its own
word back, and advances the index -- so an empty deque at the fault means either that no probe was
registered in that run, or that gdb resolved a different internal-linkage instance of the anonymous
namespace's object than the one the product uses. A previous run printed nine registrations through the
same expression, so both readings are live and one more check would tell them apart. Recorded as
unresolved rather than as "no probes are installed", which is what a single empty print invites.

### The faulting instruction, decoded with the byte order right

With the reversal undone, the opcode the interpreter was executing is `0xe2060380`, and

```
scanned 9,432,460 executable bytes in 17 blocks
  control 0x7c7e1b78 (the frame's sixth word): 4,893 matches
  sought  0xe2060380:                             0 matches
```

**So the faulting instruction is not in the title's own RPX -- and that now survives the byte-order
fix, which the earlier version of the claim did not.** `0xe2060380` decodes as `lwarx r16, r6, r0`, a
load-and-reserve, which is the shape a lock takes and not the shape a display path takes. The
interpreter frame's own name (`PPCInterpreter_LWZ`) cannot be taken at face value either, since an
`lwarx` is not what that handler executes; the backtrace's argument values are read off a frame that
has been unwound, which is the same reason the effective address differed between two runs.

What is consistent across every run of the fault: the program counter is in the loader arena at
`0x00e000xxx` and the link register is inside the stand-in's block. **So the second paint enters the
arena and leaves it somewhere the title's own code does not account for, and the instruction it
executes there belongs to a module this project has not analysed.**

### A search that reported a falsifier it had not earned

Worth recording because it nearly became a retraction of a correct reading. Searching the image for
the fault's `0x3c` displacement, over every function:

```
instruction.getOpObjects(0)   ->   "lwz r0"            for lwz r3,0x48(r0)
instruction.toString()        ->   "lwz r3,0x48(r0)"
```

`getOpObjects(0)` returns **one operand object, not the instruction's text**, and the displacement is
not in it. A search matching `"0x3c("` against that string found **0 of 29,408 functions** -- a
plausible-looking falsifier of this file's `0x3c` reading, produced by a string that never contains
a displacement. Searched against the full text, the same scan finds **2,120 of 29,408**, rendering as
`lwz r12,0x3c(r10)`. **The `0x3c` reading was never falsified**, and the retraction was about to be
written from an instrument that could not have found a match whatever the image held.

### The guest-memory dump had been reading the host's

The fault harness printed the stand-in's block with `x/8wx 0x00e05880` and the vtable slot with
`x/1wx 0x10004f54`. gdb has no view of the guest: those are host addresses, and the answers are
visible in the output. `0x00e05880` came back as four words gdb attributed to
`_ZN7glslang16TOutputTraverser10visitUnaryE+3872` **inside the wiiuport binary** -- the stand-in's
block was being read out of the emulator's own text segment -- and the vtable slot and the frame both
answered `Cannot access memory at address`. Every "the arena holds" claim from a run before this was a
host read. `memory_base` is cemu's global (`MMU.h:19`, `MMU.cpp:8`) and `memory_base + guest` is a
host address, so `scratch/frame-loop/arena.py` now reads guest memory that way, prints the mapping it
read through, and **checks the frame's first word against the image's `0x7c0802a6` before treating
any of its output as evidence**.

Two more defects in the same instrument, both found by its own output rather than by reading it:

- The guest-CPU sampler read the CPU state from `%rsp`, which is right only in recompiled code
  (`REG_RESV_HCPU` is `X86_REG_RSP`); in the interpreter it is the first argument. On a thread in the
  interpreter it returned the host stack pointer, and the "guest pc" that came out was a host return
  address with the top bit set. It now names the register it used and whether that was the interpreter
  or the recompiler.
- That sampler also ended by detaching, which killed the inferior before the guest dump that runs after
  it: `memory_base` read back as `0x0` and the dump read host addresses again -- the same defect, once
  removed. And the runtime addresses were read *after* the two holding windows, by which time the fault
  had happened and the channel was refusing connections, so the one run that mattered recorded only
  that it could not be read. They are read at arming time now, while the product still answers.

**This is where the double-paint fault stands, as known rather than as a theory.** The guest is
healthy at the fault; the fault is a host segfault reached through `PPCInterpreterSlim_executeInstruction`
-- the recompiler's *fallback* -- on the display fiber. **The running guest is the disc image**, proven
by the product's own accessor: eight of the frame's words read identically and the ninth is the mod's
own probe, and the byte order that made it look otherwise is now fixed in the reader. The faulting
opcode, decoded with that order right, is `0xe2060380` (`lwarx r16, r6, r0`) and is **not in the
title's RPX**: 0 matches over 9,432,460 executable bytes against a control found 4,893 times. The
frame's own field usage is measured and the mod's offsets are confirmed against it. Five payload
shapes have each been measured -- four fault, and the objective's own neither faults nor paints.

One thing is open, and it is now specific: **which branch of the second paint lands in the loader
arena's data.** The arena's base holds the HLE registry's code and symbol names and then zero padding,
the fault's program counter is in that range on every run, and the recompiler is registered for the
whole 2 MiB because the loader does put real code there -- so a branch into the registry is translated
as instructions. The display object's being cleared is a consequence of the frame never having been
entered, not a cause. The candidates are the stand-in's own control flow after the frame returns, or
one of the frame's five `bctrl`s; the five targets come from `display+0x24`, measured identical across
paints, so the targets are not the divergence. This is a question about the mod's own code, not about
the title.

### A real bug found on the way, which is not this fault

`PPCRecompilerX86_allocateExecutableMemory` (`BackendX64.cpp:1313`) bounds-checked with
`codeMemoryBlockIndex + size > codeMemoryBlockSize` and then wrote up to three bytes of `0x90`
padding **past** that -- so a translation landing flush against the end of its block wrote at
`codeMemoryBlock[codeMemoryBlockSize]`, one past the allocation, into a page with no permissions.
Fixed by rounding the size up before the check (`(size + 3) & ~3`).

**It does not fix the double-paint fault, and that is measured**: mode 9 armed over a run that had
reached 1,845 paints at rest still takes the product down with it. The overflow is three bytes past a
block; the faulting access is 0x198 into a page that was never part of that block. So the fix is
kept because it is a genuine out-of-bounds write, and the fault is *not* claimed as fixed.

What is left, and it is narrower than it was: the recompiler's generated code for the arena
stand-in is holding a base register that points at released memory. Either a code-cache block is
released while a core is still executing inside it, or the base is computed from a block that was
never committed. Those need opposite fixes -- a pin or reference count against release, versus a
block that must not be published unbacked -- and the run that separates them is AddressSanitizer over
the same capture workload, which will name the function and the offset instead of leaving a base
register to be interpreted.

So the two candidates this section weighed -- the payload clobbering `r3`, and the frame not being
re-entrant -- are **both refuted by the guest state**, and the fault is in the emulator's handling
of arena-resident code: the stand-in lives in the loader's trampoline arena, and the recompiler's
block bookkeeping for that region is computing an index the table cannot hold. It is the same
family as the fork's `23eb318`, which moved the mask publication *after* the mapping it promises;
this is the block index itself, and it is out of range rather than early.

**That redirects the work.** Four payloads were built to test shapes, and the shape is no longer
the question: every payload that actually paints twice faults, and it faults in the emulator's code
cache with a healthy guest. Mode 6 and mode 8 survive precisely because they never reach a second
pass through the stand-in. So the work is in `external/cemu`, not in a payload -- and the first
candidate there is now cleared, which is worth as much as a suspect confirmed.

**The earlier reading of this fault is withdrawn.** It was recorded as "the display thread's PC is
outside the title's code, `0x02c12ffc` and `0x00e000b28` in two runs" and as "what remains
unestablished is why that leaves the program counter outside the title's code". The program counter
is not outside the title's code; it is in the arena, which is where the stand-in is, and the frame
that read it as a lost guest was reading a host fault address as a guest one.

**A note on how the repair word was found, because the wrong turns are the useful part.** Three
attempts to compute the `or`/`mr` encoding by hand each found *nothing* in nine megabytes of
PowerPC. `mr` is everywhere and the image holds 141,339 `or` instructions, so an encoding matching
nothing is an encoding that is wrong. The two bugs were the extended opcode's field position and
then the register fields' offsets, and what caught the second was not reasoning but the shape of
the answer. The word came from the disassembler's own listing, with its address printed beside it,
and two controls -- the frame's first word and the swap-interval call -- decide whether the scan is
reading this title at all. One of those controls was itself wrong at first: the payload's leading
word `0x819f0024` is not the frame's first instruction, and treating it as one is how a control ends
up checking a claim instead of the file.

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

### The offset, looked for in the assembled buffer, and not there

`title::ObjectPoseLocator` walks every 4-aligned offset of every assembled uniform buffer the
fork hands over, tests each for a rigid 3x4, and keeps the counts. It is an
`AssemblyRecordedListener`, so it sees exactly the draws the blends see. Measured, with the
paint mod at one vblank a flip and the logic gate holding the tick at thirty:

    window 0:  614,690 assemblies,  2 candidate offsets,  0 believed at 20%,  best offset null
               371,528 of them had no block sources at all
    window 1:  756,350 assemblies,  2 candidate offsets,  0 believed at 20%,  best offset null
               456,968 of them had no block sources at all

**No offset cleared the bar, and the two candidates are coincidences** — a colour triple near
unit length, or three equal rows, which the perpendicularity test rejects and the tolerance
lets through only just. And **three fifths of the draws source no uniform blocks at all**, so
the draws that do read uniforms are a minority and none of them carries a transform that
recurs.

That is the second place the pose is not, and it is consistent with the first. The title
positions geometry on the CPU each frame — the shipped mechanism's own evidence says so:
"geometry the title positions on the CPU each frame, skinned characters and effects", which
is why it had to keep vertex bytes and blend them. **So the pose is in the vertex data, not in
a uniform**, and the transform that puts it there is the node's own, applied before the
display list is built.

**Which is what the objective asked, read literally.** "Find which **node field** holds the
pose" — a node field, not a block. This document went looking in uniform blocks because the
binder names one and because the block census was the instrument already built, and two
measurements in a row say the block is the wrong place: 233 whole-block scans of a 64-byte
block with no transform, and 756,350 assembled buffers with none either. The node's own
transform is the thing the title applies to its vertices, and lerping it and letting the
game's own draw run is exactly "skinning, attributes and display lists regenerated by the
game's own draw at the lerped pose" — no host-side vertex work at all.

### The node field, and a third place the pose is not

The probe is at the function's **entry**, `0x02160018`, and not at the address the vtable
holds: `0x10036300` slot `+0xc` is `0x02160180`, which is 0x168 bytes *into* the function, and
its first instruction is a conditional branch (`0x41820010`, `beq 0x02160190`) — a probe resumes
at "the instruction after the entry", so a taken branch there would make the stub re-run the
code the branch was there to skip, and the fork refuses such a site outright. The entry's own
first word is `stwu r1,-0x148(r1)`, `0x9421FEB8`, which does not branch, so the entry is the
safe site, and the node is in `r3` there — the prologue copies it to `r28` and the frame saves
r24-r31, so `r28` is the node for the rest of the function.

**And measured, the entry is never called.** The probe installs and takes zero calls over two
eight-second windows: the title dispatches the node's draw only through the vtable's target.
The table at `0x10010648`, which also holds the entry, is not the one in use.

So the node is fed the way the draw itself reaches it: the draw calls its own sub-object at
`node + 0xa1c` (`addi r3,r28,0xa1c` immediately before the call into it), and the binder probe on
that sub-object — already installed, already firing 137,489 times in a run — holds the node one
fixed subtraction away. That is the route in production, and it is not a convenience: the draw's
only other entry is not a probe site at all.

Fed that way, on the real title, with the paint mod at one vblank a flip:

    window 0:  8 nodes tracked, 166,635 refused, 0 unreadable, 1024 words per node,
               4 scans per node, and no offset at all
    window 1:  8 nodes tracked, 190,719 refused, 0 unreadable, same

**No node held a rigid transform at any 4-aligned offset in its first 1024 words.** So the pose
is not in the node's own memory either, and that is three places now: the binder's 64-byte
block, the assembled uniform buffers, and the node. Each was tested with the same shape test and
the same counted bar, and each is reported with its denominators.

What that leaves, and it is a short list rather than a wide one. The draw walks the
**sub-object** — the binder's argument, at `node + 0xa1c`, with the descriptor at
`subobject + 0x10` and the cursor at `subobject + 0x4c`. That is the object the descriptor
belongs to, so it is the next thing to scan, and the binder hands it over with no subtraction
at all. And if a node's own transform is not a rigid 3x4 it may be because it carries scale, or
because the transform a renderer multiplies is the **world** matrix, the product of the node's
place in the scene graph with its parents' — which is a thing a local transform is not and
which a scene keeps on the node that has no parent. Both are bounded reads with the same
instrument.

### Two things the first scan got right by accident, and how they were found

The locator is fed from the binder, which names **two** addresses: the node's sub-object, and
the node one `kSubObjectOffset` away. They are scored in separate tables, because one table
over both would let a field at the same offset in a node and in its sub-object count twice
towards the bar. The first run of the two tables produced a *positive*, and it was wrong twice
over. Both defects are in the instrument, not the title, and both are the kind a green test
cannot see.

**"Moved" was a bitwise test, not a motion test.** The report said `moved 18` beside `biggest
delta 0.000000`. The values differed in the last mantissa bit and in no way a pose moves, and
`%.6f` printed the difference as zero -- so the report claimed movement while showing no
magnitude at all. Three offsets in the node's table and one in the sub-object's were "held by
6 of 8 objects" on that basis: node 320, 1340, 2360, 3380 and sub-object 792, each 24 scans,
18 comparisons, 18 "moved", 0.000000. A static basis matrix, a normal, a colour basis -- the
same shape as a pose, at the same offset in every object, for ever.

The bar is now stated rather than tuned: a candidate must change by more than
`kMotionEpsilon = 1e-3` between two draws of the same object, which are a frame apart, and an
offset is named only if it both crosses the cross-object bar *and* has been seen to move. The
report carries `moved`, `still`, `moving` and `biggestDelta` at nine significant digits, and
`motionEpsilon` itself, because it is the one number here a reader could reasonably want to
argue with.

**The two windows overlapped, so the "separate" tables were one measurement counted twice.**
`kSubObjectOffset` is 0xa1c — 2588 bytes — and the scan window was 4096 bytes, so a node's
window reached 1508 bytes into its own sub-object. The run said so itself: the sub-object's
`+80` was reported in the node's table at `+2668`, which is 2588 + 80. That is the exact
failure the separate tables were built to prevent, reintroduced through the addressing.

The node is now read over its own leading fields only, `kNodeScanWords = kSubObjectOffset / 4`
= 647 words = 2588 bytes, ending exactly where the sub-object begins, and a `static_assert`
holds that line. Each table reports its own `scanBytes`, so the two being apart is visible
rather than asserted in prose.

### The samples were taken per binding, which cannot see a pose move

With the movement bar in place the next run returned, for every candidate, `0 moved`, `18
still`, `biggest delta 0`. That is what a static field looks like. It is also what a locator
that samples an object once per *binding* looks like, and an object is bound several times per
frame -- so all four of its samples can land inside one frame, microseconds apart, where no
pose has moved by a thousandth of a unit. The report could not tell the two apart, and a
negative that cannot distinguish "nothing moved" from "I never looked twice" is not a
negative.

The sample schedule is now explicit: **one sample per object per frame**, taken from the
title's own paint counter, which `WindWakerPaint::paintCounter()` exposes for the purpose --
a count is no use to anything that has to schedule its own reads. The report carries
`schedule` (`perFrame` or `perBind`) and `frameCounter`, so a run says which schedule ran. With
no counter wired the locator samples per binding and says `perBind`, because that schedule's
negatives are worth less than they look and a reader has to be able to see that.

A third defect, small and of the same family: `frameCounter` was written as a raw value, so
the report carried `0x00000001` unquoted, which is not JSON, and the whole body would have
failed to parse in the one client that reads it. The test that caught it was asserting on the
field, which is the only reason anything looked at the encoding.

### With the schedule per frame, the node's transforms are static -- and that is now a fact

Rerun with the schedule fixed: `schedule perFrame at frame counter 0x00000631`, four samples
per object, each a frame apart, 8 objects of each kind, 124,360 refused.

    node:       offset 320  in 5 of 8, 20 scans, 15 comparisons, 0 moved, 15 still, delta 0
                offset 1340 in 6 of 8, 24 scans, 18 comparisons, 0 moved, 18 still, delta 0
                offset 2360 in 6 of 8, 24 scans, 18 comparisons, 0 moved, 18 still, delta 0
    subObject:  offset 792  in 6 of 8, 24 scans, 18 comparisons, 0 moved, 18 still, delta 0

**Not one of them moves, and the samples are a frame apart.** Those are bind poses, rest
poses, basis or normal tables: rigid, at the same offset in most objects, and the same value
for ever. That is a different thing from a pose, and the report now says so in words rather
than leaving `0 moved` to be read as a failure to look.

### The loose class, which is the one that chooses between two answers

A rigid test cannot tell "this node has no transform here" from "this node's transform
carries scale", and those point at different places. If the field is here and scaled, the
parent chain is needed only to *compose* with it. If the field is absent, the transform a
renderer multiplies -- the world matrix, the product of the node's place in the graph with
its parents' -- has to be read off the parent. A rigid-only bar reports nothing for both, so
"nothing found" would not have chosen.

So a second class counts any **non-singular** 3x3, with a floor: `kDeterminantFloor = 1e-6`,
because without one a plane of near-zero numbers has a determinant near zero and reads as a
matrix, and the loose bar finds one at every offset -- a bar that cannot fail. The row
lengths are reported as a deviation from unit, so a transform with rows 2.5 long is visible
as `scale 1.5` rather than being silently excluded.

The report carries both tables per kind, `rigid` and `affine`, and which of them fired *is*
the answer to where to look next. One defect caught on the way: the scale was a deviation
already and the report subtracted one from it again, so a scale of 2.5 came out as 0.5.

A property worth knowing before reading the tables: consecutive offsets overlap. A 12-word
transform at byte 80 is also the tail of a window at 84 and 92, so a real field shows up as a
cluster of nearby offsets, and the bar takes the one held by the most objects.

### The loose bar over 836,990 assembled uniform buffers, and what the run said about itself

The same two classes, the same movement bar, over what the title assembles for a named node's
draw:

    836,990 assemblies scanned, 95 candidate offsets in either class, 398,118 with no block
    sources, 0 past the bound
    rigid bar:   4 offsets ever in this class, 0 held often enough, 0 seen to move
    affine bar: 95 offsets ever, 23 held often enough, 18 of those seen to move, best offset 36
      offset 36: 262,978 assemblies, 63 repeat comparisons, 1 moved, 62 still,
                 delta 1.2913326, rows off unit by 364193.062, 1 identity

**The strict bar's negative stands and is now the better-measured one**: four offsets were ever
a rigid 3x4 across 836,990 assemblies, none in 20% of them, none seen to move.

**The loose bar's positive is not believable, and the report says why in its own numbers.**
Three things in that line are disqualifying, and each is now a bar or a field:

- **`rows off unit by 364193`** is not a transform with scale, it is a projection constant.
  The loose class had a determinant *floor* and no ceiling, so any 3x3 of coordinate-like
  floats counted. There is now a ceiling -- `kScaleCeiling = 100`, generous on purpose, because
  the point is to exclude the numbers that are arithmetic rather than to find a transform at one
  scale -- and `classify()` returns *which* refusal it is: `Singular`, `TooLarge`, `NotFinite`.
  "Rows off unit by inf" is no longer producible.
- **`1 identity` and `63 repeat comparisons` against `262,978 assemblies`.** The whole locator
  rests on "did this value change between two draws of one object", and that comparison
  happened 63 times. `1 moved` out of 63 is not evidence about a quarter of a million
  assemblies. The report now carries `identitiesSeen` and `identitiesRefusedForTracking` at the
  top so the denominator cannot be read past.
- **The cause is the identity, and it contradicts the code's own comment.**
  `RecordedUniformAssembly::blockSources` is documented as "the engine's own storage for the
  object, and the only identity a recorded draw carries". Measured: it matched exactly **one**
  identity across the 438,872 assemblies that had sources. The uniform block is re-uploaded at
  a new guest address each frame, so the set of addresses is nearly unique per draw and the same
  object's assemblies never meet. The comment is wrong and has been corrected in place.

The identity the objective names is the **node**, and the node is not in this record: the
binder sees it, at `node + 0xa1c`, and the assembly hook does not. Correlating the two is the
fix, and until it is done the movement counts in this report are counts over a handful of
comparisons -- so the uniform-buffer negative is the *strict* bar's, and the loose bar there is
not yet a measurement. The comment on `identityOf` now says so where the next reader will find
it.

### Identity is the node, and the address the fork's record called one is not

The identity the whole uniform-buffer question rests on was measured and did not hold.
`RecordedUniformAssembly::blockSources` -- "the guest addresses of the uniform blocks this
draw sourced" -- was documented in the fork's own header as "the engine's own storage for the
object, and the only identity a recorded draw carries". Over 836,990 assembled buffers it
matched exactly **one** identity across the 438,872 that had sources. The uniform block is
re-uploaded at a new guest address each frame, so the set of addresses is nearly unique per
draw and the same object's assemblies never meet. Every "did this value change between two
draws of one object" comparison in `ObjectPoseLocator` therefore happened 63 times, and one
movement in 63 is not a measurement of a quarter of a million assemblies. The comment is
corrected in place: the field is kept, because it is what the draw actually read, and is no
longer called an identity.

**The node is one step from both ends and in neither.** The GX2 hook cannot see it; the binder
can, because the draw calls its sub-object at `node + 0xa1c` and the probe there already fires
137,489 times a run. So `ObjectIdentityScope` is a single published slot: the census writes it
on the way into its scan, `RecordingObserver` reads it once per assembly, and
`RecordedUniformAssembly` gains `objectAddress`. The node is never passed between the two --
the binder and the GX2 hook are different call sites, and the slot is the whole of the join.

**A single slot is a limitation and its error rate is reported.** `bindsSinceLastQuery` is how
many writes the slot was standing for when it was read. One is the answer for a correct
correlation: the binding these assemblies belong to. Above one, something else bound in
between and the identity read is another object's -- which is worse than a missing one, so the
count is in the report and a run where it is large says so rather than being believed. The
census carries the scope's coverage and error rate in its own report, because a correlation
whose error rate is not beside its results is one that gets believed.

Two report defects, both in the field added to make the denominator visible. `identitiesSeen`
was `m_seen.size()`, which is one entry per *(identity, offset)* pair -- a field named for
identities carrying pairs, and precisely the number a reader divides the movement counts by.
It now counts distinct identities, with `trackedPairs` beside it. And the scope kept the
binds-since-last-read count and the answer in one field, so the report read the value *after*
the reset: always zero, which looks like a good result and is the absence of a measurement.
Two fields now, one in progress and one the answer.

Verified by mutation rather than by argument: keying the identity on the block source again
fails three checks. Ten assemblies of one object with ten *different* block addresses give nine
comparisons and nine movements under the node, and no comparisons at all under the address --
which is the whole of what the node buys, and the number the earlier run could not produce.

### The node identity works, and it makes the rigid negative well-powered

Same run, same bars, identity switched from the block address to the node:

    before:  source blockSources,  1 distinct identity,      63 repeat comparisons
    after:   source objectAddress, 8 distinct identities, 75,703 repeat comparisons

A thousandfold more comparison, from one join between the binder and the assembly hook. And the
strict bar's answer, which was previously resting on 63 comparisons, is now resting on tens of
thousands and does not move:

    rigid bar: 4 offsets ever in this class, 0 held often enough, 0 seen to move,
                over 809,682 assembled buffers and 397,534 with no block sources

**So: with a working identity, no rigid 3x4 is present in 20% of the title's assembled uniform
buffers and changes between draws of one object.** That is now a well-powered negative rather
than a 63-comparison one, and it is the strongest single result in the search.

The loose bar still names an offset, and still should not be believed: `rows off unit by 9`,
`3.9`, `12.5`, `81`. Those pass the ceiling of 100 and are still not poses. But the loose class
was added to answer one question -- *is the transform here at all, or is it absent* -- and it has
answered: **present, not rigid, and at no plausible scale.** It has done its work. It is not a
pose detector and should not be read as one, which is why the rigid bar is the one the question
"where is the pose" is answered from.

### What that means for the question the objective asks

The objective asks for "which node field holds the pose". Four places have now been read with a
working identity and a frame-apart schedule, and the answer is that **no field holds it**: the
node's own 2588 bytes, the sub-object's 4096, the binder's 64-byte block, and 809,682 assembled
uniform buffers all hold transforms, and every one of them is static. The only things that move
are the values at scales of 4 to 81, which are arithmetic and not transforms.

That is consistent with the title's own behaviour and with what the shipped mechanism had to do:
`VertexBlend` existed because the title positions geometry **on the CPU each frame** and hands
GX2 a display list of already-transformed vertices. There is no pose left in the object at draw
time, because the pose has already been consumed into vertex bytes. "The picture reaches sixty
without the logic following it" and "a blend cannot read N-1 out of the ring" are both true and
both consequences of the same fact.

So the blend's landing place is the vertex stream at the game's own draw, not a field to
lerp -- and the fork already has both halves of that: `LatteFrameHooks::UniformAssembly::data`
is writable at the last point before the buffer is uploaded, and
`LatteFrameHooks::Observer::OnDrawPrepared` hands over `VertexReplacements` at the draw. Identity
is the node, which the objective names and which the binder already publishes.

That is a change of mechanism, not a smaller version of the same one, and it is recorded here as
the finding it is: the question "which node field holds the pose" has an answer, and the answer
is that the pose is not held anywhere as a transform.

### The position attribute, named from the title's own tables

Because the pose is consumed into vertex bytes, the blend writes vertex bytes, and the one
thing that has to be *known* is which attribute carries the position. Reading a semantic index
out of a GX2 header would be a guess about a title nobody has disassembled, so it is measured
instead -- how often each `(semantic, format, size, buffer, offset)` signature recurs across the
title's own objects, with the bar over *distinct objects* so one odd mesh cannot name it.

    502,922 guest draws, 916,081 attributes read, 7 objects tracked (386,949 refused),
    0 draws with no attributes, 1,994 with no node published, 0 naming a buffer the draw lacks
    a position is named when 4 of the tracked objects agree and it is 12 or 16 bytes

    position: semantic 0, format 0x00000030, 12 bytes, buffer 0, offset 0, per instance 0

    semantic 0: format 0x30, 12 bytes, buffer 0, offset  0, in 7 objects and  71,165 draws
    semantic 0: format 0x30, 12 bytes, buffer 0, offset 16, in 4 objects and  39,689 draws
    semantic 6: format 0x1e,  8 bytes, buffer 0, offset 12, in 6 objects and  27,520 draws
    semantic 1: format 0x1e,  8 bytes, buffer 0, offset 24, in 7 objects and   1,855 draws

**And the near-misses are in the report, which is the point of the histogram.** There is a
*second* twelve-byte format-`0x30` attribute at offset 16, in 4 of 7 objects and 39,689 draws
-- it clears the bar of 4 on its own. It is not named because the one at offset 0 is in 7 of 7,
and a reader can see both. A bar that filtered the immovable and the runner-up out of the
report would have shown one line and called it a finding.

**What the format byte is, is not guessed.** `0x30` is what the title pairs with a
twelve-byte, three-component position in 7 objects and 71,165 draws, and `0x1e` is what it
pairs with eight-byte ones -- that much is the measurement. What `0x30` is *called* in Latte's
enumeration is a lookup, and the report carries the byte in hex and in decimal precisely so a
reader does not have to take this one's word for it.

**The sampling limit, stated rather than buried.** Seven objects of the first eight the binder
published, and 386,949 refused. Those seven may well be seven instances of one kind of thing --
the sea, the sky, a particle system -- and every one of them agreeing is a weaker claim than
seven *different* objects agreeing. The census reports the refusals beside the belief for
exactly this reason: "7 of 7" and "7 of the first 7" are not the same statement, and only one of
them is what the number says.

### The position is per vertex layout, and a single global one was wrong

The falsifier's first honest run said 2 of 8 nodes blendable, 5 identical, and the 2 blendable
ones carried `magnitude believable=false` with 18 and 60 components out of range. The 5 identical
nodes were at **stride 32** and compared cleanly -- 585, 975, 1235, 845 and 4 vertices, zero
differing bytes, every magnitude believable. The 2 were at strides of **20 and 64**.

**So the position is per layout, and my census was naming one offset for a title with several.**
The attribute the census named sits at offset 0 *of its own layout*, and a title packs positions
differently per vertex layout. The cause was in the census's own key: `Signature` recorded a
stride and the comparison key **ignored it**, so a stride-32 draw and a stride-20 draw whose
attribute fields agreed folded into one signature and the majority counted both. That is how one
global position came to be reported, and it is why 18 and 60 components came out as
unreadable floats.

The stride is in the key now, `positionFor(stride)` asks about one layout, and the report lists
**every layout** with its own position and its own denominator -- because the number of layouts is
precisely what the one global answer was hiding. `VertexPoseHistory` asks per draw, over the
draw's own buffers, and takes no stride on faith: the stride that identifies the layout is the
stride of the buffer the position is *in*, which is not known until the census answers.

And with the magnitude ceiling in place, a position that moves believably is now separable from
one whose bytes are not a position: node 1046041348 reported 40 differing bytes with a largest
component delta of **0.488** -- a plausible half-unit of travel -- alongside 18 components that
were out of range. That is a real movement with a real layout problem beside it, and before the
ceiling both were reported as the single number 1.06e+38.

### A verification lesson, because three of my mutation checks were vacuous

Three times I "verified" that removing the stride from the key broke nothing, and reported the
test as not having teeth. **The mutation had never been applied.** clang-format aligns that
return across two lines, so my replacement string -- written with single spaces -- matched
nothing, the build succeeded, and the suite passed on unmodified code. A mutation check that
reports "not caught" is ambiguous between *the test is weak* and *the mutation did not land*, and
I read it as the first three times.

Done with a regular expression, which reports its substitution count: the mutation applies, and
**four checks fail**. The assertion that catches it is on the histogram and counts two entries at
the same offset with different strides -- because the `layouts` block lists both strides either
way and so cannot tell whether the key separated them.

### The bitwise fault, again, one level down -- and a verdict beside another shape's numbers

With the per-layout position in place the falsifier read 6 blendable, 1 identical, 1 unpaired.
Two of those lines were wrong, and the report is what showed it:

    node 1166856616: blendable, 1040 vertices, 12480 bytes, stride 32,
                    differing bytes 13780, biggest component delta 1.18866922e-07

**13,780 differing bytes out of a total of 12,480.** That is impossible for one pair of samples,
so the two numbers came from different shapes: the verdict was about a shape that was not the one
whose geometry was printed beside it. The node's `vertices`, `positionBytes` and `stride` came
from whichever sample happened to be last, while the verdict came from the best-matched pair. The
geometry now travels with the verdict.

**And 13,780 bytes differing beside a largest component delta of 1.19e-07 is not a pose.** That
is a difference in the low mantissa bits -- the *same* bitwise fault the node scan had, now at
vertex level, and `differingBytes > 0` was deciding "blendable". So a third answer exists and is
named: `valueUnchanged`, for bytes that really did differ while the position did not move. It is
kept apart from `identical`, because the bytes did differ and a reader is entitled to that fact,
and apart from `blendable`, because the position did not move. The threshold is stated at
1e-4 -- generous, because the point is to separate "the bytes changed" from "the position
changed" and not to bound a scene.

**And the same "zero for not reported", for the fourth time.** The differing-byte count was
written only on the blendable path, so a `valueUnchanged` node reported `differingBytes: 0` --
about a comparison that had happened and found four differing bytes. The counts are now written
for every paired shape whatever the verdict.

A fixture detail worth recording, because it measured the opposite case: a nudge of 1e-8 on a
value of 1.0 rounds straight back to 1.0 -- the float epsilon there is 1.19e-7 -- so the test was
comparing identical bytes and reporting `identical` for what should have been `valueUnchanged`.
The smallest nudge that changes the bits at all is 1e-7, and the test says so.

### The falsifier's answer, and the layouts that explain the earlier nonsense

    493,839 draws seen, 194,999 with no position named, 8 nodes tracked (224,099 refused)
    of those 8:  1 blendable, 6 identical (compared, byte for byte),
                 1 whose bytes differ but whose values did not move, 0 with nothing paired

    node 1046050528: identical,     24 vertices, stride 64,  0 differing bytes, delta 0
    node 1046048488: identical,      4 vertices, stride 32,  0 differing bytes, delta 0
    node 1046042368: identical,      4 vertices, stride 32,  0 differing bytes, delta 0, 124 frames apart
    node 1046045428: identical,      3 vertices, stride 32,  0 differing bytes, delta 0
    node 1046046448: valueUnchanged, 10 vertices, stride 32, 30 differing bytes, delta 2.79e-19
    node 1046041348: blendable,       4 vertices, stride 20, 36 differing bytes, delta 15.38,
                                      magnitude believable=false, 20 components out of range

**So: of eight tracked objects, none has a believable moving position.** Six are byte-for-byte
identical across 4 to 124 frames, one differs in 30 bytes with a largest component delta of
2.8e-19 -- the mantissa-bit case, caught and named rather than called a pose -- and the single
`blendable` is not believable either, with 20 of its components out of range. **The vertex-stream
blend has no ingredient here**, and the reason is specific rather than a shrug: the title's
positions either do not change, or change in ways that are not positions.

And the layouts explain every unreadable magnitude this session produced:

    layout stride 20: 7 objects, semantic 0, 12 bytes, buffer 0, offset  0
    layout stride 28: 1 object,  positionKnown=false
    layout stride 32: 7 objects, semantic 0, 12 bytes, buffer 0, offset  0
    layout stride 48: 3 objects, semantic 0, 12 bytes, buffer 0, offset 16
    layout stride 64: 7 objects, semantic 0, 12 bytes, buffer 0, offset 16

**The position is at offset 0 in two layouts and offset 16 in two others.** A single global
offset -- which is what the census named before the stride went into its key -- is right for
half the title's layouts and nonsense for the rest, and reading a 64-byte stride's offset 0 as a
position is where every 1e+38 came from. That is the whole of the earlier nonsense, accounted
for.

**The limit on the negative, stated.** These are eight objects of the 224,107 the binder
published, and they are the *first* eight. A sea, a sky and a particle system are all plausible
for a wind game's opening frame, and "6 of 8 identical" is a statement about those eight and not
about the title. The next measurement is a *strided* sample -- objects spread across the whole
run rather than the first few -- because a negative about eight objects of one kind is not a
negative about the game.

### Spread across the run, the answer changes: three blendable, all at one stride

With the strided sample the eight tracked objects sit at frames 109, 132, 159, 180, 204, 225,
253 and 293 -- spread across the whole run rather than packed into its first seconds -- and the
verdicts change with them:

    500,896 draws seen, 8 nodes tracked, 47 refused by the set bound
    3 blendable, 1 identical, 1 valueUnchanged, 3 with nothing paired

    node 1166878616: blendable,  4 vertices, stride 20, 38 differing bytes,
                     biggest component delta 0.106766738, 28 components out of range
    node 1216241436: blendable,  4 vertices, stride 20, 42 differing bytes, delta 209089
    node 1160909272: blendable,  4 vertices, stride 20, 40 differing bytes, delta 883540
    node 1046048488: valueUnchanged, 10 vertices, stride 152, 80 differing bytes, delta 9.1e-07
    node 1160952552: identical,   3 vertices, stride 32, 0 differing bytes, delta 0

**All three blendable are at stride 20, and one of them has a believable magnitude.** A largest
component delta of **0.107** is a tenth of a unit of travel, which is what a position does. So
there is something real here, and the reason the earlier sample found none is exactly what the
stride was for: a sea and a sky do not move and a particle system is not a mesh.

**And the stride-20 layout is the one whose position the census got wrong.** 4 vertices at a
20-byte stride with a 12-byte position at offset 0 leaves 8 bytes of something else in the
stride, and those are being read as floats -- which is the out-of-range count. But a *believable*
0.107 on the same 12 bytes is impossible if all three components were wrong, so the 12 bytes at
offset 0 are partly position and partly not.

**Which means the per-layout bar is still too weak.** It names a position by how many objects
agree on the *signature* -- semantic, format, size, buffer, offset -- and for stride 20 seven
objects agreed on offset 0, which is enough to clear a bar over counts and not enough to
establish that the twelve bytes there are three floats of plausible magnitude. The next bar is a
magnitude condition, not a count: a per-layout position whose components are implausible is not
a position, and a layout that fails it is a layout the census has not solved rather than a layout
with a position at offset 0.

### Withdrawn: the believable 0.107 was the census reading a non-position

The previous entry reported a largest component delta of **0.106766738** on a stride-20 layout
and called it "a tenth of a unit of travel, which is what a position does", and said there was
something real. **That was wrong, and the magnitude bar is what showed it.** Those twelve bytes
at offset 0 are not a position; one component in them read as 3e+38, which is what the
out-of-range count had been saying all along. A delta of 0.107 beside a delta of 1e+38 in the
same twelve bytes was never a position, and reading it as one was the same class of mistake as
every other one this session: a number that looks like a result.

The census now reads the values at a candidate offset and will not name a position whose
components have ever read as something a position is not. The layouts it now reports:

    stride 20: 7 objects, positionKnown=false
    stride 28: 1 object,  positionKnown=false      (one object cannot clear a cross-object bar)
    stride 32: 7 objects, semantic 1,  12 bytes, offset 12
    stride 48: 3 objects, semantic 14, 16 bytes, offset 0
    stride 64: 7 objects, positionKnown=false
    stride 80: 2 objects, positionKnown=false
    stride 96: 5 objects, semantic 4,  12 bytes, offset 48

**Seven layouts, and the position at a different offset in each** -- offset 12, offset 0, offset
48. The stride-32 answer moved from `semantic 0, offset 0` to `semantic 1, offset 12` and the
stride-64 layout now names nothing at all. So the answer the census had been giving for two of
the four layouts it claimed was a position at offset 0 or 16 was **wrong**, and only the count
bar had let it through.

And the falsifier, on the corrected positions:

    488,712 draws seen, 403,774 with no position named, 4 nodes tracked
    0 blendable, 3 identical, 0 valueUnchanged, 1 with nothing paired

    node 1046041348: identical, 4 vertices, stride 32, 0 differing bytes, delta 0, 101 frames apart
    node 1160971032: identical, 4 vertices, stride 32, 0 differing bytes, delta 0
    node 1160969272: identical, 4 vertices, stride 32, 0 differing bytes, delta 0

**Zero blendable.** `drawsWithoutPosition` rose from 188,522 to 403,774 of 488,712 draws, because
most draws are now correctly refused: their layout has no position this census is willing to
name. **The vertex-stream blend has no ingredient on these objects, and the one time it appeared
to have one, the census was reading bytes that are not positions.**

Four of the seven layouts are unresolved, and "unresolved" is the honest word: the magnitude bar
refuses them rather than naming a position and reading rubbish. A layout the census has not
solved is a different thing from a layout with no position, and the report says which --
`positionKnown: false` beside the layout, with the implausible-component count in the
histogram.

### Every layout is refused, and that is the search's answer

With the denormal floor's non-zero clause in place -- so a vertex at the origin is a position
and a 1.7e-38 denormal is not -- the census now refuses **all eight** of the title's vertex
layouts:

    stride 20:  6 objects, 0 candidates clearing
    stride 28:  1 object,  0 candidates clearing
    stride 32:  7 objects, 0 candidates clearing
    stride 48:  4 objects, 0 candidates clearing
    stride 64:  5 objects, 0 candidates clearing
    stride 80:  1 object,  0 candidates clearing
    stride 96:  2 objects, 0 candidates clearing
    stride 152: 4 objects, 0 candidates clearing

    semantic 0, format 0x30, 12 bytes, buffer 0, offset 0, stride 32:
    in 7 objects and 15,976 draws, 136,381 components implausible (magnitudeBar=fail)

**So the position attribute cannot be identified, in any layout, by the title's own attribute
table.** Not "the position does not move" and not "the blend is hard": at every candidate offset,
roughly one vertex in ten reads as something that is not a position -- above 1e6, non-finite, or
a nonzero denormal -- and that share is over a 1% bar. `479,158 draws seen, 479,158 with no
position named, 0 nodes tracked`.

**And the falsifier, with no position to read, has nothing to compare.** 0 blendable, 0 identical,
0 valueUnchanged, 0 with nothing paired -- not because nothing moves, but because no draw had a
position the census was willing to name. A falsifier that cannot run is not a negative result, and
this one is not one either: it is the absence of a measurement, stated as such.

### What this means for the objective, plainly

The objective asks for "which node field holds the pose" and then for a blend "at the game's own
draw, with identity being the node, and skinning, attributes and display lists regenerated by the
game's own draw at the lerped pose". Measured, on the real title, with every instrument corrected
against its own false positives:

1. **No node field holds the pose.** The node's leading 2,588 bytes, the sub-object's 4,096 at
   `node + 0xa1c`, the binder's 64-byte block, and 838,155 assembled uniform buffers were all read
   with a working identity and frame-apart samples. Every transform in every one is static. The
   only moving values sit at row scales of 4 to 81, which are projection constants.
2. **The title consumes the pose into vertex bytes**, which is why: it positions geometry on the
   CPU each frame and hands GX2 a display list of already-transformed vertices. That is also why
   the mechanism being retired needed `VertexBlend`.
3. **The position attribute cannot be found by the title's own attribute tables**, in any of
   eight layouts, because the candidate offsets are not positions for a substantial minority of
   vertices.

The remaining route is the one the objective names and the attribute table does not carry: **the
vertex shader's own input declaration.** `DrawPrepared` gives the fetch shader's attribute table,
whose `semanticId` is an index whose *meaning* lives in the shader's input declaration -- and
that declaration is in the guest's shader memory, not in the draw. Guessing that `semantic 0` means
position is exactly the kind of assumption every measurement in this log exists to refuse, so it
has not been made.

That is where the search stands, and it is a smaller and more honest place than where it started.

### The ring test, and what the title actually named

The objective's second question -- whether tick N-1's uniform block contents are still present
when tick N paints -- needs the block's *address*, and the answer so far is that the title has not
named where it lives.

`UniformBlockRing` is the measurement: at each binding the block's bytes are hashed, and when
the next binding of the same object arrives the earlier address is read again and compared with
the hash taken then. Three states, not two -- a first sample has nothing to compare, a comparison
that failed is not a "no", and a comparison that ran is the only thing a count of overwrites may
be built from.

**And it cannot run, because the descriptor entry's two words are not both usable.** Measured on
the real title: the word at `+0x0c` reads `0x40` for every object, and `0x40` is 64 bytes, which
agrees with 233 whole-block scans of a 64-byte block finding no rigid transform in one. So `+0x0c`
is the block's **size**. The word at `+0x04` is a **relative offset** -- the census's own
`blockOf` says so, and says that taking it for a length once "asked the product for a gigabyte
and the product died". The base the title set elsewhere has not been identified.

So the ring reports `sizesKnown`, `addressesKnown`, the sizes it saw, and
`addressState: relativeOffsetBaseNotIdentified` -- and claims no comparison. **That is the
objective's second question answered as far as it can be**: the block is 64 bytes, and a
relative offset is not a place to re-read.

### The two words in this file were named backwards, and I followed the header twice

`UniformBlockCensus.h` said "within an entry the block's size at +0x04 and offset at +0x0c", and
`blockOf` in the same project said the opposite in a comment. The constants matched the header:
`kEntrySizeOffset = 0x04`, `kEntryOffsetOffset = 0x0c`. Both stories were in one project and
they contradicted each other, and **the measurement settles it in the other direction from the
header**.

I followed the header twice, and both times the run said so exactly: first 177,317 bindings and
not one object tracked with nothing unreadable and nothing oversize, because the hand-off sat in
the guard *above* `readEntry` and read an entry that had not been read; then 186,133 bindings and
186,133 "past the bound of 4096", because the sizes being passed were the `0x1000`-and-up words
and the addresses were `0x40`.

The constants are now `kEntryBlockSize = 0x0c` and `kEntryBlockAddress = 0x04`, the
contradictory sentence is replaced by the measurement, and `blockOf`'s locals follow. A name that
disagrees with what it holds is not a naming problem; it is a trap with a comment on it, and this
one had a second trap in the same file pointing the other way.

A constant worth recording beside them: the hand-converted first word. The image's word is
`0x9421FEB8`; a value worked out from the signed decimal Ghidra prints gave `0x9422FEB8`, and
the fork's refusal — `entryHeldOther` — reads exactly like a real finding about the title. It
was this project's own rule that caught it: every payload word is lifted from the image, never
derived, because a derived one is a word nobody checked. The report now carries both the word
the entry held and the one the image has, so a refusal can say what it found. Then the substitution on the in-between present, keyed by the display list's
address. Then the discriminator, whose null case is **two paints of one pass** — the thing
condition 4 asks for and the thing two *consecutive presents* are not, since with the tick at
thirty consecutive presents are a tick apart.

### The base was a phantom, and the binder says so

Everything above was built on one word: that the descriptor entry's `+0x04` is a **relative
offset** and that a base the title set elsewhere had to be found before anything could be
re-read. The binder at `0x027ff88c` / `0x027ff9c0` was decompiled to find that base, and the
decompilation is what removed it:

```c
uVar6 = *(uint32 *)(iVar2 + 4);      /* 3rd argument */
uVar4 = *(uint32 *)(iVar2 + 0xc);   /* 2nd argument */
GX2SetVertexUniformBlock(iVar5, uVar4, uVar6);
```

and on the host side, `external/cemu/src/Cafe/OS/libs/gx2/GX2_shader_legacy.cpp`:

```c
void _GX2SubmitUniformBlock(uint32 registerBase, uint32 index, MPTR virtualAddress, uint32 size)
{
    gx2WriteGather_submit(..., registerBase + index * 7,
                          memory_virtualToPhysical(virtualAddress),
                          size - 1, ...);
}
```

**Nothing is added to either word.** One of them becomes `memory_virtualToPhysical(...)` in the
uniform block register and the other becomes `size - 1`. So the two words are the address and
the size, in one order or the other, and *there is no base to find*: the earlier arithmetic
measured the difference of a size and an address, which is a number with no meaning, and it
found none.

That measurement is deleted rather than left to rot. `UniformBlockBase` histogrammed
`address - offset` over 180,707 bindings and 218,850 candidates from 815,562 assemblies, and
reported `base: null` with the leader at a **2.0% share** against a 20% bar — correct, and for
the wrong reason twice over:

- **The corpus was a lottery.** Only **95,943 of 815,562** assemblies (11.8%) had a binding
  pending, because 815,562 assemblies arrive against 180,707 bindings, so most assemblies were
  paired with whatever record happened to be last. **84,764 of 180,707** bindings (47%) were
  overwritten before an assembly arrived. A wrong pairing lowers every candidate's count equally
  and a right one is invisible, so the histogram could not have found a base even if the base
  were real and the pairing exact.
- **The 4096-entry map was saturated**: 153,407 candidates were refused, so the leader's 2.0%
  was a share of a truncated set and the real base, if any, could have been among the refused.

The pairs are now by the **title's own object** rather than by adjacency: the assembly names the
object the draw is in the middle of (`ObjectIdentityScope::current()`) and the binder named the
same object, so the pair is exact and identity is the title's, read from its own binder.

### `mapWords` assumed the thing it was looking for, and found itself

`mapWords` read each word of the record **added to the word at `+0x04`**, on the theory that one
of them was a base. Each word is now read as an address on its own, with nothing added. Measured
over **199,280** bindings:

| word | reads | share |
|---|---|---|
| `+0x00` | 199,280 | 1.00 |
| `+0x04` | 199,280 | 1.00 |
| `+0x08` | 199,280 | 1.00 |
| `+0x0c` | **0** | 0.00 |
| `+0x10` | **0** | 0.00 |
| `+0x14` | 199,280 | 1.00 |
| `+0x18` | 199,280 | 1.00 |

**Five of the seven words read in every single binding**, so a "first word to clear a majority"
rule returns word 0 — the record's own leading pointer. That is what it returned, the ring was
wired to it, and the ring reported:

> is tick N-1's uniform block still there when tick N paints? **16 of 16 comparisons say still
> present** … 8 consecutive pairs used different addresses, which is double buffering measured

**That result is withdrawn.** The ring was re-reading the descriptor record and comparing it with
itself, which is the most agreement a measurement can produce and means nothing. A route that
cannot discriminate must say so rather than return the first index, so `addressWordByMapping`
now requires that **exactly one** word read, and reports
`addressWordRefused: severalWordsReadSoNoneIsDistinguished` with the candidate count when more
than one does. The ring gets a size and no address, and reports **0 of 0** comparisons — honest,
where a named word was not.

### `kEntries = 2` is wrong, and the other slot does not exist

`readEntry` for the second slot failed in **199,280 of 199,280** bindings: the entry at
`object + 0x10 + kEntrySize` is not mapped. So this binder's descriptor list has one entry it can
read, and a publication that waited on the second slot waited for memory that does not exist —
which is why the address measurement reported `0 bindings` for two full runs before the
`otherRecordsRead` / `otherRecordsUnread` counters were added to say so. **The earlier claim that
"the two slots of every object measured are exactly `0x100` apart" is withdrawn**: it came from
the same `mapWords` arithmetic, reading unmapped memory as though it were a difference.

### What is left, and it is a real answer about the draw

The draw's own block addresses are not in doubt: **1,555 distinct values** over 382,575 sourced
addresses, from the fork's `(bufferId, physicalAddress)` pairs. What is in doubt is which record
word names one of them. Over **142,682 exact object-keyed pairs**, **no word of the record
matched any of the draw's addresses** — the best word hit **3 times**, a share of 2.1e-05
against a 50% bar.

**And the records say why.** The report quotes four of them raw, and they are not what a
descriptor naming a block looks like:

```
record at 0x3e595304: [1046692688, 1046692864, 1046692864, 64, 64, 50397184, 269917568]
                    =  [0x3e5953d0, 0x3e595400, 0x3e595400, 0x40, 0x40, 0x03010000, 0x10163e00]
record at 0x3e597adc: [0x3e5957c0, 0x3e5957e0, 0x3e5957e0, 0x40, 0x40, 0x03010000, 0x10163e00]
```

- Words 0, 1 and 2 are **pointers `object + 0xCC`, `object + 0xEC`, `object + 0xEC`** — sibling
  structures, which is exactly why five of the seven words "read as guest memory": they point into
  the object's own mapped neighbourhood. They are not block addresses and never were.
- Words 3 and 4 are **both `0x40`**. So the binder hands `GX2Set*UniformBlock` the pair
  `(0x40, 0x40)` — and the fork writes `memory_virtualToPhysical(0x40) = 0x40` and `size - 1 =
  0x3f` into the uniform block register. **There is no block address in this record at all.**
- Word 6 is `0x10163e00`, 0x24c past `cWorldViewMatrix[0]` at `0x10163bb4` — the record points
  into the title's own global data, and not at a uniform block.

**So the 1,555 distinct addresses are not this binder's.** The fork reads
`contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + group.kcacheBankIdOffset / 4]` — word 0 of the
bank the *shader* names, not the bank the binder wrote. The binder writes `0x40` into the banks
it names; the values being read come from whatever else last wrote the slot the shader's group
points at. Which is also why `blockSources` matched **1** identity across 438,872 assemblies when
it was first measured, and why the comment in `LatteFrameHooks.h` calling it "the only identity a
recorded draw carries" is wrong: it carries the register file's contents, not the draw's.

**The base route is now a proper measurement rather than a truncated one.** The histogram of
`address - word` is a **space-saving sketch** of 65,536 slots, not a bounded map: a plain bounded
map keeps the first keys that arrive and evicts nothing, and with 1,441,075 candidates into 4,096
slots the whole tail was discarded, so a base that recurred in a large share of draws could have
been among what was thrown away. The sketch displaces the *least frequent* entry instead, and the
evictions are counted — its counts are upper bounds, which the report says and the tests assert as
bounds rather than as equalities.

### Condition 2's second question, answered: the block is a two-deep ring and it survives

The ring is fed the title's own two words from the record -- the address and the size the binder
passes to `GX2Set*UniformBlock` -- with no base, no offset and no host interpretation between.
Measured on the real title:

```
block ring: 178,021 bindings, 8 objects, 0 unreadable, 0 past the bound of 4096
where the block sits: 16 distinct block addresses over 8 objects, 16 distinct offsets
is tick N-1's uniform block still there when tick N paints?
  16 of 16 comparisons say still present, 0 say overwritten, 0 could not be read
  8 consecutive pairs used different addresses
```

**Yes.** Each of the 8 objects names **two** distinct block addresses and alternates between them
-- 16 addresses over 8 objects, and one transition per object, which is double buffering measured
rather than assumed. And the ring does not compare across an address change, so each of the 16
comparisons is between two bindings of the same object *at the same address*, which are at least
two ticks apart: **the previous use of that address is still there when the next tick binds.**
That is the question the objective asks, and the answer is yes, with the denominator stated: 16
comparisons over 8 objects.

**The scan against the real address is negative too, and now that is a fact about the block
rather than about where it was read from.** `ObjectPoseHistory` is handed the same address the
ring re-reads. It used to be handed the record's *size* word, `0x40`, as though it were an
address, so every one of its readings was 64 bytes of guest memory at `0x40` -- one location, over
and over. Measured at the address the binder names:

```
the bound block, read at the address the binder names:
  236,161 observations, 0 whose block looked like a pose, 236,161 that did not,
  0 unreadable, 0 without an object, 0 blocks refused
scans: 233 over 4 blocks, 12 pose words
offset hits: 0 -- none
```

So the block the descriptor names, read where the binder says it is, holds no twelve-float 3x4 at
any 4-aligned offset, over 236,161 observations and 233 whole-block scans. **That is a real
negative and it retires the third place to look.** The two earlier negatives were artifacts of the
address; this one is not, and it agrees with the other two -- the title positions geometry on the
CPU and hands GX2 vertex bytes, so there is no transform left in a uniform block to find.

**The ring's verdict is 15 to 16 of 16 across runs**, not 16 of 16: one run of identical code
reported one comparison overwritten out of sixteen, so the block's bytes do change sometimes and
the comparison is not trivially always-equal. Stated as a range because two samples is two samples.

Two things this does **not** say, because the earlier report had them backwards:

- **The 233 whole-block scans were not of this block.** They read `object + 0xFC`, which is the
  record's own word 1 used as an address. This block is elsewhere: the offsets from the object are
  large and all different, the leading one 0xA7C44. So "every transform in every one is static"
  was a statement about a record's neighbourhood and says nothing about *these* 64 bytes. The
  scan has to be redone against this address.
- **`consecutivePairsWithDifferentAddress` is not evidence on its own.** 8 objects at 8 different
  addresses also gives 8. That is why the report now carries `distinctBlockAddresses` and
  `distinctOffsetsFromObject` beside it: 16 addresses over 8 objects is two each, and one
  transition each is a ring, where 8 would have been 8 objects.

### The size word tells the guest's writes from register leftovers, and the pool is 0x100-strided

Word 0 of a uniform block register is whatever last held the slot — the guest indexes these
registers by the index it passes to `GX2Set*UniformBlock` and the shader names them by its own
group, so the two do not agree. **Word 1 is `size - 1` as the guest wrote it**, and a size the
guest chose is a small constant that register state does not invent. The fork now hands both
words over (`LatteFrameHooks::UniformAssembly::blockSizes`, cemu `cffee96`, pinned here by
revision), and a slot whose word 1 equals the record's size minus one is a block the title put
there.

Measured on the real title, with the record's size word as the filter and never a guessed one:

```
expected size 64 bytes, 3,990,665 size words read,
1,000,430 slots holding size-1, from 4,096 distinct addresses (574,837 refused)
  0x4581c200: seen 17,806, share 0.0225
  0x4581c300: seen 17,045, share 0.0215
  0x45436700: seen 14,939, share 0.0189
  0x45436300: seen 10,930, share 0.0138
  0x45978b00: seen 10,496, share 0.0133
  0x3e634300: seen  4,145, share 0.0052
```

**The pool is real and it is 0x100-strided**: `0x4581c200` → `0x4581c300` is 0x100,
`0x45436300` → `0x45436700` is 0x400, and so on. And `0x3e634300` is in the list — the very
value an earlier comment in this file dismissed as "a pointer, not a length". It was a block
address all along; what it was not was *reachable by the route being tried then*.

The histograms were truncated at 4,096 distinct values with 574,837 refused, which is a pool with
its middle missing — so the bound is 65,536 now, and the refusals stay in the report.

**And the join is nameable, from the binder's own arguments.** The decompilation says the block
*index* is not in the record either — it comes from `param_2`, read as
`*(short *)(iVar3 + 0xc)` where `iVar3 = *(int *)(param_2 + 0x10) + 0x28` — and **`param_2` is
`r4` at the probe**. So the title names its block by a *register index*, from a structure the
census is not currently reading, while the block's bytes live in the register slot that index
addresses. That is the exact join, and it is the title's own: r3 gives the object, r4 gives the
structure holding the vertex/pixel/geometry uniform block indices, and
`contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + index * 7]` gives the address for the one the
binder passed. No base, no offset, and no host-side matching.


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
