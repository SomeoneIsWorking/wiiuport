"""Client for the running product's control channel.

This is how a tool asks the runtime what it is doing while it runs, instead of
launching it and reading its log afterwards. A log-scraping loop can only ask
what the script already knew to ask, and cannot ask anything of a run in
progress.
"""

from __future__ import annotations

import http.client
import json
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

DEFAULT_PORT = 21337
"""The port the product listens on, loopback only, unless WIIUPORT_CONTROL_PORT
moves it (`ControlChannel::kDefaultPort`); `./run.sh` sessions answer here too."""


ENV_CONTROL_PORT = "WIIUPORT_CONTROL_PORT"
ENV_INTERPOLATION = "WIIUPORT_INTERPOLATION"


def runtime_env(port: int, *, continuous: bool = True) -> dict[str, str]:
    """The environment a driven run hands the product.

    Continuous interpolation is the product and stays on unless a tool needs a
    frame boundary of its own: a one-shot replay, null diff or single
    interpolated frame would otherwise compete with it for every boundary, and
    the runtime refuses them while it runs."""
    return {ENV_CONTROL_PORT: str(port), ENV_INTERPOLATION: "1" if continuous else "0"}


class ControlUnavailable(RuntimeError):
    """The channel did not answer, with the reason named."""


@dataclass(frozen=True)
class Counters:
    """What the runtime reports about its own work, with denominators."""

    framesObserved: int
    framesRefusedIncomplete: int
    displayListsSeen: int
    uniformAssembliesSeen: int
    lastFrameDisplayLists: int
    lastFrameUniformAssemblies: int
    lastFrameBytes: int
    replaysRun: int
    replayListsSubmitted: int
    replayListsRefused: int
    inputPollsSeen: int
    inputPollsAnswered: int
    inputPressesQueued: int
    capturesRequested: int
    capturesRefused: int
    imagesReceived: int
    displayListsFromRuntime: int
    uniformAssembliesFromRuntime: int
    presentsObserved: int
    presentsObservedTv: int
    presentsObservedDrc: int
    presentsSubmitted: int
    presentsRefusedUnobserved: int
    presentsRefusedBySubmit: int
    nullDiffsCompleted: int
    nestedListsSeen: int
    guestDrawsFromCommandBuffers: int
    guestDrawsFromRing: int
    guestDrawsPrepared: int
    guestDrawsWithoutVertexUniforms: int
    runtimeSubmissions: int
    runtimePacketsProcessed: int
    runtimeDrawsIssued: int
    interpolatedFramesArmed: int
    interpolatedFramesRefused: int
    assembliesOffered: int
    assembliesSubstituted: int
    assembliesUnarmed: int
    assembliesUnknownShader: int
    assembliesTooShort: int

    @property
    def recorded_anything(self) -> bool:
        return self.framesObserved > 0 and self.displayListsSeen > 0

    def render(self) -> str:
        return (
            f"frames {self.framesObserved} (refused incomplete "
            f"{self.framesRefusedIncomplete}), display lists {self.displayListsSeen}, "
            f"uniform assemblies {self.uniformAssembliesSeen} (of which the runtime's own "
            f"replays: {self.displayListsFromRuntime} lists, "
            f"{self.uniformAssembliesFromRuntime} assemblies); last frame held "
            f"{self.lastFrameDisplayLists} lists and "
            f"{self.lastFrameUniformAssemblies} assemblies in {self.lastFrameBytes} bytes; "
            f"nested lists {self.nestedListsSeen}; the title drew "
            f"{self.guestDrawsFromCommandBuffers} times from command buffers and "
            f"{self.guestDrawsFromRing} straight from the ring, "
            f"{self.guestDrawsWithoutVertexUniforms} of {self.guestDrawsPrepared} drawn "
            "with no vertex uniforms, which no blend moves; "
            f"replays {self.replaysRun} submitting {self.replayListsSubmitted} lists "
            f"({self.replayListsRefused} refused) in {self.runtimeSubmissions} submissions "
            f"the command processor walked {self.runtimePacketsProcessed} packets of, "
            f"issuing {self.runtimeDrawsIssued} draws; presents observed "
            f"{self.presentsObserved} ({self.presentsObservedTv} TV, "
            f"{self.presentsObservedDrc} GamePad), submitted {self.presentsSubmitted} "
            f"({self.presentsRefusedUnobserved} with nothing to send, "
            f"{self.presentsRefusedBySubmit} refused); null diffs "
            f"{self.nullDiffsCompleted}; interpolated frames "
            f"{self.interpolatedFramesArmed} armed ({self.interpolatedFramesRefused} refused) "
            f"substituting the view into {self.assembliesSubstituted} of "
            f"{self.assembliesOffered} replayed assemblies ({self.assembliesUnarmed} with "
            f"nothing armed, {self.assembliesUnknownShader} not carrying it, "
            f"{self.assembliesTooShort} too short)"
        )


@dataclass(frozen=True)
class TransformCandidate:
    """One 3x4 the runtime found, with what decides whether it is a view."""

    stageIndex: int
    baseHash: int
    auxHash: int
    floatOffset: int
    framesSeen: int
    shadersSharing: int
    rotationError: float
    meanTranslationStep: float
    values: tuple[float, ...]

    @property
    def is_shared(self) -> bool:
        return self.shadersSharing > 1

    def render(self) -> str:
        shared = (
            f"shared by {self.shadersSharing} shaders"
            if self.is_shared
            else "in this shader only, so not shown to be a view"
        )
        translation = (self.values[3], self.values[7], self.values[11])
        return (
            f"stage {self.stageIndex} shader {self.baseHash:016x}:{self.auxHash:016x} "
            f"float {self.floatOffset} (byte {self.floatOffset * 4}): {shared}, "
            f"rotation error {self.rotationError:.2e}, mean step "
            f"{self.meanTranslationStep:.1f} over {self.framesSeen} frames, at "
            f"({translation[0]:.1f}, {translation[1]:.1f}, {translation[2]:.1f})"
        )


@dataclass(frozen=True)
class TransformReport:
    """What the search found and, as importantly, what it looked at."""

    framesObserved: int
    shadersTracked: int
    spansExamined: int
    rejectedVaryingWithinFrame: int
    rejectedNeverChanging: int
    rejectedRotation: int
    candidatesFound: int
    sharedAndMoving: int
    shadersInLastFrame: int
    candidates: tuple[TransformCandidate, ...]

    def render(self) -> str:
        totals = (
            f"{self.candidatesFound} candidates ({self.sharedAndMoving} shared and moving) "
            f"from {self.spansExamined} spans examined across {self.shadersTracked} shaders "
            f"({self.shadersInLastFrame} of them drawing in the last frame) "
            f"over {self.framesObserved} frames"
        )
        rejected = (
            f"  rejected: {self.rejectedVaryingWithinFrame} varying within a frame, "
            f"{self.rejectedNeverChanging} never changing, "
            f"{self.rejectedRotation} not a rotation"
        )
        lines = [totals, rejected]
        if self.framesObserved < 2:
            lines.append(
                "  fewer than two frames were watched, so nothing is known to change and "
                "no candidate could be told from a constant"
            )
        elif self.candidatesFound == 0:
            lines.append(
                "  no slot holds a frame-constant 3x4 with a real rotation. Either the run "
                "never reached a drawn scene, or this title does not pass its view as a 3x4 "
                "in the assembled uniform buffer."
            )
        elif self.sharedAndMoving == 0:
            lines.append(
                "  every candidate appears in one shader only, so none is shown to be a "
                "view rather than that object's own transform"
            )
        lines.extend(f"  {candidate.render()}" for candidate in self.candidates)
        return "\n".join(lines)


# When set, every request is written to this file before it is sent and again with
# what came back. Two runs that differ only in the product's fate differ here
# first, and the difference is one line rather than a bisect through nine-minute
# runs. Off unless asked for, because a maintainer tool that narrates is a
# maintainer tool nobody reads.
TRACE_PATH: Path | None = None


def _trace(outgoing: str) -> None:
    if TRACE_PATH is None:
        return
    with TRACE_PATH.open("a", encoding="utf-8") as trace:
        trace.write(f"{time.monotonic():9.3f} {outgoing}\n")


def _unreachable(url: str, dropped: BaseException) -> ControlUnavailable:
    """A channel that stopped answering, in the words a tool can report.

    A refused connection and a connection dropped mid-request are the same fact
    for anything that polls the channel -- the runtime is not there any more --
    and they used to read differently: the first became ControlUnavailable and
    the second escaped as http.client's own exception, so a tool died on the very
    run whose failure it was meant to report. RemoteDisconnected is a subclass of
    both BadStatusLine (an HTTPException) and ConnectionResetError (an OSError);
    both are caught, and so is anything else the socket layer raises.
    """
    return ControlUnavailable(
        f"{url} did not answer ({type(dropped).__name__}: {dropped}). The runtime is not "
        "running, stopped while answering, or listens on another WIIUPORT_CONTROL_PORT."
    )


def _get(path: str, port: int, timeout: float) -> dict:
    url = f"http://127.0.0.1:{port}{path}"
    _trace(f"GET {path}")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            _trace(f"  <- {response.status}")
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.URLError as unreachable:
        raise _unreachable(url, unreachable.reason) from unreachable
    except (http.client.HTTPException, OSError) as dropped:
        raise _unreachable(url, dropped) from dropped
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )


def request_bytes(method: str, path: str, port: int, timeout: float) -> bytes:
    """One request to the channel, refusing by reason: a refusal carries the
    runtime's own explanation, and silence says the runtime is not there."""
    url = f"http://127.0.0.1:{port}{path}"
    request = urllib.request.Request(url, method=method, data=b"" if method == "POST" else None)
    _trace(f"{method} {path}")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            _trace(f"  <- {response.status}")
            return response.read()
    except urllib.error.HTTPError as refused:
        body = refused.read().decode("utf-8", "replace").strip()
        raise ControlUnavailable(f"{url} was refused ({refused.code}): {body}") from refused
    except urllib.error.URLError as unreachable:
        raise _unreachable(url, unreachable.reason) from unreachable
    except (http.client.HTTPException, OSError) as dropped:
        raise _unreachable(url, dropped) from dropped


def require_fields(url: str, payload: dict, fields: object, what: str) -> None:
    missing = set(fields) - set(payload)
    if missing:
        raise ControlUnavailable(
            f"{url} answered without {sorted(missing)}, so the runtime and this client "
            f"disagree about what {what} is"
        )


def wait_for_channel(port: int, seconds: int) -> bool:
    """Poll until the channel answers or `seconds` pass. False means the
    runtime never opened it, which a caller reports rather than driving a
    title nothing can observe."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        time.sleep(5)
        try:
            read_counters(port)
        except ControlUnavailable:
            continue
        return True
    return False


def wait_for[T](read: Callable[[], T], seconds: int) -> T:
    """What `read` returns once it stops refusing, polled each second. A title
    running slowly takes longer to reach what was asked of it, and that is
    itself worth seeing rather than a refusal after a fixed sleep."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return read()
        except ControlUnavailable:
            if time.monotonic() >= deadline:
                raise
            time.sleep(1)


def read_counters(port: int = DEFAULT_PORT, timeout: float = 2.0) -> Counters:
    """Read /counters, refusing by reason rather than returning empty."""
    payload = _get("/counters", port, timeout)
    require_fields(
        f"http://127.0.0.1:{port}/counters", payload, Counters.__annotations__, "a counter set"
    )
    return Counters(**{field: int(payload[field]) for field in Counters.__annotations__})


@dataclass(frozen=True)
class FrameShape:
    """What one published frame held."""

    frameIndex: int
    displayLists: int
    uniformAssemblies: int
    distinctShaders: int
    byteCount: int
    complete: bool

    def render(self) -> str:
        return (
            f"frame {self.frameIndex}: {self.displayLists} lists, "
            f"{self.uniformAssemblies} assemblies across {self.distinctShaders} shaders, "
            f"{self.byteCount} bytes" + ("" if self.complete else " (incomplete)")
        )


@dataclass(frozen=True)
class FrameWindow:
    """The last few published frames, oldest first."""

    framesLogged: int
    frames: tuple[FrameShape, ...]

    def render(self) -> str:
        if not self.frames:
            return f"{self.framesLogged} frames published, none held in the window"
        shaders = [shape.distinctShaders for shape in self.frames]
        headline = (
            f"{self.framesLogged} frames published; the last {len(self.frames)} held "
            f"{min(shaders)} to {max(shaders)} distinct shaders"
        )
        lines = [headline]
        lines += [f"  {shape.render()}" for shape in self.frames]
        return "\n".join(lines)


def read_frames(port: int = DEFAULT_PORT, timeout: float = 5.0) -> FrameWindow:
    """Read /frames, refusing by reason rather than returning empty."""
    url = f"http://127.0.0.1:{port}/frames"
    payload = _get("/frames", port, timeout)
    require_fields(url, payload, FrameWindow.__annotations__, "a frame window")
    shapes = []
    for entry in payload["frames"]:
        require_fields(url, entry, FrameShape.__annotations__, "a frame shape")
        shapes.append(FrameShape(**entry))
    return FrameWindow(framesLogged=int(payload["framesLogged"]), frames=tuple(shapes))


@dataclass(frozen=True)
class OfferedShader:
    """One shader the blend was armed for, or one a replayed draw used."""

    stageIndex: int
    baseHash: int
    auxHash: int
    floats: int
    times: int
    substituted: bool

    def render(self) -> str:
        where = f"float {self.floats} long" if self.times else f"view at float {self.floats}"
        seen = f", {self.times} draws" if self.times else ""
        return (
            f"stage {self.stageIndex} shader {self.baseHash:016x}:{self.auxHash:016x} "
            f"({where}{seen})"
        )


@dataclass(frozen=True)
class Substitution:
    """The two key sets an interpolated frame needs to agree on."""

    armed: bool
    blendPoint: float
    assembliesOffered: int
    assembliesSubstituted: int
    slots: tuple[OfferedShader, ...]
    offered: tuple[OfferedShader, ...]

    def render(self) -> str:
        headline = (
            f"armed {self.armed} at t={self.blendPoint}, {self.assembliesSubstituted} of "
            f"{self.assembliesOffered} replayed assemblies substituted"
        )
        lines = [headline, f"  armed for {len(self.slots)} shaders:"]
        lines += [f"    {slot.render()}" for slot in self.slots[:8]]
        lines.append(f"  the replay offered {len(self.offered)} distinct shaders:")
        lines += [f"    {shader.render()}" for shader in self.offered[:8]]
        if not self.offered:
            lines.append("    (none: no replayed draw reached a shader at all)")
        return "\n".join(lines)


def read_substitution(port: int = DEFAULT_PORT, timeout: float = 2.0) -> Substitution:
    """Read /substitution, refusing by reason rather than returning empty."""
    payload = _get("/substitution", port, timeout)
    url = f"http://127.0.0.1:{port}/substitution"
    require_fields(url, payload, Substitution.__annotations__, "a substitution report")
    return Substitution(
        armed=bool(payload["armed"]),
        blendPoint=float(payload["blendPoint"]),
        assembliesOffered=int(payload["assembliesOffered"]),
        assembliesSubstituted=int(payload["assembliesSubstituted"]),
        slots=tuple(OfferedShader(**shader) for shader in payload["slots"]),
        offered=tuple(OfferedShader(**shader) for shader in payload["offered"]),
    )


@dataclass(frozen=True)
class ControllerStatus:
    """What the host reports about the physical pad it has attached."""

    hostReports: bool
    attachedDevice: str
    devicesAttached: int
    devicesLost: int
    bindings: int

    def render(self) -> str:
        if not self.hostReports:
            return "no host reported controllers; this build attaches none"
        held = self.attachedDevice or "nothing"
        return (
            f"attached {held} with {self.bindings} bindings; "
            f"{self.devicesAttached} attached and {self.devicesLost} lost so far"
        )


def read_controllers(port: int = DEFAULT_PORT, timeout: float = 2.0) -> ControllerStatus:
    """Read /controllers, refusing by reason rather than returning empty."""
    payload = _get("/controllers", port, timeout)
    require_fields(
        f"http://127.0.0.1:{port}/controllers",
        payload,
        ControllerStatus.__annotations__,
        "a controller status",
    )
    return ControllerStatus(
        hostReports=bool(payload["hostReports"]),
        attachedDevice=str(payload["attachedDevice"]),
        devicesAttached=int(payload["devicesAttached"]),
        devicesLost=int(payload["devicesLost"]),
        bindings=int(payload["bindings"]),
    )


@dataclass(frozen=True)
class SetupStatus:
    """What the host reports about its first-run setup screen."""

    hostReports: bool
    shown: bool
    state: str
    selectionsOffered: int

    def render(self) -> str:
        if not self.hostReports:
            return "no host reported a setup screen; this run is past it or shows none"
        where = "on the display" if self.shown else "not shown"
        return (
            f"the setup screen is {where}, waiting at {self.state!r}, "
            f"handed {self.selectionsOffered} selections"
        )


def read_setup(port: int = DEFAULT_PORT, timeout: float = 2.0) -> SetupStatus:
    """Read /setup, refusing by reason rather than returning empty."""
    payload = _get("/setup", port, timeout)
    require_fields(
        f"http://127.0.0.1:{port}/setup",
        payload,
        SetupStatus.__annotations__,
        "a setup status",
    )
    return SetupStatus(
        hostReports=bool(payload["hostReports"]),
        shown=bool(payload["shown"]),
        state=str(payload["state"]),
        selectionsOffered=int(payload["selectionsOffered"]),
    )


def request_quit(port: int = DEFAULT_PORT, timeout: float = 5.0) -> None:
    """Ask the running product to stop as a player closing its window does.

    Refused with the runtime's reason while no title runs. A signal is no
    substitute: the emulated system's handler ends the process on SIGTERM
    without the host's shutdown, so only this exercises the path players take."""
    request_bytes("POST", "/quit", port, timeout)


def read_transforms(port: int = DEFAULT_PORT, timeout: float = 5.0) -> TransformReport:
    """Read /transforms, refusing by reason rather than returning empty."""
    url = f"http://127.0.0.1:{port}/transforms"
    payload = _get("/transforms", port, timeout)
    require_fields(url, payload, TransformReport.__annotations__, "a transform report")
    candidates = []
    for entry in payload["candidates"]:
        require_fields(url, entry, TransformCandidate.__annotations__, "a transform candidate")
        candidates.append(
            TransformCandidate(
                stageIndex=int(entry["stageIndex"]),
                baseHash=int(entry["baseHash"]),
                auxHash=int(entry["auxHash"]),
                floatOffset=int(entry["floatOffset"]),
                framesSeen=int(entry["framesSeen"]),
                shadersSharing=int(entry["shadersSharing"]),
                rotationError=float(entry["rotationError"]),
                meanTranslationStep=float(entry["meanTranslationStep"]),
                values=tuple(float(v) for v in entry["values"]),
            )
        )
    return TransformReport(
        framesObserved=int(payload["framesObserved"]),
        shadersTracked=int(payload["shadersTracked"]),
        spansExamined=int(payload["spansExamined"]),
        rejectedVaryingWithinFrame=int(payload["rejectedVaryingWithinFrame"]),
        rejectedNeverChanging=int(payload["rejectedNeverChanging"]),
        rejectedRotation=int(payload["rejectedRotation"]),
        candidatesFound=int(payload["candidatesFound"]),
        sharedAndMoving=int(payload["sharedAndMoving"]),
        shadersInLastFrame=int(payload["shadersInLastFrame"]),
        candidates=tuple(candidates),
    )


@dataclass(frozen=True)
class CallerEntry:
    """One function the census watches, and the call sites that reached it."""

    entry: int
    installation: str
    calls: int
    callers: tuple[tuple[int, int], ...]

    def render(self) -> str:
        sites = ", ".join(f"{address:#010x} {count}" for address, count in self.callers[:4])
        return (
            f"{self.entry:#010x} {self.installation}: {self.calls} calls"
            f"{f' from {sites}' if sites else ''}"
        )


def read_callers(port: int = DEFAULT_PORT, timeout: float = 2.0) -> tuple[CallerEntry, ...]:
    payload = _get("/callers", port, timeout)
    require_fields("GET /callers", payload, ("entries",), "the caller census")
    return tuple(
        CallerEntry(
            entry=int(entry["entry"], 16),
            installation=str(entry["installation"]),
            calls=int(entry["calls"]),
            callers=tuple(
                (int(caller["returnAddress"], 16), int(caller["calls"]))
                for caller in entry["callers"]
            ),
        )
        for entry in payload["entries"]
    )


@dataclass(frozen=True)
class PaintState:
    """What the title's own paint mod is doing, and what it has seen.

    `paints` counts the frames the display thread has drawn since the title
    started, so a rate is taken from two of these a window apart rather than
    from a running average the runtime keeps.
    """

    frame: int
    vtable: int
    installed: bool
    block: int
    paints: int
    probe: str
    mode: str
    display: int
    fields: dict[str, int]
    refusal: str
    # The two fields the flip decision is read from, as the probe sampled them at
    # the last paint. Sampled per paint rather than read on request because the
    # frame's toggle is a per-paint event: a field read once reads whichever value
    # the last paint left, and the same value means both "the toggle never fires"
    # and "the toggle fired between the two reads".
    flags_at_paint: int = 0
    phase_at_paint: int = 0

    def render(self) -> str:
        fields = ", ".join(f"{name} {value}" for name, value in sorted(self.fields.items()))
        body = (
            f"display paint: installed {self.installed} ({self.mode}), probe "
            f"{self.probe}, block {self.block:#010x}, {self.paints} paints"
        )
        if self.display:
            body += f", display {self.display:#010x}"
            if fields:
                body += f" ({fields})"
        if self.refusal:
            body += f", refused: {self.refusal}"
        return body


@dataclass(frozen=True)
class Binding:
    """One binding: the cursor, the descriptor entry whole, and which of its
    words names memory the guest can read at the entry's offset -- for both
    slots of the ring, because the other one is where the previous tick's values
    would still be."""

    cursor: int
    object: int
    offset: int
    size: int
    entry: dict[int, int]
    readable: dict[int, bool]
    other_cursor: int
    other_offset: int
    other_size: int
    other_entry: dict[int, int]
    other_readable: dict[int, bool]

    def _resolve(self, entry: dict[int, int], readable: dict[int, bool], offset: int) -> int | None:
        """A slot's block address, if exactly one of its words reads.

        Zero or several is not an answer, and this returns nothing rather than
        picking one: a wrong base dumps another object's block and reads as a
        pose.
        """
        named = [word for word, ok in readable.items() if ok]
        return entry[named[0]] + offset if len(named) == 1 else None

    def block(self) -> int | None:
        """The block this binding is drawing from."""
        return self._resolve(self.entry, self.readable, self.offset)

    def other_block(self) -> int | None:
        """The block of the slot this binding did *not* use.

        Whether it still holds the previous tick's pose is what decides if a blend
        can read two ticks' values when the tick paints, rather than recording one
        and replaying it.
        """
        return self._resolve(self.other_entry, self.other_readable, self.other_offset)


@dataclass(frozen=True)
class BlockCensus:
    """What the title's own uniform block binder did, counted."""

    binder: int
    probe: str
    entries: int
    bindings: int
    objects: int
    cursors: dict[int, int]
    cursors_out_of_range: int
    cursor_switches: int
    cursor_compared: int
    examples: tuple[Binding, ...]

    def render(self) -> str:
        spread = ", ".join(
            f"entry {entry}: {count}" for entry, count in sorted(self.cursors.items())
        )
        return (
            f"uniform blocks: {self.bindings} bindings over {self.objects} objects, "
            f"probe {self.probe}; cursor {spread}"
            f"{f', {self.cursors_out_of_range} out of range' if self.cursors_out_of_range else ''}"
        )

    def parity(self) -> str:
        """What the cursor did, in words, from the two entries the list holds."""
        if not self.bindings:
            return "the binder was never called, so nothing is known"
        counts = sorted(self.cursors.values(), reverse=True)
        where = []
        if len(counts) < 2:
            where.append("only one of the two entries was ever read")
        elif counts[0] == 0:
            where.append("no entry was read at all")
        elif counts[0] == counts[1]:
            where.append(f"both entries equally ({counts[0]} each)")
        else:
            where.append(f"the two entries unevenly ({counts[0]} and {counts[1]})")
        # Whether the ring turns per bind or per frame, as a fraction of the
        # bindings that could have been a switch at all. Near one and the two
        # slots are two passes; near zero and the choice is not per bind.
        if self.cursor_compared:
            rate = self.cursor_switches / self.cursor_compared
            where.append(
                f"the cursor moved on {self.cursor_switches} of {self.cursor_compared} "
                f"repeat bindings ({rate:.2f} per bind)"
            )
        else:
            where.append("no object was bound twice, so the cursor's turn is unknown")
        return "; ".join(where)


@dataclass(frozen=True)
class GateState:
    """What the logic gate is doing, and the two counts it keeps in guest memory.

    The three descriptive fields exist so that a count of zero can be read. Zero
    is either "no calls came" or "the gate is not wired to the tick", they are the
    same number, and the difference is whether the probe installed and what the two
    words the gate works through actually hold right now.
    """

    tick: int
    enabled: bool
    calls: int | None
    ticks: int | None
    refusal: str
    probe: str = "unreported"
    gate_probe: str = "unreported"
    gate_entries: int | None = None
    block: str = "unreported"
    word_at_entry: str = "unreported"
    word_at_body: str = "unreported"
    # The probe that holds the tick's entry, and the calls it counted. Separate
    # from `calls`, which is the gate's own guest-written counter: this one is
    # kept by the host, so it moves whenever the tick is called even while the
    # gate is out, and a report that conflated the two would read a call rate as
    # a tick rate.
    holding_probe: str = "unreported"
    calls_at_probe: int | None = None
    resume: str = "unreported"
    # Whether the block holds the counting payload or the pass-through control.
    # Both are "enabled", and only one of them is the gate.
    through: bool = False
    flavour: int = 0

    def render(self) -> str:
        what = (
            ("the pass-through control" if self.through else "the counting payload")
            if self.enabled
            else "nothing"
        )
        return (
            f"logic gate: {'in' if self.enabled else 'out'} at {self.tick:#010x}, "
            f"{what} in {self.block}, {self.calls} calls, {self.ticks} ticks through it; "
            f"{self.calls_at_probe} calls at its probe ({self.holding_probe})"
        )


def read_gate(port: int = DEFAULT_PORT, timeout: float = 2.0) -> GateState:
    """The logic gate's own state, from `/logic`.

    Not from `/gate`: that is the frame gate, which holds a title between frames
    and answers with a different body entirely. Asking it for the tick's counters
    fails on the fields, which is at least a refusal -- but a caller that fell
    back on it would be reading the frame gate's state and calling it the
    simulation's, which is how a run reports a logic rate of zero while the
    simulation is perfectly healthy.
    """
    payload = _get("/logic", port, timeout)
    require_fields(
        "GET /logic", payload, ("tick", "enabled", "callsCount", "ticksCount"), "the logic gate"
    )
    return GateState(
        tick=int(payload["tick"], 16),
        probe=str(payload.get("probe", "unreported")),
        gate_probe=str(payload.get("gateProbe", "unreported")),
        gate_entries=(None if payload.get("gateEntries") is None else int(payload["gateEntries"])),
        block=str(payload.get("block", "unreported")),
        word_at_entry=str(payload.get("wordAtTickEntry", "unreported")),
        word_at_body=str(payload.get("wordAtTickBody", "unreported")),
        holding_probe=str(payload.get("holdingProbe", "unreported")),
        calls_at_probe=(
            None if payload.get("callsAtProbe") is None else int(payload["callsAtProbe"])
        ),
        resume=str(payload.get("resume", "unreported")),
        through=bool(payload.get("through", False)),
        flavour=int(payload.get("flavour", 0)),
        enabled=bool(payload["enabled"]),
        calls=None if payload["callsCount"] is None else int(payload["callsCount"]),
        ticks=None if payload["ticksCount"] is None else int(payload["ticksCount"]),
        refusal=str(payload.get("refusal", "")),
    )


def set_gate(
    on: bool,
    port: int = DEFAULT_PORT,
    timeout: float = 5.0,
    through: bool = False,
    flavour: int = 2,
) -> GateState:
    """Arm or disarm the logic gate.

    `through` installs the pass-through control instead: one word that branches
    back to the tick and keeps no state. It exists to ask whether a direct branch
    out of recompiled code into the loader's arena runs at all, and the observer is
    the caller census, which keeps counting the tick either way -- so nothing in the
    answer rests on the gate's own payload or its own counters.
    """
    query = f"/logic?on={1 if on else 0}"
    if through:
        query += f"&through=1&flavour={flavour}"
    body = request_bytes("POST", query, port, timeout)
    try:
        payload = json.loads(body.decode("utf-8"))
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"POST /logic answered something that is not JSON: {malformed}")
    return (
        read_gate(port, timeout)
        if "tick" not in payload
        else GateState(
            tick=int(payload["tick"], 16),
            enabled=bool(payload["enabled"]),
            calls=None if payload["callsCount"] is None else int(payload["callsCount"]),
            ticks=None if payload["ticksCount"] is None else int(payload["ticksCount"]),
            refusal=str(payload.get("refusal", "")),
        )
    )


def read_blocks(port: int = DEFAULT_PORT, timeout: float = 2.0) -> BlockCensus:
    payload = _get("/blocks", port, timeout)
    require_fields(
        "GET /blocks",
        payload,
        (
            "binder",
            "probe",
            "entries",
            "bindings",
            "objects",
            "cursors",
            "cursorsOutOfRange",
            "cursorSwitches",
            "cursorCompared",
        ),
        "the uniform block census",
    )
    return BlockCensus(
        binder=int(payload["binder"], 16),
        probe=str(payload["probe"]),
        entries=int(payload["entries"]),
        bindings=int(payload["bindings"]),
        objects=int(payload["objects"]),
        cursors={int(k): int(v) for k, v in payload["cursors"].items()},
        cursors_out_of_range=int(payload["cursorsOutOfRange"]),
        cursor_switches=int(payload["cursorSwitches"]),
        cursor_compared=int(payload["cursorCompared"]),
        examples=tuple(
            Binding(
                cursor=int(one["cursor"]),
                object=int(one["object"], 16),
                offset=int(one["offset"]),
                size=int(one["size"]),
                entry={int(k): int(v) for k, v in one.get("entry", {}).items()},
                readable={int(k): bool(v) for k, v in one.get("readableAtOffset", {}).items()},
                other_cursor=int(one.get("otherCursor", 0)),
                other_offset=int(one.get("otherOffset", 0)),
                other_size=int(one.get("otherSize", 0)),
                other_entry={int(k): int(v) for k, v in one.get("otherEntry", {}).items()},
                other_readable={
                    int(k): bool(v) for k, v in one.get("otherReadableAtOffset", {}).items()
                },
            )
            for one in payload.get("examples", {}).values()
        ),
    )


def capture_frame(port: int, slot: int, timeout: float = 15.0) -> bytes:
    """One frame's bytes, by arming a slot and waiting for the image to land.

    The arm is one-shot, so a second call is a second frame rather than the
    same one again -- which is the whole point when the question is whether two
    consecutive paints are the same picture.
    """
    request_bytes("POST", f"/capture?slot={slot}", port, timeout)
    deadline = time.monotonic() + timeout
    refusal = ""
    while time.monotonic() < deadline:
        try:
            body = request_bytes("GET", f"/capture?slot={slot}", port, timeout)
        except ControlUnavailable as notyet:
            refusal = str(notyet)
            time.sleep(0.2)
            continue
        if body:
            return body
        refusal = "the slot held an empty image"
        time.sleep(0.2)
    raise ControlUnavailable(f"no image reached slot {slot} within {timeout:.0f}s: {refusal}")


def capture_run(port: int, count: int, slot: int = 0, timeout: float = 25.0) -> tuple[bytes, ...]:
    """`count` consecutive presents, each of them *new*.

    A slot keeps whatever last landed in it, so polling it and taking the first
    non-empty body returns the previous run's image whenever the new one has not
    arrived yet. That is not a slower read, it is a different answer: two rounds
    of a comparison came back with byte-identical checksums and a paint count
    that had not moved, which is what a stale pair looks like and what a
    re-capture does not.

    So the arm's own `imagesReceived` is the watermark. The product counts every
    image it hands over, the answer to the arm says what that count was, and the
    wait is for the count to reach watermark + count before a single slot is
    read. Each image then got here after the arm, which is the property the
    comparison needs and the property a bare poll does not have.

    Consecutive, not "ask `count` times": the renderer's screenshot request is
    one at a time, so asking again waits a whole frame, and a title that animates
    gives a different picture.
    """
    if count < 1:
        raise ControlUnavailable(f"refused: a run of {count} presents is not a run")
    answer = request_bytes("POST", f"/capture?slot={slot}&count={count}", port, timeout)
    try:
        watermark = int(json.loads(answer.decode("utf-8"))["imagesReceived"])
    except (json.JSONDecodeError, KeyError, ValueError) as unreadable:
        raise ControlUnavailable(
            f"POST /capture answered something without a watermark to wait on: {unreadable}; "
            f"body was {answer[:200].decode('utf-8', 'replace')!r}"
        ) from unreadable

    deadline = time.monotonic() + timeout
    wanted = watermark + count
    received = watermark
    while time.monotonic() < deadline:
        received = _counter_field(port, "imagesReceived", timeout=5.0)
        if received is not None and received >= wanted:
            break
        time.sleep(0.1)
    if received is None or received < wanted:
        raise ControlUnavailable(
            f"only {received} images reached the product's slots within {timeout:.0f}s, and "
            f"{wanted} were needed for a run of {count}"
        )

    images: list[bytes] = []
    for index in range(count):
        where = slot + index
        body = b""
        last = time.monotonic() + min(timeout, 10.0)
        while time.monotonic() < last and not body:
            try:
                body = request_bytes("GET", f"/capture?slot={where}", port, 5.0)
            except ControlUnavailable:
                time.sleep(0.1)
        if not body:
            raise ControlUnavailable(
                f"the run reported {received} images received, and slot {where} is empty"
            )
        images.append(body)
    return tuple(images)


def _counter_field(port: int, name: str, timeout: float = 5.0) -> int | None:
    """One number out of `GET /counters`, or None when the report does not carry it.

    `/counters`, and not `/setup`: the capture counts are in the counters report,
    and `/setup` is the first-run setup status with four fields in it. Reading the
    wrong one is a None that looks like a report, which is how a wait for a
    watermark that is never there passes for a wait that succeeded.

    None rather than zero: a missing field is not a count of nothing, and a
    watermark that silently read zero would let a stale image through as though it
    were fresh -- which is the exact failure this wait exists to prevent.
    """
    try:
        payload = _get("/counters", port, timeout)
    except ControlUnavailable:
        return None
    value = payload.get(name)
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


# The most a tool may ask the product to hand over in one piece. A gigabyte is
# not a hypothetical: the title's own descriptor entry has a word at +0x04 that
# reads 0x3e634300, which a tool took for a byte count and asked for, and the
# product died allocating it. The bound is here rather than in each caller
# because the number that caused it came out of the title, not out of a tool.
MAX_DUMP_BYTES = 1 << 20


def dump_guest(port: int, address: int, size: int, timeout: float = 5.0) -> bytes:
    """A range of guest memory, as the bytes lie. Refuses by reason.

    A size beyond `MAX_DUMP_BYTES` is refused here, naming the number, because the
    cost of asking is the product's memory and there is no way to ask politely
    about a gigabyte. A word read out of guest memory is not a length until
    something has said it is one.
    """
    if size < 0 or size > MAX_DUMP_BYTES:
        raise ControlUnavailable(
            f"refused: {size} bytes at {address:#x} is beyond the {MAX_DUMP_BYTES}-byte bound "
            "on one dump. A word read out of guest memory is not a length until something has "
            "said it is one."
        )
    return request_bytes("GET", f"/memory?address={address:x}&size={size}", port, timeout)


def compare_bytes(first: bytes, second: bytes) -> str:
    """How two byte ranges differ, for a range that has no image to look at."""
    if len(first) != len(second):
        return f"different sizes: {len(first)} and {len(second)} bytes"
    differing = [i for i, (a, b) in enumerate(zip(first, second)) if a != b]
    if not differing:
        return f"identical, {len(first)} bytes"
    return (
        f"{len(differing)} of {len(first)} bytes differ, first at offset {differing[0]} "
        f"({first[differing[0]]:#04x} against {second[differing[0]]:#04x})"
    )


def compare_images(first: bytes, second: bytes) -> str:
    """How two captured frames differ, byte by byte, in one sentence."""
    if len(first) != len(second):
        return f"different sizes: {len(first)} and {len(second)} bytes"
    differing = [i for i, (a, b) in enumerate(zip(first, second)) if a != b]
    if not differing:
        return f"identical, {len(first)} bytes"
    return (
        f"{len(differing)} of {len(first)} bytes differ, first at offset {differing[0]} "
        f"({first[differing[0]]:#04x} against {second[differing[0]]:#04x})"
    )


def read_paint(port: int = DEFAULT_PORT, timeout: float = 2.0) -> PaintState:
    return _paint_state(_get("/paint", port, timeout), "GET /paint")


def set_paint(
    on: bool, port: int = DEFAULT_PORT, timeout: float = 5.0, mode: int = 3
) -> PaintState:
    body = request_bytes("POST", f"/paint?on={1 if on else 0}&mode={mode}", port, timeout)
    try:
        return _paint_state(json.loads(body.decode("utf-8")), "POST /paint")
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(
            f"POST /paint answered something that is not JSON: {malformed}; body was "
            f"{body[:300].decode('utf-8', 'replace')!r}"
        )


def _paint_state(payload: dict, url: str) -> PaintState:
    require_fields(
        url,
        payload,
        ("frame", "vtable", "installed", "block", "paints", "probe", "mode", "display", "fields"),
        "the display paint mod",
    )
    return PaintState(
        frame=int(payload["frame"], 16),
        vtable=int(payload["vtable"], 16),
        installed=bool(payload["installed"]),
        block=int(payload["block"], 16),
        paints=int(payload["paints"]),
        probe=str(payload["probe"]),
        mode=str(payload["mode"]),
        display=int(payload["display"], 16),
        fields={str(name): int(value) for name, value in payload["fields"].items()},
        refusal=str(payload.get("refusal", "")),
        flags_at_paint=int(payload.get("flagsAtLastPaint", 0)),
        phase_at_paint=int(payload.get("phaseAtLastPaint", 0)),
    )
