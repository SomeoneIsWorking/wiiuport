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
| Fork interposition hooks | `external/cemu`, minimal and per-cause | The fork exposes callbacks at the frame boundary and at `LatteCP_itIndirectBuffer`, each draw's vertex buffers and attributes with the vertices it reads by index (`OnDrawPrepared`), and `GuestCallProbes` (`Cafe/HW/Espresso`), which branches a registered function of the title's to a stub that reports each call's registers and link register, runs the instruction it displaced and resumes the function, touching no register, installed once the title is linked; it does not implement recording, blending, or policy. |
| Process-lifetime ownership of first-party state | `src/wiiuport/Runtime.h` | One object, installed once. The hook registry holds a raw pointer, so what it points at outlives every frame. |
| Frame capture of the guest draw stream | `src/wiiuport/frame/` | Records the command buffers the title's queue submits, by copy, because the guest reuses the storage. Buffers referenced from inside them are counted, not recorded: replaying the outer one walks into them. |
| Driving the gamepad | `src/wiiuport/input/InputDriver.h` | Supplies presses and stick positions to the emulated gamepad through the fork's one input boundary. Declines the player when nothing is queued, so an unused build behaves as upstream. |
| What each published frame held | `src/wiiuport/frame/FrameShapeLog.h` | A window of recent frame shapes -- lists, assemblies, distinct shaders, bytes -- kept as shapes and never as frames. One frame's totals cannot show whether the recorder publishes whole frames; a run of them can. |
| The title-neutral guest values | `src/wiiuport/interp/` | `Blendable.h` is the one predicate for a value that can be lerped and the one midpoint; `Affine.h` is the 3x4 transform (product, inverse, midpoint). |
| Which draws read vertex bytes the title rewrote | `src/wiiuport/frame/VertexChanges.h`, `tools/wiiuport/draws.py` | Each of the title's draws' vertex bytes, from the fork's `OnDrawPrepared`, hashed and matched against the bytes its vertex shader read the frame before; draws without vertex uniforms always, the rest over a census (`POST /draws?frames=K`). Owned by `RecordingObserver`, read with `GET /draws`. |
| Runtime counters and their denominators | `src/wiiuport/evidence/` | Frames recorded/replayed, bailouts by reason, substituted slots. Consumed by gates, not by prose. |
| The in-between picture's inputs | `src/wiiuport/title/DrawInterpolation.h` | Probes `fpcM_Management`, `camera_draw`, `fopAc_Draw`, the model view pass, and BeforeOfDraw and AfterOfDraw. While the logic gate is in, the tick's draw phase sees each camera's eye, center, up, fovy and bank, and each executed actor's `current.pos` and `shape_angle`, at the midpoint of the previous tick's and this tick's, and each model's joint world matrices at the midpoint pose (`0x027f55fc`); all are put back after the draw phase. `GET`/`POST /interpolation` (`on=0` for comparison); covered by `tests/cxx/draw_interpolation_tests.cpp`. |
| Particles in the in-between paint | `src/wiiuport/title/ParticleInterpolation.h` | Probes `JPADraw::draw` (`0x0282bec8`). In the paint after a gated draw phase (`DrawInterpolation::MidPaintListener`) it sets each continuing particle's `mGlobalPosition` and `JPADrawParams` (scale, colour, rotation) to the midpoint of the last paint's and now before the emitter draws, and restores it at the next BeforeOfDraw. Reported on `GET /particles`; covered by `tests/cxx/particle_interpolation_tests.cpp`. |
| Sea waves and sky clouds in the in-between paint | `src/wiiuport/title/EnvironmentInterpolation.h` | A `DrawInterpolation::MidPaintListener`: when the in-between paint opens it sets each continuing wave's and cloud's position and alpha (and a wave's counter) in the environment's two packets to the midpoint of the last two ticks', and restores them when it closes. Reported on `GET /environment`; covered by `tests/cxx/environment_interpolation_tests.cpp`. |
| Material animation in the in-between draw phase | `src/wiiuport/title/MaterialInterpolation.h` | A `DrawInterpolation::DrawPhaseListener`: probes the bpk, btk and brk entries and `mDoExt_baseAnm::play`; at each actor's gated draw it sets the frame of each animation that actor's draw entered to the midpoint of its last two ticks', and steps one played in the draw once a tick. Reported on `GET /materials`; covered by `tests/cxx/material_interpolation_tests.cpp`. |
| The sea surface in the in-between draw phase | `src/wiiuport/title/SeaInterpolation.h` | A `DrawInterpolation::DrawPhaseListener`: for the gated draw phase it sets the sea grid's corner and heights to the midpoint of the last two ticks' and holds `daSea_Draw`'s texture scroll counter to one step a tick. Reported on `GET /sea`; covered by `tests/cxx/sea_interpolation_tests.cpp`. |
| Reverse-engineering instruments on the running title | `src/wiiuport/guest/CallerCensus.h`, `src/wiiuport/control/GuestMemoryRead.h` | `CallerCensus` probes up to 8 title functions named by `WIIUPORT_CALLER_CENSUS` (`entry:firstInstruction` in hex, comma separated) and counts the call sites and guest call chains (return address, then each frame's saved link register up the back chain) that reach each (`GET /callers`): the answer for a function reached only through a pointer. `GuestMemoryRead` answers `GET /memory?address=<hex>&size=<decimal>` (at most 16 MiB) with the guest's bytes, refusing a range that is not all guest memory. Both take the fork's calls injected; covered by `tests/cxx/control_tests.cpp`. |
| Control channel (counters now; input injection and frame stepping next) | `src/wiiuport/control/ControlChannel.h` | Routes only, over `lucent::http::Server`. Always open on loopback port 21337; `WIIUPORT_CONTROL_PORT` moves it. `GET /counters` answers with zeros rather than nothing, so an idle runtime is distinguishable from a broken one. This is how a tool asks a running product what it is doing; reading its log afterwards is not a substitute. |
| Launcher | `run.sh` -> `bootstrap.py` | A shim into the locked Python environment; `bootstrap.py` builds through `tools/wiiuport/build.py`'s `build_product`, the maintainers' sequence, and execs the runtime. It never runs tests or checks. |
| Host shell: window, lifecycle, renderer bring-up, title launch | `src/wiiuport/shell/ShellHost.h`, `ShellWindow.h` | The product's front end, and the only implementation of the fork's `WindowSystem` seam in this build (`shell/WindowSystemBridge.cpp`). Composes; the emulated system is brought up by the fork's `Boot/SystemBringup`, and the process ends through its `SystemBringup::Exit` (`shell/RunProduct.cpp`). `POST /quit` reaches it through `control::HostStopTarget` (`control/HostStop.h`) while a title runs. |
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
| First-party C++ tests | `tests/cxx/` | A harness that prints its own check count, so a suite that ran nothing fails. Suites are declared in `tests/cxx/suites.h`. |
| Building and running those tests as a gate | `tools/wiiuport/cxxtests.py` | Configures with Clang, reads the compiler back out of the cache, and scores the run by the checks it reports rather than by exit status alone. |
| Linting every first-party unit as a gate | `tools/wiiuport/cxxtidy.py` | Runs the `clang-tidy` pinned in `uv.lock` (found by `tools/wiiuport/lockedtools.py`, as is `clang-format`) with `.clang-tidy` over each first-party unit from the database of the build that compiles it, library or fork; refuses a missing database or an unreported check group. |
| Holding the title between frames | `src/wiiuport/frame/FrameGate.h` | Pauses at a frame's end, steps N frames and resumes, for `POST /gate`; runs a diagnostic's job on the held rendering thread (`runWhileHeld`); the last frame-shown listener, released by `ShellHost::shutdown`. Covered by `tests/cxx/gate_tests.cpp` on a thread standing in for the renderer. |
| Frame times at the display | `src/wiiuport/frame/PresentPacing.h` | Times every present the renderer returns from, the title's and the runtime's, and reports percentiles and the two halves of a tick since the last restart. One instance is fed by `OnDisplayed`; a second by `OnShown` (`RecordingObserver::ScanOutListener`), the times images reached the screen as the fork's `PresentTimingVk` reads them through `VK_EXT_present_timing`, counting an interval only between times on one clock. |
| Keeping one offscreen runtime up to drive and probe live | `tools/session.py`, `tools/wiiuport/gameplay.py` | Boots headless with the staged save, presses into the world (the one front-end drive every tool shares), prints the port and PID, and waits; everything else is done over the control channel while it runs. |
| Build orchestration, provisioning, verification | `tools/` (Python) | One locked environment via `uv run --frozen`. |
| Reverse engineering the title's executable | `src/wiiuport/maint/TitleFiles.cpp` (`wiiuport_title_files`), `tools/rpx_to_elf.py`, `tools/wiiuport/rpx.py`, `rpx_link.py` | Lists or extracts a file of the title's own volume (such as `code/cking.rpx`) through the core that mounts titles; the RPX is inflated section by section into a plain big-endian ELF at its load addresses, its import stubs placed after the code and every relocation applied (those against its own sections must reproduce the prelinked bytes), which Ghidra imports as `PowerPC:BE:32:Gekko_Broadway` into `build/ghidra/`. Both outputs stay under the ignored `build/`. Maintainer tools. |
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


## The title's own paint path

| Responsibility | Owner | Notes |
|---|---|---|
| Writing the guest's own memory | `external/cemu/src/Cafe/HW/Espresso/GuestPatching.{h,cpp}` | Fork: `AllocateCode` for guest memory the guest may execute from, out of the loader's trampoline area; `ReadWord`/`WriteWord` carrying a value in the guest's big-endian order, because a word crossing the boundary is a value and not a copy; `WriteBytes` for a caller that already holds that order. A range that is not mapped is refused whole, and what is written is invalidated in the recompiler. Inert with nothing calling it. |
| Wind Waker HD's display thread painting twice | `src/wiiuport/title/WindWakerPaint.h` | A stand-in for the display frame, written into guest code space and reached by rewriting one word of the display vtable, so the thread paints each tick's world twice and asks for one vblank a flip. The stand-in is the title's own loop body, so the frame is re-read from the title's vtable on each pass. The vtable it patches is the one the *running* display holds, read from the display pointer a probe hands over on every paint -- the address out of the image is a check, not the source, since patching the wrong vtable rewrites a word of an object whose slot `0xcc` may mean something else. Refuses by name over a frame slot or a loop that is not this title's, and over a display thread that has not painted yet. Its memory is reserved at startup, so enabling it is one word written. Counts paints, reports the display object's own fields, and separates the redirect from the second paint from the interval so a failure says which. |
| Measuring it | `tools/paint_run.py`, `tools/wiiuport/control.py` | Alternating off/on/off windows in one run, the off windows being the control: the display's paint rate from a probe on the frame, the logic's rate from the caller census on `fapGm_Execute`, which the flip does not move. Fails when the on window did not paint more than its neighbours, when the off windows were too quiet to be a control, or when the logic rate left 28.5-31.5 Hz. |

**Where the boundary is now, honestly.** This codemap's dependency direction says `wiiuport`
never contains a title's addresses, and `src/wiiuport/title/WindWakerPaint.h` does: the
display vtable, the frame it holds, the thread entry it returns to, the `gx2` import it calls,
and every word of the stand-in are Wind Waker HD's, read out of its executable. That is a
deliberate move and not an accident of placement:

- The mechanism (`GuestPatching`) is title-neutral and belongs here. The title's addresses and
  payload are the title's, and belong to the title project.
- They are here because `setsail` has no C++ build of its own: it provisions this runtime and
  launches it. Giving the title project a compiled payload means it compiles C++, which is a
  larger change than the mod is, and until then the alternative is a mod that cannot be built
  or measured at all.
- So the split is by concept, not by repository: `guest/` and the fork own *how* to change the
  guest, `title/` owns *what* to change in this title, and the day the title project compiles,
  `title/` moves across wholesale rather than being reimplemented.
