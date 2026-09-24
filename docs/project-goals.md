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

## GOAL-INTERP — A title-neutral render-state interpolation mechanism

**Outcome.** The runtime can present N intermediate frames between two guest simulation
ticks by re-issuing a recorded frame's draw stream with substituted transform state,
where the substituted values are produced by a consumer-supplied blend over the previous
and current tick's state. The mechanism owns recording, replay, shadowing of guest
uniform storage, and presentation pacing. It does not know what any particular value
means.

**Why.** Wii U titles commonly lock simulation to 30 Hz. Raising presentation rate by
interpolating the transforms the game already submits preserves simulation semantics,
unlike patching the game's tick rate.

**Success conditions.**
- A frame's draw stream can be replayed, with the blend the identity at t=1, to output no
  further from the title's frame than the renderer's own replay of that stream is from
  itself (the null-interpolation discriminator; the renderer does not draw the same
  commands identically twice, so byte-identical is not a bar any replay can meet).
- Substituted transform storage never writes back into guest memory.
- The runtime reports, with denominators: frames recorded, frames replayed, replay
  bailouts by reason, and substituted transform slots per frame.

**Constraints.** Interpolation is deterministic and source-state-driven. It never
inspects rendered pixels, infers motion from image content, or samples adjacent frames
to decide geometry. Blending only ever combines matching source state with explicit
provenance.

**Non-goals.** Image-space frame generation, optical flow, or an external presentation
layer. Changing the guest's simulation rate.

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
