# ISSUE-004 — Offscreen runs fall back to llvmpipe, so they cannot carry GPU evidence

**State:** open, cause unknown. **Affects:** ST-HEADLESS-GAME, and every future
performance or frame claim.

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

The machine has an AMD Radeon RX 6700 XT on RADV.

## The cause that was recorded here, and why it is wrong

This issue previously asserted that Xvfb provides no DRI3, so RADV reports no present
support, fails `VulkanRenderer::IsDeviceSuitable`, and the enumeration falls through to
the software rasteriser. That reading of the source is accurate as far as it goes, but
as an explanation of the measurement it is **falsified**:

```
$ Xvfb :91 -screen 0 1280x720x24 &
$ DISPLAY=:91 vkcube --c 30
Selected GPU 0: AMD Radeon RX 6700 XT (RADV NAVI22), type: DiscreteGpu
```

An ordinary Vulkan client presenting to an X11 surface on this same Xvfb gets RADV and
runs to completion. Whatever pushed Cemu onto llvmpipe, it is not "Xvfb cannot present
on RADV". The `No DRI3` line was read as the cause when it was only the loudest message
nearby.

The remaining candidates are untested, and will stay untested until the runtime builds:
the harness's isolated environment differs from a plain shell and could change ICD
enumeration; Cemu's configuration carries a `graphic_device_uuid` that the harness's
settings template leaves unset, so device choice falls to whatever Cemu defaults to; and
Cemu builds its surface from a GTK window rather than its own, which vkcube does not
exercise.

## Why it matters

Correctness observations from such a run are still usable — the swap-interval
measurement in ISSUE-003 came from one, and it read a value the guest wrote, which the
host renderer cannot change. **Anything about frame timing, throughput, or GPU
behaviour from a run like this is worthless**, and an interpolated-60 Hz performance
claim built on it would be a fabrication. Frame captures are also suspect until the
rendering device is the real one.

## What resolves it

No installation is required, which is the other thing the old entry got wrong: it asked
for `weston` or `cage`. `gamescope` is already on this machine and runs headless on the
real GPU:

```
$ ENABLE_GAMESCOPE_WSI=0 gamescope --backend headless -W 1280 -H 720 -- vkcube --c 60
[gamescope] vulkan: selecting physical device 'AMD Radeon RX 6700 XT (RADV NAVI22)'
Selected GPU 0: AMD Radeon RX 6700 XT (RADV NAVI22), type: DiscreteGpu
[gamescope] launch: Primary child shut down!
```

So there are three things to try against the built runtime, cheapest first:

1. Pin `graphic_device_uuid` in the harness's settings template to the real device.
2. Run the harness under `gamescope --backend headless` instead of bare Xvfb.
3. A real headless mode in the fork: drop the surface requirement from device selection
   and capture rendered images instead of presenting. This is the better long-term
   answer because it also serves frame capture and hosted CI, neither of which should
   depend on a compositor being installed.

Which of these is needed depends on the cause, which is not yet known. Until one is in
place and verified, every gate and every claim that depends on the renderer must state
that it has not been measured on the real device, rather than quietly inheriting a
software-rendered result.
