# wiiuport — project goals

Epic-level intent only. Capability status lives in `docs/project-state.md`.

## GOAL-RUNTIME — A maintained, source-built Wii U runtime for Linux desktop

**Outcome.** A Wii U guest runtime built from our own pinned fork of Cemu
(`external/cemu` -> `SomeoneIsWorking/Cemu`, upstream `cemu-project/Cemu`, MPL-2.0),
buildable from a clean checkout with the host toolchain, and usable as a library by a
consuming title project rather than only as a standalone application.

**Why.** Consuming titles (`wiiu/setsail`) need to modify guest-visible render state at
runtime. That requires source-level access to the Latte command processor and the
renderer, which a prebuilt emulator binary cannot give.

**Success conditions.**
- The pinned fork configures and builds with Clang + Ninja from a clean tree.
- The runtime is exposed to a consuming project through a narrow C++ interface, not by
  forking the application entry point per title.
- Fork changes exist as reviewable commits on the fork, never as tracked patch files.

**Constraints.** Upstream Cemu is MPL-2.0; fork changes stay upstreamable and
per-cause. No game files, keys, or derived title data enter this repository.

**Non-goals.** Reimplementing a Wii U emulator from scratch. Owning any title's
addresses, identity, transform layout, or gameplay policy.

## GOAL-INTERP — A title presents at its display's rate, with its own logic rate

**Outcome.** The runtime presents N intermediate frames between two guest simulation
ticks, and the intermediate frames are drawn by the title's own render path at a
substituted pose rather than by the host re-issuing what the title submitted. The
mechanism owns the substitution, the pacing, and the evidence; it does not know what
any particular value means. Two routes exist and one is being retired: a title-side
mod of the display thread's paint path, and a host-side replay of a recorded draw
stream.

**Why.** Wii U titles commonly lock simulation to 30 Hz. Raising the *presentation*
rate preserves simulation semantics, unlike patching the game's tick rate. And a frame
the title draws itself carries the title's own identity and the title's own pose, so
nothing has to be guessed about which submitted value is which object's transform.

**Success conditions.**
- The title presents at the display's rate with its logic rate unchanged, measured on
  the real title in a headless driven run, with denominators: paints per second, logic
  ticks per second, and two consecutive paints compared byte by byte.
- An intermediate frame is the blend of the previous and current tick's pose, nearer
  each than they are to each other, and differs from both — with the null case measured
  too: two paints with no substitution are byte-identical.
- Nothing is inferred from rendered pixels, and geometry is never sampled from an
  adjacent frame.
- The runtime reports, with denominators: paints, logic ticks, per-binding block
  descriptors, and every refusal by cause.

**Constraints.** Interpolation is deterministic and source-state-driven. Blending only
ever combines matching source state with explicit provenance. The title's own disc
image is never modified, copied or committed: any change is applied in memory, as the
title's modules are linked.

**Non-goals.** Image-space frame generation, optical flow, or an external presentation
layer. **Changing the guest's simulation rate** — which is not what this does: the logic
keeps its own rate, and the picture's rate comes from the flip. What it does change, and
says so here rather than leaving it to be discovered, is the complete list of what a
stand-in in a display path touches:

- one word of a vtable the display thread already calls, so the display thread paints
  through the stand-in;
- **the first word of the frame being stood in for**, replaced by a relative branch into
  a stub this runtime allocated, with the title's own instruction preserved inside that
  stub and executed there before the frame resumes. A change to the guest's *code*, not
  to a pointer to it, and therefore the one that needs the recompiler told;
- **executable memory in the loader's trampoline area**, through the loader's own
  allocator. That area's base is the HLE function registry's code, then its symbol
  names, then zero padding, so the capability has to say where in the area it is safe to
  place code, and a consumer that does not is one branch away from executing data;
- the title's own record of the interval it asked for;
- and the emulator's flip pacing.

The logic-path gate this runtime also offers is a **backstop, not a finding**: measured on
the real title, the logic keeps 30.12 a second while the picture reaches 60, so the logic
is not slaved to the flip on this title. Which of these a given title needs is the title
project's decision, and the title-neutral capability that offers them says what each one
is — including, for the third, that the loader's arena is not uniformly executable.

## GOAL-DRIVE — The runtime is drivable and measurable by an agent

**Outcome.** An opt-in, off-by-default control channel (`lucent::http::Server`) that lets
a maintainer tool inject input, step and pace frames, read runtime counters, and capture
frames offscreen and silent, without a window, an audio device, or the desktop focus.

**Why.** Interpolation correctness cannot be established from logs. It needs driven,
repeatable runs that compare real captured frames against stated expectations.

**Success conditions.**
- A headless maintainer run reaches gameplay, captures frames, and reports counters
  without opening a window or a real audio device.
- The channel is absent from the default product path.

**Non-goals.** Shipping a remote-control feature to players.

## GOAL-PLATFORM — Hosted CI for every claimed host

**Outcome.** A hosted job per claimed host that configures, builds, lints, and tests the
runtime and its synthetic interpolation fixtures on that host.

**The claimed host is Linux, and only Linux.** The first consumer ships a Linux
product, the interpolation work substitutes into the Vulkan backend, and the offscreen
evidence path is built on Linux graphics. Windows and macOS are therefore not claimed
and deliberately have no job: a job that ran only the Python gates on those runners
would report green for a platform nothing has ever been built or run on, which is worse
than an honest absence. They become goals when something actually targets them, and
they get a real build job in the same change.

**Success conditions.**
- The Linux job builds the pinned fork through the same Python owners a maintainer
  uses, rather than duplicating build policy in workflow YAML.
- A cold job is correct: caches only make it faster, never make it pass.
- Third-party actions are pinned to full commit SHAs, permissions are least-privilege,
  and every job has an explicit timeout.

**Constraints.** Real-title conformance stays local: no game files, keys, or derived
title data in CI, secrets, commits, or packages. A green CI run proves the runtime
builds and the gates hold; it never proves anything about this or any title.
