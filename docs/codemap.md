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
| Frame capture of the guest draw stream | `src/wiiuport/frame/` | Records the `IT_INDIRECT_BUFFER` display lists a frame references, by copy, because the guest reuses the storage. |
| Frame replay with substituted state | `src/wiiuport/frame/` | Re-feeds recorded buffers through the fork's existing command-buffer entry point. Substitutions are applied to the recorded copy, never to guest memory. |
| Transform substitution interface | `src/wiiuport/interp/` | Title-neutral: a consumer registers which recorded dwords are transform state and supplies the blend. The runtime does not know what a camera is. |
| Runtime counters and their denominators | `src/wiiuport/evidence/` | Frames recorded/replayed, bailouts by reason, substituted slots. Consumed by gates, not by prose. |
| Control channel (input injection, frame stepping, counter and capture endpoints) | `src/wiiuport/control/` | Routes only. The server itself is `lucent::http::Server`; no socket, parsing, or dispatch code is written here. |
| Headless/offscreen/silent run mode | `src/wiiuport/host/` | Keeps maintainer runs off the desktop and off the real audio device. |
| Configuration and environment reads | `src/wiiuport/config/` | The single owner. No other module calls `getenv`. |
| Logging | `lucent` | The only output boundary. No `printf`/`std::cerr` in first-party modules. |
| Build orchestration, provisioning, verification | `tools/` (Python) | One locked environment via `uv run --frozen`. |

## Where the guest's render state enters

Two paths, both observable inside a recorded display list:

- **Uniform registers** — `IT_SET_ALU_CONST` carries its values inline in the display
  list. Substituting these means patching dwords in the recorded copy; no shadow storage
  and no guest-memory writeback is needed.
- **Uniform blocks** — bound by resource registers that point at guest memory. These
  need a shadow buffer holding the blended values, with the binding redirected for the
  replay only.

Which path a given title uses is a title question and is answered in that title's
project, not here.
