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
| Frame capture of the guest draw stream | `src/wiiuport/frame/` | Records the `IT_INDIRECT_BUFFER` display lists a frame references, by copy, because the guest reuses the storage. |
| Frame replay with substituted state | `src/wiiuport/frame/` | Re-feeds recorded buffers through the fork's existing command-buffer entry point. Substitutions are applied to the recorded copy, never to guest memory. |
| Transform substitution interface | `src/wiiuport/interp/` | Title-neutral: a consumer registers which recorded dwords are transform state and supplies the blend. The runtime does not know what a camera is. |
| Runtime counters and their denominators | `src/wiiuport/evidence/` | Frames recorded/replayed, bailouts by reason, substituted slots. Consumed by gates, not by prose. |
| Control channel (counters now; input injection and frame stepping next) | `src/wiiuport/control/ControlChannel.h` | Routes only, over `lucent::http::Server`. Off unless `WIIUPORT_CONTROL_PORT` is set, loopback only. `GET /counters` answers with zeros rather than nothing, so an idle runtime is distinguishable from a broken one. This is how a tool asks a running product what it is doing; reading its log afterwards is not a substitute. |
| Headless/offscreen/silent run mode | `src/wiiuport/host/` | Keeps maintainer runs off the desktop and off the real audio device. |
| Logging | `lucent` (`external/lucent`, pinned) | The only output boundary. No `printf`/`std::cerr` in first-party modules. |
| Environment reads | `lucent::config`, prefix `WIIUPORT_` | The single owner. No first-party module calls `getenv`. |
| Blend maths for one 3x4 transform | `src/wiiuport/interp/Transform3x4.h` | Rotation slerped, translation lerped. Knows nothing about cameras, actors, or where the floats came from. |
| First-party C++ tests | `tests/cxx/` | A harness that prints its own check count, so a suite that ran nothing fails. Suites are declared in `tests/cxx/suites.h`. |
| Building and running those tests as a gate | `tools/wiiuport/cxxtests.py` | Configures with Clang, reads the compiler back out of the cache, and scores the run by the checks it reports rather than by exit status alone. |
| Build orchestration, provisioning, verification | `tools/` (Python) | One locked environment via `uv run --frozen`. |
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
- Registration happens once at startup, through `extern "C" void wiiuport_install_hooks()`
  called from `main`. It is not a static initialiser: the library is static, and a linker
  drops an object file nothing references, taking the initialiser with it. That failure
  would be silent -- a build that records nothing and reports no error. The fork names one
  C symbol and no first-party type, and the library never edits fork state directly.

This is the reason recording, blending, and policy are listed above as `src/wiiuport/` and
not as fork changes, even though the capture instruments currently live in the fork. Those are
reverse-engineering instruments that answer a question and are removed once answered; they are
not the product mechanism and must not grow into it.

