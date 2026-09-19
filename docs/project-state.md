# wiiuport — project state

Factual capability inventory. Epic intent is in `docs/project-goals.md`.
Every item is `verified`, `partial`, `blocked`, or `missing`.

**Current focus.** ST-BUILD — get the pinned Cemu fork building from source with Clang.
Everything downstream of it is blocked until a runtime binary exists.

## Comparison baseline

**Baseline: upstream Cemu (`cemu-project/Cemu`), used as a standalone application.**
A user today runs the Cemu AppImage, points it at a WUX, and plays at the title's own
simulation rate with community graphic packs for resolution and shading. Upstream offers
no mechanism to re-issue a frame with substituted transform state, so a 30 Hz title
presents at 30 Hz. Every item below states its difference from that baseline.

| ID | Capability (delta from baseline) | State | Evidence / exact gap |
|---|---|---|---|
| ST-FORK | Cemu fork exists and is pinned as a submodule at a reviewable revision | verified | `SomeoneIsWorking/Cemu` forked from `cemu-project/Cemu`; `external/cemu` submodule pinned at upstream `54ffbed`. |
| ST-BUILD | Pinned fork configures and builds from a clean tree with Clang + Ninja | partial | Configure with `clang 22.1.8` + `ninja 1.13.2` started and is resolving vcpkg ports; no completed build yet. Fedora 44 satisfies Cemu's documented `zlib-devel`/`perl-core` via `zlib-ng-compat-{devel,static}` and the base `perl` split packages. |
| ST-LIB | Runtime exposed to a consuming title through a narrow C++ interface | missing | No interface exists. Cemu is currently only an application entry point. |
| ST-RECORD | A frame's guest draw stream can be recorded for replay | missing | Cemu's `LatteCommandProcessor` consumes PM4 packets in place and retains no per-frame stream. |
| ST-REPLAY | A recorded frame replays with substituted transform state | missing | Depends on ST-RECORD. Substitution happens at the Vulkan renderer's uniform-assembly site (`uniformData_updateUniformVars`), which covers both Latte uniform modes in one place. |
| ST-REPLAY-GL | Interpolation under the OpenGL renderer | missing | Deliberately out of scope: the assembly site used for substitution is the Vulkan renderer's. OpenGL presents at the guest rate. |
| ST-NULLDIFF | Null-interpolation discriminator: replay at t=1 is byte-identical to the original frame | missing | The gate that proves replay is faithful before any blending is trusted. Depends on ST-REPLAY. |
| ST-SHADOW | Substituted transform storage never writes back to guest memory | missing | Depends on ST-REPLAY. |
| ST-COUNTERS | Runtime reports frames recorded/replayed, bailouts by reason, substituted slots, all with denominators | missing | Required before any interpolation claim. |
| ST-CONTROL | Opt-in control channel for input injection, frame stepping, counters, capture | missing | Must use `lucent::http::Server`; no local HTTP server may be written here. |
| ST-HEADLESS-ENV | Offscreen, silent, isolated environment for maintainer runs | verified | `tools/wiiuport/headless.py`. Live run reported `DISPLAY=:97`, its own `XDG_CONFIG_HOME`, a working X server, and clean teardown; settings name no audio device. 7 tests cover isolation, the timeout path, the exit-code path, refused missing keys, and the self-kill guard. |
| ST-HEADLESS-GAME | A maintainer run reaches gameplay in that environment | missing | Depends on ST-BUILD. The environment above has never been run against the runtime binary, which does not exist yet. |
| ST-CI-LINUX | Hosted Linux CI: configure, build, lint, test with Clang | missing | |
| ST-CI-WIN | Hosted Windows CI on a supported MSVC/clang-cl configuration | missing | |
| ST-CI-MAC | Hosted macOS CI on AppleClang | missing | |
| ST-VERIFIER | Canonical Python verifier carrying format, tidy, structure, and test gates | partial | `tools/verify.py` runs ruff, pytest, a non-mutating `clang-format` check, and source-size limits; 4 of 4 gates pass. `.clang-tidy` is configured and confirmed (`--dump-config` keeps `clang-diagnostic-*`, `--list-checks` reports 251 active). Gaps: `clang-tidy` is not yet executed against a compile database, and the three syntax-aware rules it cannot express are unwritten (ISSUE-002). Both are due with the first first-party C++ file. |
