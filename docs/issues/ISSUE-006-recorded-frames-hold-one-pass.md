# ISSUE-006 — A recorded frame holds one pass, not the frame the search analysed

**State:** open, cause partly known. **Affects:** ST-REPLAY, ST-NULLDIFF, and every
interpolation claim that depends on substituting the view into a replay.

## What was measured

`tools/interpolated_frame.py` on Wind Waker HD, after the top-level command buffers were
hooked (see below), with the blend armed at t=0.5:

```
armed at t=0.5 over 65 shaders carrying the view
last frame held 16 lists and 64 assemblies in 34976 bytes; nested lists 115064
17 submissions ... 1408 packets walked, 36 draws issued
the replay produced 36 display lists and 64 uniform assemblies of the runtime's own;
the view was written into 0 of them (64 did not carry it, 0 too short)
  armed for 65 shaders:
    stage 0 shader 07d0dd71bdcd3795:0 (view at float 32)
    ...
  the replay offered 12 distinct shaders:
    stage 0 shader 6669a23d03806414:0 (float 68 long, 14 draws)
    stage 1 shader 2802e519ac163806:79 (float 8 long, 8 draws)
    ...
title frame vs interpolated frame: 0 of 6220800 bytes differ
```

The replay is faithful to what was recorded -- 64 recorded assemblies, 64 replayed -- and the
two shader sets do not intersect at all. The search's own report says the same thing from the
other side: the view candidate is shared by 65 shaders but was seen in only 57 of 2,361
recorded frames.

## What that means

The recording is not a whole frame. A frame's world pass and its 2D/GamePad pass reach the
recorder in different frame windows, so the frame that gets replayed is usually the small
one: 12 shaders, uniform buffers 8 to 92 floats, no view transform in any of them. Arming a
blend from a candidate the search saw in some other frame therefore writes it nowhere, and
the interpolated frame comes out byte-identical -- which is exactly what a replay that drew
nothing looks like.

## What was already fixed on the way here

The frame's real command buffers enter Latte through `LatteCP_itIndirectBufferDepr`, the TCL
queue path, which was never hooked. Only `LatteCP_itIndirectBuffer` -- the *nested* buffers
referenced from inside them -- reported to the observer. Every recording before this held
state with no geometry in it: 36 lists, 532 packets walked, **0 draws issued**. With the
top-level path hooked and nested buffers counted instead of recorded (recording both would
issue their contents twice), the same replay walks 1,408 packets and issues 36 draws.

## What has been settled since

**The arm no longer hands out stale shaders.** A shader's last values outlive the frame it
stopped drawing in, so `TransformSearch::viewSlots()` was offering 65 shaders that had carried
the view at some point. It now offers only shaders that drew in the most recent frame -- the
frame a replay re-issues -- and `GET /transforms` reports how many that is. The tool's answer
changed from a silent identical image to `no view transform found yet`, which is the same
finding stated honestly.

**No draw escapes the recorder's reach.** Every guest draw is now counted by origin, and the
measurement is unambiguous: **152,215 draws from command buffers, 0 straight from the ring**,
over 2,290 frames. So the missing pass is not in a path the recorder cannot see; it is in a
frame window the recorder does not attribute to the frame being replayed.

## The frame window does not alternate

`GET /frames` keeps the shape of the last 32 published frames, and the answer is not the one
the boundary theory predicted:

```
2392 frames published; the last 32 held 10 to 12 distinct shaders
  frame 2360: 16 lists, 64 assemblies across 12 shaders, 34976 bytes
  ...
  frame 2370: 16 lists, 56 assemblies across 10 shaders, 23552 bytes
  ...
  frame 2391: 16 lists, 64 assemblies across 12 shaders, 34976 bytes
```

Every frame is the same small shape. Nothing alternates, so the recorder is not publishing
half of each frame at the wrong boundary; it is consistently seeing the same small pass and
never the world pass, for long stretches at a time.

## What that leaves

The numbers still to reconcile, all from one run of 2,392 frames:

- 166,670 guest draws, 0 of them from the ring: every draw came out of a command buffer.
- 40,166 top-level command buffers, all of them recorded: ~17 per frame, matching the 16 in
  each published frame.
- 271,949 uniform assemblies seen, ~114 per frame, against 56 to 64 in every published frame.

Roughly half of each frame's assemblies are seen by the hook and are not in the frame that
gets published, while every buffer the title submits is recorded. The next measurement is
therefore about where those assemblies are relative to the frame's scan-buffer copies: record
the presents that fall inside each frame window, by target, alongside its shape. A window that
holds one copy rather than two is a frame boundary that fires mid-frame; a window that holds
both and still misses half the assemblies means the draws are reaching the renderer through a
path that submits no command buffer of its own.

None of this is a reason to weaken the gate: `tools/interpolated_frame.py` refuses by naming
which stage failed, and it is the instrument that found every one of these.
