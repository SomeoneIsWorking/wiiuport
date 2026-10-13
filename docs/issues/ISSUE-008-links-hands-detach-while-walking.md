# ISSUE-008 — Link's hands detach while walking

**State:** fixed. **Affects:** ST-OBJECTS (the in-between paint).

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
