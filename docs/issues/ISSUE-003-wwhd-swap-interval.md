# ISSUE-003 — Determine Wind Waker HD's swap interval before fixing the present model

**State:** open, blocks the present model in `docs/frame-interpolation.md`.
**Affects:** wiiuport ST-REPLAY; setsail ST-60.

`LatteTiming_signalVsync` reads `swapInterval` from the guest's shared area and only
flips when `s_vsyncIntervalCounter >= swapInterval`. A Wii U title that runs its logic at
30 Hz typically does so by setting `GX2SetSwapInterval(2)`: the display stays at 60 Hz
and the same image is shown for two vsyncs.

**Why this matters.** It decides the present model, and the two answers give materially
different designs:

- **If Wind Waker HD uses `swapInterval == 2`**, the runtime already has a 60 Hz vsync
  cadence with every second flip repeating the previous image. Interpolation then fills
  a slot that already exists: on the repeat vsync, present the blended replay instead of
  the duplicate. No extra latency beyond what the blend inherently needs, and the
  existing timing structure is reused rather than replaced.
- **If it uses `swapInterval == 1`** and simply submits at 30 Hz, the two-presents-per-
  tick ordering currently written in `docs/frame-interpolation.md` applies.

**How to settle it.** Read the value the title actually sets, from a driven offscreen
run: log `LatteGPUState.sharedArea->swapInterval` and the observed guest flip rate
against the vsync rate. Reading the field alone is not enough — a title can change it,
so the measurement must cover gameplay, not just boot.

Do not pick a present model from the more convenient answer. The document currently
states the `swapInterval == 1` model; if the measurement says otherwise it is corrected
there, not annotated.
