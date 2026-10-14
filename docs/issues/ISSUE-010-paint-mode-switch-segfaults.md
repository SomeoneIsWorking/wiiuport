# ISSUE-010 — Switching the paint mode while armed segfaults

**State:** open. **Affects:** ST-CONTROL, `title::WindWakerPaint::enable`.

**Reproduction.** Boot with the save, reach play, then `POST /paint?on=1&mode=3` while mode 13
(`loopFrameLiteralTwice`, the default) is armed. The log shows the slot put back, `twiceAtSixty`
written to the same block `0x00e05988`, then `signal 11`; the PPC stack has return address
`0x00e05990`, inside the old stand-in (`scratch/perf/run.log`, 2026-10-14).

**Cause.** `WindWakerPaint::enable` rewrites the stand-in block in place on a mode change. Holding the
probe's lock only proves the display thread is not inside the probe; the stand-in calls the frame
twice, so between the calls the thread is in the block being rewritten, and its recompiled function
is deleted under it.

**Fix to make.** On a mode change, put the slot back and write the new block only after a paint has
gone through the title's own frame, or write the new block elsewhere and switch the slot.
Switching off and on in the same mode does not hit it (measured: four alternating windows).
