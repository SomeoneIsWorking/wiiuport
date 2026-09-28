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
any particular value means. **One route exists: a mod of the display thread's own paint
path, so the title paints the tree itself at the lerped pose and there is no host-side
copy of the frame to keep in step with one.** The other route — a host-side replay of a
recorded draw stream, with the camera found by searching shaders and identity matched by
uniform-block address — was deleted with its evidence, not deferred.

**Why.** Wii U titles commonly lock simulation to 30 Hz. Raising the *presentation*
rate preserves simulation semantics, unlike patching the game's tick rate. And a frame
the title draws itself carries the title's own identity and the title's own pose, so
nothing has to be guessed about which submitted value is which object's transform.

**Success conditions.**
- The title presents at the display's rate with its logic rate unchanged, measured on
  the real title in a headless driven run, with denominators: paints per second, logic
  ticks per second, and two consecutive paints compared byte by byte.
- An intermediate frame is the blend of the previous and current tick's pose, nearer
  each than they are to each other, and differs from both — **and the null case is a
  discriminator, not a uniformity.** "Byte-identical" is not the right bar and is not
  what the measurement shows: two paints with nothing substituted differ in 0.85% of
  6,220,800 bytes with a largest delta of 3, against 32-35% with deltas of 164 and 221
  for a genuinely different frame. So the condition is that the null case is
  *indistinguishable from one paint on a stated measure*, and that the measure separates
  the two cases. A uniformity would pass whether the two paints were the same or not.
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
- **either the pointer to the frame, or the frame's own first word.** Two shapes are
  offered, and the difference matters because one of them faults: the stand-in can be
  reached by rewriting the one word of the vtable the display thread already calls
  through, or by writing a branch at the frame's own entry. The first changes a pointer
  to the frame; the second changes the guest's *code* and is the one that needs the
  recompiler told, and it is the one that does not run. The shape that paints carries
  the frame as a literal built from instructions lifted verbatim from the image, with
  the two address words checked by reconstructing the frame from them rather than by
  asserting two constants;
- **executable memory in the loader's trampoline area**, through the loader's own
  allocator. That area's base is the HLE function registry's code, then its symbol
  names, then zero padding, so the capability has to say where in the area it is safe to
  place code, and a consumer that does not is one branch away from executing data;
- the title's own record of the interval it asked for;
- and the emulator's flip pacing.

**The logic-path gate is a finding, not a backstop, and this corrects what this document
used to say.** It was written as "measured on the real title, the logic keeps its own rate
while the picture reaches 60, so the logic is not slaved to the flip on this title". Both
halves of that were read off runs in which the gate's probe had been refused, so the gate
counted nothing while the title carried on. **A gate that was never installed looks
exactly like a gate that was not needed.** With the probe installed, the tick is called
once per paint, one for one: 480 paints and 480 tick calls in an 8.00 s window, 59.99 a
second each, and the logic *does* follow the flip. The gate in the logic path is what
holds it at 30.00 a second. So the gate is offered as a capability and its own
installation state is reported next to its counts, because a zero that means "refused" and
a zero that means "not needed" are the same number and opposite findings.

Which of these a given title needs is the title project's decision, and the title-neutral
capability that offers them says what each one is — including, for the third, that the
loader's arena is not uniformly executable, and for the gate, that it is measured rather
than assumed.

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
