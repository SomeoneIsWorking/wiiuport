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


def runtime_env(port: int) -> dict[str, str]:
    """The environment a driven run hands the product.

    It is the control port and nothing else. This once also carried
    `WIIUPORT_INTERPOLATION`, which switched the host-side frame interpolation
    on and off; that mechanism is deleted, the runtime no longer reads the
    variable, and a tool that still set it would be asking for something the
    product cannot obey."""
    return {ENV_CONTROL_PORT: str(port)}


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
    inputPollsSeen: int
    inputPollsAnswered: int
    inputPressesQueued: int
    capturesRequested: int
    capturesRefused: int
    imagesReceived: int
    displayListsFromRuntime: int
    uniformAssembliesFromRuntime: int
    nestedListsSeen: int
    guestDrawsFromCommandBuffers: int
    guestDrawsFromRing: int
    guestDrawsPrepared: int
    guestDrawsWithoutVertexUniforms: int
    runtimeSubmissions: int
    runtimePacketsProcessed: int
    runtimeDrawsIssued: int

    @property
    def recorded_anything(self) -> bool:
        return self.framesObserved > 0 and self.displayListsSeen > 0

    def render(self) -> str:
        # **The counters the retired mechanism reported are gone, and so are their sentences.** The
        # replay's lists, the runtime's own submissions, the presents it drove, the null diffs and
        # the substituted assemblies all belonged to a host that re-issued a recorded frame. What
        # is left is the title's own draw stream, and the numbers here are all about that.
        return (
            f"frames {self.framesObserved} (refused incomplete "
            f"{self.framesRefusedIncomplete}), display lists {self.displayListsSeen}, "
            f"uniform assemblies {self.uniformAssembliesSeen}; last frame held "
            f"{self.lastFrameDisplayLists} lists and "
            f"{self.lastFrameUniformAssemblies} assemblies in {self.lastFrameBytes} bytes; "
            f"nested lists {self.nestedListsSeen}; the title drew "
            f"{self.guestDrawsFromCommandBuffers} times from command buffers and "
            f"{self.guestDrawsFromRing} straight from the ring; "
            f"{self.guestDrawsWithoutVertexUniforms} of {self.guestDrawsPrepared} draws prepared "
            "with no vertex uniforms"
        )


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
