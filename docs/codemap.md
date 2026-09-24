# wiiuport — codemap

Ownership and placement only. No status, goals, or evidence; those live in
`docs/project-state.md`, `docs/project-goals.md`, and `docs/issues/`.

## Dependency direction

```
setsail (title: identity, transform meaning, blend policy, packaging)
  -> wiiuport (runtime host, frame record/replay, control channel, evidence counters)
       -> external/cemu  (pinned fork of cemu-project/Cemu, MPL-2.0)
       -> lucent         (logging, layered configuration, HTTP server)
```

`wiiuport` never contains a title's addresses, identity, transform layout, or blend
rules. `external/cemu` is a consumed fork: changes to it are commits on the fork, never
tracked patch files, and the submodule pin is the source of truth.

## Responsibility owners

| Responsibility | Owner | Notes |
|---|---|---|
| Wii U guest execution, Latte GPU, GX2/OS HLE, audio, input backends | `external/cemu` (fork) | Upstream code. First-party policy does not move into it beyond the interposition hooks below. |
| Fork interposition hooks | `external/cemu`, minimal and per-cause | The fork exposes callbacks at the frame boundary and at `LatteCP_itIndirectBuffer`; it does not implement recording, blending, or policy. |
| Process-lifetime ownership of first-party state | `src/wiiuport/Runtime.h` | One object, installed once. The hook registry holds a raw pointer, so what it points at outlives every frame. |
| Frame capture of the guest draw stream | `src/wiiuport/frame/` | Records the command buffers the title's queue submits, by copy, because the guest reuses the storage. Buffers referenced from inside them are counted, not recorded: replaying the outer one walks into them. |
| Driving the gamepad | `src/wiiuport/input/InputDriver.h` | Supplies presses and stick positions to the emulated gamepad through the fork's one input boundary. Declines the player when nothing is queued, so an unused build behaves as upstream. |
| Finding the view transform | `src/wiiuport/interp/TransformSearch.h` | Classifies uniform slots by behaviour across frames and draws. Pure: a test drives it frame by frame with no emulator. |
| What each published frame held | `src/wiiuport/frame/FrameShapeLog.h` | A window of recent frame shapes -- lists, assemblies, distinct shaders, bytes -- kept as shapes and never as frames. One frame's totals cannot show whether the recorder publishes whole frames; a run of them can. |
| Feeding the search | `src/wiiuport/frame/SearchFeed.h` | Folds each published frame in. Carries no policy, so the search stays testable and the recorder stays ignorant of its readers. |
| Frame replay | `src/wiiuport/frame/FrameReplayer.h` | Re-feeds recorded buffers through `LatteFrameHooks::SubmitDisplayList`. Submission is injected, so the replayer is tested without an emulator. Armed one frame at a time; never fires on its own. |
| When a replay may run | `src/wiiuport/frame/ReplayScheduler.h` | After a frame is published and never before, so nothing acts on a half-recorded frame. Keeps the recorder free of any opinion about replay. |
| Transform substitution interface | `src/wiiuport/interp/` | Title-neutral: a consumer registers which recorded dwords are transform state and supplies the blend. The runtime does not know what a camera is. |
| Writing a blended view into a replayed draw | `src/wiiuport/interp/TransformSubstitution.h` | Twelve floats at one offset, at the last moment before the buffer is uploaded, and only into the runtime's own assemblies. Counts what it did not write, split by reason. |
| Interpolating every frame | `src/wiiuport/interp/ContinuousInterpolator.h` | At each finished guest frame: blended replay under the guest-state guard, present, restore the guest's frame by copy (by replay when the copies cannot undo everything). Counts every tick it did not interpolate by reason, every restore by how, and times each phase. Owns no pacing: the display's FIFO present mode spaces the frames. |
| Putting the guest's frame back by copy | `external/cemu/src/Cafe/HW/Latte/Core/LatteGuestStateGuard.h`, `VulkanTextureShadow.cpp`; runtime side `src/wiiuport/frame/GuestStateGuard.h` | Fork: while open, copies each texture subresource aside before its first write (render targets at `UpdateCurrentFBO`, Vulkan clears and copies) and copies them back on close; counts what copies cannot undo (textures created, stream-out, a renderer with no copies). Runtime: opens and restores it, refusing misuse. |
| Checking the restore is exact | `src/wiiuport/interp/RestoreCheck.h`, `tools/wiiuport/restore_check.py` | A `TickProbe` that captures the guest's frame before an in-between frame and after its restore (or the in-between frame, as the control), armed with `POST /restorecheck`; the client compares them byte for byte. |
| Checking an in-between frame lies between its neighbours | `src/wiiuport/interp/NeighbourCheck.h`, `tools/wiiuport/neighbour_check.py` | A `TickProbe` that captures one interpolated tick's title frame, then the next consecutive tick's in-between frame and title frame, into capture slots 2-4, armed with `POST /neighbourcheck`; the client judges the in-between frame against both by code. Several probes share a tick through `interp::TickProbes`. |
| Following the view frame to frame | `src/wiiuport/interp/ViewTracker.h` | Finds last frame's view slot again in the next, and reseeds from the search when it is lost or every few frames. |
| Refusing to blend across a cut | `src/wiiuport/interp/CutDetector.h` | Judges the camera's own turn and step against its recent motion. Pure. |
| Planning each object's blend | `src/wiiuport/interp/ObjectPlanner.h` | Plans every guest draw as it is recorded against the same draw two frames back (the title double-buffers its uniform blocks) and checks the midpoint lands on its partner in the frame between, finding an object by its values when its blocks name another; draws at N every draw that writes depth alone and every pixel stage's values (`shading`); counts every object's outcome. Pure and single-threaded. `POST /blends?objectShaderOff=<hash>` (repeatable) draws one shader's objects as the title drew them, to attribute an artefact. |
| Whether a draw a frame before is the one an object passed through | `src/wiiuport/interp/Midpoint.h` | Judges, value by value, whether the midpoint of what an object moved in from N-2 to N lands on a candidate in N-1, forgiving each value's rounding to a float; fed uniforms by `ObjectPlanner` and vertices by `VertexBlend`. Pure. |
| Values an unverified object shares with blended draws | `src/wiiuport/interp/SharedValues.h` | Indexes, per frame, the values blended draws moved in, by (N-2 bits, N bits) across every shader and place, so `ObjectPlanner` draws an object drawn at N in what it held alike -- a pass's view or light -- as the blended draws are. Pure. |
| The camera's step in matrices each object is handed | `src/wiiuport/interp/SharedTransforms.h` | Learns, per frame, the transform T the blended draws of a shader agree changed a 4x4 of their values by (W' = T W), so `ObjectPlanner` draws an object drawn at N with T times its own matrix there. Pure. |
| Which objects were not blended | `src/wiiuport/interp/ObjectCensus.h`, `tools/wiiuport/census.py` | Groups one planned frame's objects by shader with each outcome counted, most un-blended first; taken by `ObjectBlend` on `POST /objects`, read with `GET /objects`. Pure. |
| Which draws read vertex bytes the title rewrote | `src/wiiuport/frame/VertexChanges.h`, `tools/wiiuport/draws.py` | Each of the title's draws' vertex bytes, from the fork's `OnDrawPrepared`, hashed and matched against the bytes its vertex shader read the frame before; draws without vertex uniforms always, the rest over a census (`POST /draws?frames=K`). Owned by `RecordingObserver`, read with `GET /draws`. |
| Blending each object by its own identity | `src/wiiuport/interp/ObjectBlend.h` | Feeds the planner on its own thread, off the thread that renders, and writes the planned blend into each replayed draw by draw order. |
| Blending the vertices the title rewrote | `src/wiiuport/interp/VertexBlend.h` | Keeps each of the title's draws' vertex bytes by the vertex-stage object it drew; at replay, lerps the float attributes that moved since N-2 against the draw of that object's planner partner in N-1 (where the object is told apart only by its place, the N-2 and N-1 draws of its shader its vertices most resemble, the N-1 one checked by `Midpoint`) and hands them to the fork's `VertexReplacements`, which binds them from a private ring (host-memory vertex path only). Counted by outcome in `GET /interpolation`, and by vertex shader in `GET /vertices` (read by `tools/wiiuport/vertex_census.py`); `POST /blends?vertexShaderOff=<hash>` draws the meshes one vertex shader reads as the title drew them, counted `excluded`, to attribute an artefact to a shader. |
| Running a frame's independent pieces of work on worker threads | `src/wiiuport/interp/SlotPool.h` | Workers claim slots in order; a reader waits for the one slot it needs (`waitFor`) or the run (`waitAll`). Blends `VertexBlend`'s meshes. |
| One frame's draws, indexed by identity | `src/wiiuport/interp/KeyedFrame.h` | Built draw by draw, then looked up by key or by sourced block address, or searched for a shader's draw nearest some values. |
| Finding the draw nearest some values | `src/wiiuport/interp/DrawTree.h` | A k-d tree per shader over one frame's draws, built as the frame ends; exact nearest search with a limit and a starting draw. Pure. |
| Planning a kept snapshot again, offline | `src/wiiuport/interp/PlanReplay.h`, `tools/cxx/plan_replay.cpp`, `tools/plan_replay.py`, `tools/wiiuport/planreplay.py` | Feeds a `WIIUREC2` snapshot (`RecordingSnapshot::parse`) through the shipping planner and reports its outcomes, searches, time and the objects left at N, overall, by shader and by frame, so planner versions are compared on the same frames. Maintainer tool. |
| A recorded draw's identity | `src/wiiuport/interp/AssemblyKey.h` | Shader, sourced block addresses, and occurrence among identical draws. Pure. |
| Rewriting a replayed draw's uniforms | `src/wiiuport/interp/ReplayBlend.h` | The one assembly filter: object blend first, then the camera's substitution. |
| Keeping consecutive frames for offline study | `src/wiiuport/frame/RecordingSnapshot.h` | Armed over the channel; copies the next K frames' assemblies and frames them as `WIIUREC2`, and reads them back (`parse`). `tools/wiiuport/interpolation.py` parses them too, for the Python tools. |
| Sequencing one interpolated frame | `src/wiiuport/interp/FrameInterpolator.h` | The only place that knows an interpolated frame is a view, a blend and a replay together. Arms; never presents. Disarms the blend at the frame end after its replay. |
| Runtime counters and their denominators | `src/wiiuport/evidence/` | Frames recorded/replayed, bailouts by reason, substituted slots. Consumed by gates, not by prose. |
| Control channel (counters now; input injection and frame stepping next) | `src/wiiuport/control/ControlChannel.h` | Routes only, over `lucent::http::Server`. Off unless `WIIUPORT_CONTROL_PORT` is set, loopback only. `GET /counters` answers with zeros rather than nothing, so an idle runtime is distinguishable from a broken one. This is how a tool asks a running product what it is doing; reading its log afterwards is not a substitute. |
| Host shell: window, lifecycle, renderer bring-up, title launch | `src/wiiuport/shell/ShellHost.h`, `ShellWindow.h` | The product's front end, and the only implementation of the fork's `WindowSystem` seam in this build (`shell/WindowSystemBridge.cpp`). Composes; the emulated system is brought up by the fork's `Boot/SystemBringup`. |
| Where the player's settings, saves and NAND live | `src/wiiuport/shell/HostPaths.h` | The shell's one owner of environment-derived paths, published to `ActiveSettings` before any configuration is read. Portable mode is a `portable` directory beside the executable. |
| The first-run setup screen | `src/wiiuport/shell/SetupScreen.h` | This product's side of `shared/setup-ui`: what the screen says, what the platform picker offers, and the SDL3 file dialog it opens. Presentation and the staged-file state machine belong to setup-ui; what counts as a title belongs to the validator. Reports itself through `control::SetupStatusSource`. |
| The release runtime | `tools/wiiuport/release.py`, `runtime_bundle.py`; entries `tools/build_release_runtime.py`, `tools/stage_runtime.py` | A clean copy of the committed checkout (submodules cloned from the local ones) is built in a pinned Ubuntu 24.04 container at `/build` and staged as `build/release/bundle`: the stripped executable and its data, the libraries a desktop lacks, and `runtime.json` with the glibc floor. The image installs `hostdeps`' apt names. A title project packages the bundle. |
| A title project's product | `src/wiiuport/shell/RunProduct.h`, `ProductCommandLine.h` (target `wiiuport::host`) | `runProduct(Product, argc, argv)` is a product's whole entry point; `Product` fixes the product's name (window and setup screen) and the title; `--product-name` and `--title-id` fix the same on the generic runtime. `ProductCommandLine` reads the command line for it (pure, `tests/cxx/product_tests.cpp`). `shell/Main.cpp` is the maintainer's runtime and `tests/consumer/FixedTitleProduct.cpp` a title's, built to keep the interface honest. |
| The one title a consuming product runs | `src/wiiuport/shell/TitleIdentity.h` | Parses `--title-id` and words the refusal of a disc holding another title; `ShellHost::titleProblem` applies it to the disc's own metadata. Pure, covered by `tests/cxx/selection_tests.cpp`. |
| Which title this product runs, between launches | `src/wiiuport/shell/TitleSelection.h` | The remembered answer and why it is unusable when it is. Free of the emulated system on purpose -- the host injects the validator -- so it is covered by `tests/cxx/selection_tests.cpp` without one. |
| Attaching a physical gamepad, and swapping it | `src/wiiuport/shell/ControllerAutoMap.h` | Front end, not library: it owns SDL's event queue and drives `InputManager`. The binding table is by position and lives in one place. Reports itself through `control::ControllerStatusSource`. |
| Headless/offscreen/silent run mode | `src/wiiuport/host/` | Keeps maintainer runs off the desktop and off the real audio device. |
| Logging | `lucent` (`external/lucent`, pinned) | The only output boundary. No `printf`/`std::cerr` in first-party modules. |
| Environment reads | `lucent::config`, prefix `WIIUPORT_` | The single owner. No first-party module calls `getenv`. |
| Blend maths for one 3x4 transform | `src/wiiuport/interp/Transform3x4.h` | Rotation slerped, translation lerped. Knows nothing about cameras, actors, or where the floats came from. |
| First-party C++ tests | `tests/cxx/` | A harness that prints its own check count, so a suite that ran nothing fails. Suites are declared in `tests/cxx/suites.h`. |
| Building and running those tests as a gate | `tools/wiiuport/cxxtests.py` | Configures with Clang, reads the compiler back out of the cache, and scores the run by the checks it reports rather than by exit status alone. |
| Linting every first-party unit as a gate | `tools/wiiuport/cxxtidy.py` | Runs `clang-tidy` (`.clang-tidy`) over each first-party unit from the database of the build that compiles it, library or fork; refuses a missing database or an unreported check group. |
| Holding the title between frames | `src/wiiuport/frame/FrameGate.h` | Pauses at a frame's end, steps N frames and resumes, for `POST /gate`; runs a diagnostic's job on the held rendering thread (`runWhileHeld`); the last frame-shown listener, released by `ShellHost::shutdown`. Covered by `tests/cxx/gate_tests.cpp` on a thread standing in for the renderer. |
| Checking an in-between frame leaves guest memory alone | `src/wiiuport/interp/ShadowCheck.h`, `src/wiiuport/frame/GuestMemorySnapshot.h`, `tools/shadow_check.py` | On a held frame, copies every mapped guest span (`LatteFrameHooks::MappedGuestMemory`), draws an in-between frame through the continuous interpolator, and compares page by page against control windows that draw nothing, for `POST /shadowcheck`. Covered by `tests/cxx/shadow_tests.cpp` and `tests/test_shadow.py`. |
| Frame times at the display | `src/wiiuport/frame/PresentPacing.h` | Times every present the renderer returns from, the title's and the runtime's, and reports percentiles and the two halves of a tick since the last restart. One instance is fed by `OnDisplayed`; a second by `OnShown` (`RecordingObserver::ScanOutListener`), the times images reached the screen as the fork's `PresentTimingVk` reads them through `VK_EXT_present_timing`, counting an interval only between times on one clock. |
| Measuring continuous interpolation on the real title | `tools/continuous_run.py`, `tools/wiiuport/paired.py` | Plays offscreen on the GPU, reads `GET /interpolation` around a walk, takes an object census, keeps a snapshot, compares the title's rate with interpolation on and off in alternating windows of one walk, checks the restore byte for byte against its control, and fails when nothing was interpolated, the restore was not exact, or the run rendered in software. |
| Keeping one offscreen runtime up to drive and probe live | `tools/session.py`, `tools/wiiuport/gameplay.py` | Boots headless with the staged save, presses into the world (the one front-end drive every tool shares), prints the port and PID, and waits; everything else is done over the control channel while it runs. |
| Build orchestration, provisioning, verification | `tools/` (Python) | One locked environment via `uv run --frozen`. |
| User-supplied inputs a maintainer run needs (disc image, keys, save) | `tools/wiiuport/title.py` resolves, `tools/wiiuport/headless.py` stages | One resolver per input, each refusing a named-but-missing path rather than running without it. A save is copied into the session's own mlc, never linked: a driven run writes to its save as it plays. |
| Native development packages a build needs | `tools/wiiuport/hostdeps.py` | The one list, and the refusal that names them. Never duplicated into a script or a setup doc; `docs/dev-container.md` points at it. |
| Where those packages are installed | `docs/dev-container.md` | A Fedora toolbox sharing the same home, so installs need no host privileges. Not a build boundary: the build is the same inside and out. |

## Where the guest's render state enters

Two paths. Both are live in a real title -- Wind Waker HD uses `GX2SetVertexUniformReg`
70,752 times and `GX2SetVertexUniformBlock` 20,529 times in a two-minute run -- so
anything that handles only one of them substitutes a fraction of the submitted state:

- **Uniform registers** — ALU constant registers, set by `IT_SET_ALU_CONST` with the
  values carried inline in the display list.
- **Uniform blocks** — resource registers pointing at guest memory.

Substitution therefore happens at `VulkanRenderer::uniformData_updateUniformVars`,
where both paths have already converged on one assembled buffer. That site is chosen
because it covers both modes in one place and never writes guest memory; the mechanism
and its null-interpolation gate are in `docs/frame-interpolation.md`.

Which slots carry a camera or an actor is a title question, answered in that title's
project, not here.

## How first-party C++ reaches the runtime

The product is the fork's executable, so first-party code has to be linked into it without
becoming part of it. The fork stays buildable on its own, which is what upstream CI and any
clean checkout of it do.

- The fork owns a **hook registry** with no-op defaults: the observation and substitution
  points, and nothing else. A build with no first-party library linked behaves exactly as
  upstream does.
- `src/wiiuport/` builds as a static library and is added by the fork's CMake only when
  `WIIUPORT_SOURCE_DIR` is passed, which `tools/wiiuport/build.py` supplies. Absent it, the
  variable is unset and no subdirectory is added.
- `cmake/ReproduciblePaths.cmake` is passed as `CMAKE_PROJECT_INCLUDE` by the same builder, so
  every project in the build records compiled paths relative to the checkout
  (`-ffile-prefix-map`); a package does not name the directory it was built in.
- Registration happens once at startup, through `extern "C" void wiiuport_install_hooks()`
  called from `main`. It is not a static initialiser: the library is static, and a linker
  drops an object file nothing references, taking the initialiser with it. That failure
  would be silent -- a build that records nothing and reports no error. The fork names one
  C symbol and no first-party type, and the library never edits fork state directly.

This is the reason recording, blending, and policy are listed above as `src/wiiuport/` and
not as fork changes, even though the capture instruments currently live in the fork. Those are
reverse-engineering instruments that answer a question and are removed once answered; they are
not the product mechanism and must not grow into it.

