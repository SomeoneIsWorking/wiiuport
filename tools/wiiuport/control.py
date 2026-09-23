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
"""The port maintainer tools use. The product opens none unless one is
configured, so this is a convention between tools, not a default the product
carries."""


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
            f"{self.guestDrawsFromRing} straight from the ring; "
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
            "or was started without WIIUPORT_CONTROL_PORT."
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
            "or was started without WIIUPORT_CONTROL_PORT."
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
