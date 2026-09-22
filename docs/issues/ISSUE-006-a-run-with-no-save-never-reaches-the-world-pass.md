# ISSUE-006 — A run with no save never reaches the world pass

**State:** resolved. **Affects:** ST-REPLAY, ST-NULLDIFF, ST-HEADLESS-GAME, and every
interpolation claim that depends on substituting the view into a replay.

## The symptom

`tools/interpolated_frame.py` armed a blend at t=0.5 over 65 shaders carrying the view, the
replay came back with 64 uniform assemblies of its own, and the view was written into **none**
of them: the two shader sets did not intersect at all. The interpolated frame was byte-identical
to the title's. `GET /frames` said every published frame held the same 10 to 12 distinct shaders,
in 16 lists of about 35 KB — so the recorder was not splitting frames at the wrong boundary. It
was consistently seeing a small pass and never the world pass, for long stretches at a time.

## The cause

The driven run was never in gameplay. It was sitting on Wind Waker HD's name-entry software
keyboard — *"Enter your name. Use the GamePad to input."* — which no random button press can
pass, and which genuinely draws nothing but a 2D pass. The 183-shader peak the search had
measured at frame 116 was the intro, not gameplay; by frame 516 it was down to 12 and stayed
there. Every conclusion drawn from "the recorded frame holds one pass" was a correct measurement
of the wrong scene.

Finding it took capturing the frame the tool refused on. A refusal that only names a stage would
have kept the boundary theory alive; `refused.png` ended it in one look.

## The fix

`tools/wiiuport/headless.py` stages a save into the session's own mlc before launch, and
`tools/wiiuport/title.py` resolves it from `--save` or `$WIIUPORT_SAVE`. It is **copied, never
linked**: a driven run presses buttons at random and writes to its save as it plays, so a link
would let a maintainer run ruin the operator's quest log. A named save that is not there is
refused; no save named at all is a legitimate answer, and the run then stops at the keyboard
exactly as before.

## What it measures now

One run, with a quest log staged, on Outset Island:

```
armed at t=0.5 over 86 shaders carrying the view
frames 1194, last frame held 21 lists and 5715 assemblies in 2538652 bytes
the title drew 926638 times from command buffers and 0 straight from the ring
1 replay of 21 lists, 22 submissions, 59572 packets walked, 5119 draws issued
the replay produced 512 display lists and 5709 uniform assemblies of the runtime's own;
the view was written into 1999 of them (3710 did not carry it, 0 too short, 0 unarmed)
1194 frames published; the last 32 held 0 to 256 distinct shaders
title frame vs interpolated frame: 1823023 of 6220800 bytes differ (29.305%),
  largest 215, mean 1.0315, over x 0..1919, y 0..1079 -- 1080 rows touched
```

The diff's shape is the claim: a half-tick camera shift moves **everything** and moves nothing
far. A blend that had written garbage would show a large maximum over part of the frame; a blend
that had written nothing would show zero. 29% of bytes differing by a mean of 1 across every row
is a camera the title's own frame did not show.

## What this does not settle

The 3,710 assemblies that did not carry the view are not a defect — most shaders in a frame take
no view matrix — but nothing yet distinguishes those from a shader whose view lives at an offset
the search has not found. Actor transforms are a separate search (GOAL-INTERP), and the blend is
still armed a frame at a time by hand rather than run every frame (ST-60).
