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

## Recording

Between two swaps, record every `IT_INDIRECT_BUFFER` the frame references as
`(physical address, dword count, copy of the contents)`. The copy is not an
optimisation to remove later: the guest reuses and overwrites that storage as soon as
the frame is submitted, so a reference alone would replay whatever the next frame wrote.

Alongside it, record the assembled uniform buffer of every draw, keyed by the draw's
position in the frame. Those recordings are what a consumer's blend reads: tick N-1's
assembled values and tick N's, for the same draw.

## Presenting

At the swap for tick N, the runtime holds tick N's recorded stream and tick N-1's
extracted transform state. Order per tick:

1. Copy the finished image of tick N aside.
2. Replay tick N's recorded stream. Each draw re-enters the uniform assembly site,
   where the consumer's blend of ticks N-1 and N is substituted into the assembled
   buffer. Present the result — this is the in-between frame.
3. Present the copy of tick N.

This costs one extra scene render per tick and adds one presented frame of latency.
Both are real and are stated rather than hidden; a title that cannot afford the extra
render does not get interpolation.

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
