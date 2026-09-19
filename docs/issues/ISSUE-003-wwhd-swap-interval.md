# ISSUE-003 — Wind Waker HD's swap interval: measured, resolved

**State:** resolved by measurement. **Affects:** wiiuport ST-REPLAY; setsail ST-60.

## Result

**Wind Waker HD calls `GX2SetSwapInterval(2)`**, once, during startup.

Measured on 2026-09-19 from a driven offscreen run of the baseline emulator against the
player's own disc image, with GX2 logging enabled (`LogType::GX2` is bit 1, so
`<logflag>2</logflag>`). The run mounted title `0005000010143500` v0, initialised the
Vulkan backend, and ran for 121 s; its log contains exactly one matching line:

```
[11:29:11.754] GX2SetSwapInterval(2)
```

One occurrence is the whole answer here, not a truncated sample: the search was over the
full 791,368-line log, and a title that changed the interval later would have produced
further lines.

## What it decides

Less than this issue originally claimed. It asserted that with an interval of 2 the
title already runs on a 60 Hz cadence in which every second flip repeats the previous
image, so interpolation could fill an existing slot and add no present. Reading the
fork falsifies that:

- `LatteTiming_signalVsync` (`src/Cafe/HW/Latte/Core/LatteTiming.cpp:77`) gates the
  **guest's** flip accounting — a counter reaches the interval and `flipExecuteCount`
  advances. It presents nothing.
- Host presentation is one `g_renderer->SwapBuffers(true, true)` per guest scan-buffer
  swap, in `LatteRenderTarget_itHLESwapScanBuffer`
  (`src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp:679`).

So at interval 2 the title produces 30 frames a second and Cemu presents 30 times a
second. There is no duplicate present to replace.

What the measurement does decide is the **tick rate**: the title advances its rendered
frame 30 times a second, which is the denominator interpolation has to double. How the
extra frame reaches the screen is a separate question, answered in
`docs/frame-interpolation.md`: the runtime adds a present and owns its own pacing
between guest swaps.

## What it does not decide

The interval says how often the guest flips, not that the guest's *simulation* advances
once per flip. That still needs confirming against gameplay before a blend at t=0.5 is
assumed to be the right phase.
