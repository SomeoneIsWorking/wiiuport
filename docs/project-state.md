# wiiuport — project state

Factual capability inventory. Epic intent is in `docs/project-goals.md`.
Every item is `verified`, `partial`, `blocked`, or `missing`.

**Current focus.** ST-RECORD/ST-REPLAY — the camera is identified by value and the draw
stream is measured, so the next step is holding a frame's display lists by copy and re-feeding
them through the command processor with substituted transforms. The local build now works, so
real-title runs no longer wait on a CI artifact.

## Comparison baseline

**Baseline: upstream Cemu (`cemu-project/Cemu`), used as a standalone application.**
A user today runs the Cemu AppImage, points it at a WUX, and plays at the title's own
simulation rate with community graphic packs for resolution and shading. Upstream offers
no mechanism to re-issue a frame with substituted transform state, so a 30 Hz title
presents at 30 Hz. Every item below states its difference from that baseline.

| ID | Capability (delta from baseline) | State | Evidence / exact gap |
|---|---|---|---|
| ST-FORK | Cemu fork exists and is pinned as a submodule at a reviewable revision | verified | `SomeoneIsWorking/Cemu` forked from `cemu-project/Cemu`; `external/cemu` submodule pinned at upstream `54ffbed`. |
| ST-BUILD | Pinned fork configures and builds from a clean tree with Clang + Ninja | verified | Verified on hosted Linux from a cold checkout (run 35436011965: Clang, Ninja, full vcpkg and Cemu corpus in 32 minutes) and now locally on Fedora, producing a 299 MB `external/cemu/bin/Cemu_relwithdebinfo` with `CMAKE_CXX_COMPILER_ID=Clang` read from the configured cache. The local build needed `libpng-static`, which Fedora splits out of `libpng-devel`; `tools/hostdeps.py` names it and the host now has it. |
| ST-LIB | Runtime exposed to a consuming title through a narrow C++ interface | missing | No interface exists. Cemu is currently only an application entry point. |
| ST-RECORD | A frame's guest draw stream can be recorded for replay | partial | The stream's shape is measured, which is what the recorder has to hold: at a real scene 298 indirect buffers and 77,536 bytes per frame, so a per-frame copy is cheap. Copying is required rather than assumed -- 175 of 175 distinct display list addresses recur across frames, so the guest recycles the storage and a recorded pointer would read the next frame's data. Cemu's `LatteCommandProcessor` still consumes PM4 packets in place and retains no stream; only the measurement exists. |
| ST-REPLAY | A recorded frame replays with substituted transform state | missing | Depends on ST-RECORD. Substitution happens at the Vulkan renderer's uniform-assembly site (`uniformData_updateUniformVars`), which covers both Latte uniform modes in one place. |
| ST-REPLAY-GL | Interpolation under the OpenGL renderer | missing | Deliberately out of scope: the assembly site used for substitution is the Vulkan renderer's. OpenGL presents at the guest rate. |
| ST-NULLDIFF | Null-interpolation discriminator: replay at t=1 is byte-identical to the original frame | missing | The gate that proves replay is faithful before any blending is trusted. Depends on ST-REPLAY. |
| ST-SHADOW | Substituted transform storage never writes back to guest memory | missing | Depends on ST-REPLAY. |
| ST-COUNTERS | Runtime reports frames recorded/replayed, bailouts by reason, substituted slots, all with denominators | missing | Required before any interpolation claim. |
| ST-CONTROL | Opt-in control channel for input injection, frame stepping, counters, capture | missing | Must use `lucent::http::Server`; no local HTTP server may be written here. |
| ST-HEADLESS-ENV | Offscreen, silent, isolated environment for maintainer runs | verified | `tools/wiiuport/headless.py`. Live run reported `DISPLAY=:97`, its own `XDG_CONFIG_HOME`, a working X server, and clean teardown; settings name no audio device. 7 tests cover isolation, the timeout path, the exit-code path, refused missing keys, and the self-kill guard. |
| ST-HEADLESS-GAME | A maintainer run reaches gameplay in that environment | partial | Offscreen runs reach a drawn scene and are driven by `tools/capture_uniforms.py`: a run at frame 900 recorded 12,737 draws across 8 frames with no window and no audio device. Gap: no input is driven, so only unattended scenes have been reached, never in-game control. |
| ST-CI-LINUX | Hosted Linux CI: configure, build, lint, test with Clang | verified | `.github/workflows/ci.yml` run 35436011965: gates job (ruff, pytest, clang-format, C++ ownership policy, structure) and runtime job (cold vcpkg + Cemu build with Clang/Ninja) both green, ending with an explicit assertion that the binary exists rather than a silent pass. Getting here fixed four real defects: a missing `libudev-dev`, an unexplained `VCPKG_FORCE_SYSTEM_BINARIES` that stopped vcpkg provisioning ninja, a refusal that named a log path the runner discards, and a toolchain check that read `CMAKE_CXX_COMPILER_ID` from the cache, where CMake never writes it. |
| ST-CI-WIN | Hosted Windows CI | not applicable | Windows is not a claimed host; see GOAL-PLATFORM. A job running only the Python gates would report green for a platform nothing has been built on. Becomes applicable when something targets Windows, with a real build job in the same change. |
| ST-CI-MAC | Hosted macOS CI | not applicable | Same reason as ST-CI-WIN. |
| ST-VERIFIER | Canonical Python verifier carrying format, tidy, structure, and test gates | partial | `tools/verify.py` runs ruff, pytest, a non-mutating `clang-format` check, the three syntax-aware ownership rules on the real libclang AST, and source-size limits; 5 of 5 gates pass over 40 tests. The ownership gate is proven on both classes: an accepted fixture reports nothing and a rejected one reports seven findings across all three rules (ISSUE-002). `.clang-tidy` is configured and confirmed (`--dump-config` keeps `clang-diagnostic-*`, `--list-checks` reports 251 active). Gap: `clang-tidy` is not yet executed, because it needs a compile database and therefore a completed runtime build. |