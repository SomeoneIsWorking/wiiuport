# ISSUE-004 — Offscreen runs fall back to llvmpipe, so they cannot carry GPU evidence

**State:** open. **Affects:** ST-HEADLESS-GAME, and every future performance or frame
claim.

## What was measured

A driven offscreen run of the baseline emulator under the harness's Xvfb display booted
the real title correctly, but selected the wrong device:

```
Using GPU: llvmpipe (LLVM 22.1.8, 256 bits)
Driver version: Mesa 26.1.8 (LLVM 22.1.8)
```

with the surrounding output repeating:

```
MESA: info: vulkan: No DRI3 support detected - required for presentation
```

The machine has an AMD Radeon RX 6700 XT on RADV. Xvfb provides no DRI3, so RADV cannot
present to that X surface, and the device selection falls back to the software
rasteriser.

## Why it matters

Correctness observations from such a run are still usable — the swap-interval
measurement in ISSUE-003 came from one, and it read a value the guest wrote, which the
host renderer cannot change. **Anything about frame timing, throughput, or GPU
behaviour from a run like this is worthless**, and an interpolated-60 Hz performance
claim built on it would be a fabrication. Frame captures are also suspect until the
rendering device is the real one.

## What resolves it

An offscreen presentation path that keeps RADV. The candidates, in order of preference:

1. A headless Wayland compositor with DRI3 (`weston --backend=headless`, or `cage`),
   which Xvfb cannot provide. Neither is installed; this needs
   `sudo dnf install weston` (or `cage`).
2. Selecting the physical device explicitly in the runtime's configuration and
   capturing rendered images rather than presenting, so no presentable surface is
   required.

Until one of these is in place, every gate and every claim that depends on the renderer
must state that it has not been measured on the real device, rather than quietly
inheriting a software-rendered result.
