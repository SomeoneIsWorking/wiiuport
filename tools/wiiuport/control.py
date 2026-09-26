"""Client for the running product's control channel.

This is how a tool asks the runtime what it is doing while it runs, instead of
launching it and reading its log afterwards. A log-scraping loop can only ask
what the script already knew to ask, and cannot ask anything of a run in
progress.
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass

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


def _get(path: str, port: int, timeout: float) -> dict:
    url = f"http://127.0.0.1:{port}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(
            f"{url} did not answer ({unreachable.reason}). The runtime is not running, "
            "or listens on another WIIUPORT_CONTROL_PORT."
        ) from unreachable
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )


def request_bytes(method: str, path: str, port: int, timeout: float) -> bytes:
    """One request to the channel, refusing by reason: a refusal carries the
    runtime's own explanation, and silence says the runtime is not there."""
    url = f"http://127.0.0.1:{port}{path}"
    request = urllib.request.Request(url, method=method, data=b"" if method == "POST" else None)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read()
    except urllib.error.HTTPError as refused:
        body = refused.read().decode("utf-8", "replace").strip()
        raise ControlUnavailable(f"{url} was refused ({refused.code}): {body}") from refused
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(
            f"{url} did not answer ({unreachable.reason}). The runtime is not running, "
            "or listens on another WIIUPORT_CONTROL_PORT."
        ) from unreachable


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
    words names memory the guest can read at the entry's offset."""

    cursor: int
    offset: int
    size: int
    entry: dict[int, int]
    readable: dict[int, bool]

    def block(self) -> int | None:
        """The block's address, if exactly one of the entry's words reads.

        Zero or several is not an answer, and this returns nothing rather than
        picking one: a wrong base dumps another object's block and reads as a
        pose.
        """
        named = [word for word, ok in self.readable.items() if ok]
        return self.entry[named[0]] + self.offset if len(named) == 1 else None


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
        counts = sorted(self.cursors.values(), reverse=True)
        if not self.bindings:
            return "the binder was never called, so nothing is known"
        if len(counts) < 2:
            return "only one of the two entries was ever read"
        if counts[0] == 0:
            return "no entry was read at all"
        if counts[0] == counts[1]:
            return f"both entries equally ({counts[0]} each): no alternation"
        return f"the two entries unevenly ({counts[0]} and {counts[1]}): they do alternate"


def read_blocks(port: int = DEFAULT_PORT, timeout: float = 2.0) -> BlockCensus:
    payload = _get("/blocks", port, timeout)
    require_fields(
        "GET /blocks",
        payload,
        ("binder", "probe", "entries", "bindings", "objects", "cursors", "cursorsOutOfRange"),
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
        examples=tuple(
            Binding(
                cursor=int(one["cursor"]),
                offset=int(one["offset"]),
                size=int(one["size"]),
                entry={int(k): int(v) for k, v in one.get("entry", {}).items()},
                readable={int(k): bool(v) for k, v in one.get("readableAtOffset", {}).items()},
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
    )
