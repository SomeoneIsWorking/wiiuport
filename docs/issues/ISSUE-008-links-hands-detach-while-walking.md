# ISSUE-008 — Link's hands detach while walking

**State:** fixed (reopened and fixed 2026-10-20). **Affects:** ST-OBJECTS (the in-between paint).

## The symptom

Reported from `./run.sh` at 60 Hz on Outset: while Link walks, his hands come off his arms.
Reproduction: `scratch/drawphase/shadow_live.py` against a running session holds `lefty=1` and
captures four consecutive paints (`walk_on_*.png`).

## The cause

`DrawInterpolation::blendModel` blended every model's joints independently. Link's hands model takes
the body's hand joints by copy (`setDrawHandModel`, `setAnmMtx` from `mpCLModel`) after the body's
view pass, so on the in-between paint the hands held the body's already-blended joints and were
blended a second time, toward the previous tick's pose.

## The fix

`blendModel` keeps each joint it blended in the draw phase as `held -> blended`; a later joint holding
exactly a `held` matrix is written that blend. Test:
`aJointCopiedFromAModelDrawnBeforeItIsDrawnAtThatModelsBlend` (fails before, passes after).

## Evidence

`scratch/drawphase/walk_fix.py` (game-frame waits), Outset pier walk: 15 numbered paints
(`walkfix/link_sheet.png`), hands on the sleeves in every one; `jointsCopied` 21,561, 0 unblendable,
unreadable or failed writes.

## Reopened (2026-10-20)

Reported again from `./run.sh` at 60 Hz on Outset: running, the left fist floats off the arm.
Reproduction: `scratch/drawphase/hands.py --warp Hyrule:0:4 --stick=1.0:0.0 --lead 8 --bursts 2`.

Cause: `DrawInterpolation::blendModel` gave up on a whole model when one joint was not a matrix.
The hands model (`mpHandsModel`, 11 joints) never calculates its world root, joint 0, so it holds
whatever the heap left there; in some runs that is not floats (`0x1016df1c`, `0xff000000`), and
then the hands model was drawn at the tick's own pose on every in-between paint while the arms
were at the midpoint: 619 of 619 unblendable reports in one run. Which runs it hits depends on the
heap, which is why the walk check above passed.

Fix: a joint that is not a matrix is drawn as it is (`jointsKept`) and the rest of the model still
blends. Shown: a run with the garbage root (`jointsKept` 619, `unblendable` 0), five consecutive
presents from the side with both fists on the sleeves (`scratch/drawphase/hands10`).
