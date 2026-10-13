# ISSUE-007 — The shadow flickers while the camera turns

**State:** fixed. **Affects:** ST-OBJECTS (the in-between paint).

## The symptom

Reported from `./run.sh` at 60 Hz on Outset: turning the camera with the right stick makes the
shadow flicker. Reproduction: `scratch/drawphase/shadow_live.py` against a running session holds
`rightx=1` and captures four consecutive paints (`turn_on_*.png`).

## Evidence

User screencast (3 s, 44 paints) while turning the camera on Outset: in paints 9 and 11 a large
dark terrain-shadow region covers the left grass and is gone in the paints either side, so the
terrain shadow map jumps between paints; Link's own shadow is steady.

## The cause

`LogicGate::payload`'s skipped call ran only fpcM's draw half and not the frame logic's
`FUN_0278febc(shadowLayer + 0x15d4)`, the shadow prepass's per-frame double-buffer flip. Both paints
of a tick wrote one slot, and the render thread drew the in-between paint's shadow maps from the
following paint's constants while its lookup used its own.

## The fix

The skipped call runs the frame logic's own guard and flip (lifted from `0x020359f0`) after the
draw half. Test: `logic_gate_tests.cpp`, the skipped draw's words (fails before, passes after).

## Evidence

`scratch/drawphase/shadow_turn.py --point 5` (Outset hilltop over the beach, `sea` room 44 point 5,
camera turned on `rightx=1`, eight runs of five consecutive paints): before, three of eight runs
alternated paint to paint in lower-half brightness (272, 227, 266, 231, 259) with the cliff shadow
over the beach on every other paint; with the blend off, paints came in equal pairs. After, all
eight runs change monotonically and the beach stays lit (`shadowturn/before_after.png`).
