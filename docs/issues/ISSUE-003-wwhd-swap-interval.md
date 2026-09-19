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

`LatteTiming_signalVsync` only flips when `s_vsyncIntervalCounter >= swapInterval`. With
an interval of 2, the title already runs on a 60 Hz vsync cadence in which **every second
flip repeats the previous image**. Interpolation therefore fills a slot that already
exists rather than adding presents:

- on the vsync that carries a new guest frame, present it as today;
- on the repeat vsync, present the blended replay instead of the duplicate.

This supersedes the two-presents-per-tick ordering that
`docs/frame-interpolation.md` was written against. There is no extra present and no
extra latency beyond the half-tick the blend inherently needs.

## What it does not decide

The interval says how often the guest flips, not that the guest's *simulation* advances
once per flip. That still needs confirming against gameplay before a blend at t=0.5 is
assumed to be the right phase.
