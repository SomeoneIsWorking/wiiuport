# wiiuport — project state

Factual capability inventory. Epic intent is in `docs/project-goals.md`.
Every item is `verified`, `partial`, `blocked`, or `missing`.

**Current focus.** ST-BUILD — get the pinned Cemu fork building from source with Clang.

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
| ST-REPLAY | A recorded frame replays with substituted transform state | missing | Depends on ST-RECORD. |
| ST-NULLDIFF | Null-interpolation discriminator: replay at t=1 is byte-identical to the original frame | missing | The gate that proves replay is faithful before any blending is trusted. Depends on ST-REPLAY. |
| ST-SHADOW | Substituted transform storage never writes back to guest memory | missing | Depends on ST-REPLAY. |
| ST-COUNTERS | Runtime reports frames recorded/replayed, bailouts by reason, substituted slots, all with denominators | missing | Required before any interpolation claim. |
| ST-CONTROL | Opt-in control channel for input injection, frame stepping, counters, capture | missing | Must use `lucent::http::Server`; no local HTTP server may be written here. |
| ST-HEADLESS | Offscreen, silent, unpaced maintainer run that reaches gameplay | missing | Needed so agent runs never seize the desktop or the audio device. |
| ST-CI-LINUX | Hosted Linux CI: configure, build, lint, test with Clang | missing | |
| ST-CI-WIN | Hosted Windows CI on a supported MSVC/clang-cl configuration | missing | |
| ST-CI-MAC | Hosted macOS CI on AppleClang | missing | |
| ST-VERIFIER | Canonical Python verifier carrying format, tidy, structure, and test gates | missing | Must include `clang-format` check, `clang-tidy` against the real compile database for first-party sources only, and source-size limits. Upstream Cemu sources are vendored and are not reformatted or tidied. |
