# ISSUE-007 — The shadow flickers while the camera turns

**State:** open. **Affects:** ST-OBJECTS (the in-between paint).

## The symptom

Reported from `./run.sh` at 60 Hz on Outset: turning the camera with the right stick makes the
shadow flicker. Reproduction: `scratch/drawphase/shadow_live.py` against a running session holds
`rightx=1` and captures four consecutive paints (`turn_on_*.png`).

## Evidence

User screencast (3 s, 44 paints) while turning the camera on Outset: in paints 9 and 11 a large
dark terrain-shadow region covers the left grass and is gone in the paints either side, so the
terrain shadow map jumps between paints; Link's own shadow is steady.

## The cause

Not yet traced.
