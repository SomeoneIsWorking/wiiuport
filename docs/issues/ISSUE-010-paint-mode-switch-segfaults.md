# ISSUE-010 — Switching the paint mode while armed segfaults

**State:** fixed. **Affects:** ST-CONTROL, `title::WindWakerPaint::enable`.

**Reproduction.** Boot with the save, reach play, then `POST /paint?on=1&mode=3` while mode 13
(`loopFrameLiteralTwice`, the default) is armed. The log shows the slot put back, `twiceAtSixty`
written to the same block `0x00e05988`, then `signal 11`; the PPC stack has return address
`0x00e05990`, inside the old stand-in (`scratch/perf/run.log`, 2026-10-14).

**Cause.** `WindWakerPaint::enable` rewrites the stand-in block in place on a mode change. Holding the
probe's lock only proves the display thread is not inside the probe; the stand-in calls the frame
twice, so between the calls the thread is in the block being rewritten, and its recompiled function
is deleted under it.

**Since the modes were deleted** there is one stand-in and `POST /paint` takes no `mode`, so the
mode switch above can no longer be requested. `enable()` still rewrites every word of the block on each
off-to-on (`WindWakerPaint.cpp`, the `m_writeWord(m_block + ...)` loop), and `GuestPatching::WriteBytes`
invalidates the recompiled range even when the words are unchanged, so a display thread still inside
the old stand-in at re-arm is the same hazard.

**Fix.** `enable()` writes only the words the block does not already hold, so re-arming the one
stand-in writes nothing and the recompiled block stays (`tests/cxx/paint_tests.cpp`, re-arming
leaves the block unwritten). Live: on, off, on, off on the save with no fault.
