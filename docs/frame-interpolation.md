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
the tick on the title's own period, so a tick that arrives before the period has
elapsed returns without doing it. That is a change to the title's logic, in guest
memory, like everything else here -- and it is the first change in this mechanism
that is not about the picture.

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
