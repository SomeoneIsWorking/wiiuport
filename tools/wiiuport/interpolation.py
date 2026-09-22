"""Client for continuous interpolation: its counters and consecutive frames.

Every tick the title draws is counted, and every tick without an in-between
frame is counted by why, so a run where interpolation never fired reads as
exactly that from the numbers rather than as a quiet pass.
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, request_bytes, require_fields

RECORDINGS_MAGIC = b"WIIUREC1"


@dataclass(frozen=True)
class Interpolation:
    """What GET /interpolation reports, with its denominators."""

    enabled: bool
    ticks: int
    framesInterpolated: int
    skipped: dict[str, int]
    restoresRefused: int
    phaseNanoseconds: dict[str, int]
    cutsByTurn: int
    cutsByStep: int
    objects: dict[str, int]
    objectPartnersDerived: int
    objectPartnersSearched: int
    objectSearchesDeferred: int
    objectValuesNotBlended: int
    objectDrawsWritten: int
    objectReplaysDiverged: int
    # Frame times at the display since the last POST /pacing: a measurement
    # over its own window, not a counter, so a window of a run keeps it whole.
    pacing: dict[str, int]
    viewFramesTracked: int
    viewFramesLost: int
    viewReseedsRun: int
    viewReseedsFound: int
    copiesSubmitted: int
    withheld: dict[str, int]
    recordingSnapshots: int

    def since(self, earlier: Interpolation) -> Interpolation:
        """The counts accumulated after `earlier`, so a window of a run is
        measured rather than everything since boot."""

        def delta(name: str) -> int:
            return int(getattr(self, name)) - int(getattr(earlier, name))

        counted = {
            name: delta(name)
            for name in Interpolation.__annotations__
            if name
            not in {"enabled", "skipped", "withheld", "phaseNanoseconds", "objects", "pacing"}
        }
        return Interpolation(
            enabled=self.enabled,
            pacing=self.pacing,
            objects={k: v - earlier.objects.get(k, 0) for k, v in self.objects.items()},
            skipped={k: v - earlier.skipped.get(k, 0) for k, v in self.skipped.items()},
            withheld={k: v - earlier.withheld.get(k, 0) for k, v in self.withheld.items()},
            phaseNanoseconds={
                k: v - earlier.phaseNanoseconds.get(k, 0) for k, v in self.phaseNanoseconds.items()
            },
            **counted,
        )

    def render(self) -> str:
        skipped = ", ".join(f"{name} {count}" for name, count in self.skipped.items() if count)
        withheld = ", ".join(f"{name} {count}" for name, count in self.withheld.items() if count)
        per_frame = max(1, self.framesInterpolated)
        phases = ", ".join(
            f"{name} {ns / per_frame / 1e6:.2f} ms" for name, ns in self.phaseNanoseconds.items()
        )
        return "\n".join(
            [
                (
                    f"continuous interpolation {'on' if self.enabled else 'off'}: "
                    f"{self.framesInterpolated} of {self.ticks} ticks interpolated"
                ),
                f"  skipped: {skipped or 'none'}",
                f"  per interpolated tick: {phases}",
                (
                    f"  restores refused {self.restoresRefused}, "
                    f"copies submitted {self.copiesSubmitted}"
                ),
                f"  cuts: {self.cutsByTurn} by turn, {self.cutsByStep} by step",
                (
                    "  objects: "
                    + ", ".join(f"{name} {count}" for name, count in self.objects.items())
                    + f"; partners {self.objectPartnersDerived} derived, "
                    f"{self.objectPartnersSearched} searched, "
                    f"{self.objectSearchesDeferred} searches deferred; "
                    f"{self.objectDrawsWritten} draws written, "
                    f"{self.objectReplaysDiverged} replays out of step with the recording, "
                    f"{self.objectValuesNotBlended} values kept as not numbers"
                ),
                (
                    f"  view: {self.viewFramesTracked} frames tracked, "
                    f"{self.viewFramesLost} lost, "
                    f"{self.viewReseedsFound} of {self.viewReseedsRun} searches found it"
                ),
                f"  withheld from runtime submissions: {withheld or 'none'}",
                self.render_pacing(),
            ]
        )

    def render_pacing(self) -> str:
        p = self.pacing
        ms = {name: p[name] / 1000 for name in p if name.endswith("Us")}
        return (
            f"  displayed: {p['guestFrames']} title frames, {p['runtimeFrames']} in-between; "
            f"frame time p50 {ms['p50Us']:.1f} ms, p95 {ms['p95Us']:.1f} ms, "
            f"p99 {ms['p99Us']:.1f} ms, longest {ms['longestUs']:.1f} ms "
            f"over {p['intervalsKept']} of {p['intervals']} intervals; "
            f"median title-to-in-between {ms['guestToRuntimeMedianUs']:.1f} ms, "
            f"in-between-to-title {ms['runtimeToGuestMedianUs']:.1f} ms"
        )


def read_interpolation(port: int = DEFAULT_PORT, timeout: float = 5.0) -> Interpolation:
    """Read /interpolation, refusing by reason rather than returning empty."""
    url = f"http://127.0.0.1:{port}/interpolation"
    try:
        payload = json.loads(request_bytes("GET", "/interpolation", port, timeout))
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )
    require_fields(url, payload, Interpolation.__annotations__, "the interpolation report")
    return Interpolation(**{field: payload[field] for field in Interpolation.__annotations__})


def restart_pacing(port: int = DEFAULT_PORT, timeout: float = 5.0) -> None:
    """Measure frame times from now on, not since boot."""
    request_bytes("POST", "/pacing", port, timeout)


def set_continuous(on: bool, port: int = DEFAULT_PORT, timeout: float = 5.0) -> None:
    request_bytes("POST", f"/continuous?on={1 if on else 0}", port, timeout)


@dataclass(frozen=True)
class RecordedAssembly:
    """One draw's uniform assembly as the recorder published it."""

    baseHash: int
    auxHash: int
    stage: int
    sources: tuple[int, ...]
    floats: tuple[float, ...]


@dataclass(frozen=True)
class RecordedFrame:
    complete: bool
    assemblies: tuple[RecordedAssembly, ...]


class _Reader:
    """Walks a framed snapshot, refusing a body that ends early rather than
    returning the frames that happened to fit."""

    def __init__(self, body: bytes) -> None:
        self._body = body
        self._at = 0

    def take(self, layout: str) -> tuple:
        size = struct.calcsize(layout)
        if self._at + size > len(self._body):
            raise ControlUnavailable(
                f"the recordings snapshot ends at byte {len(self._body)} while {size} more "
                f"were expected at {self._at}: it is truncated or not a snapshot"
            )
        values = struct.unpack_from(layout, self._body, self._at)
        self._at += size
        return values

    def finished(self) -> bool:
        return self._at == len(self._body)


def parse_recordings(body: bytes) -> tuple[RecordedFrame, ...]:
    """Decode GET /recordings. The runtime writes host byte order, and the
    tools run on the host that wrote it."""
    if not body.startswith(RECORDINGS_MAGIC):
        raise ControlUnavailable("the recordings body does not start with WIIUREC1")
    reader = _Reader(body[len(RECORDINGS_MAGIC) :])
    (frame_count,) = reader.take("=I")
    frames = []
    for _ in range(frame_count):
        complete, assembly_count = reader.take("=II")
        assemblies = []
        for _ in range(assembly_count):
            base, aux, stage, source_count = reader.take("=QQII")
            sources = reader.take(f"={source_count}I")
            (float_count,) = reader.take("=I")
            floats = reader.take(f"={float_count}f")
            assemblies.append(RecordedAssembly(base, aux, stage, sources, floats))
        frames.append(RecordedFrame(bool(complete), tuple(assemblies)))
    if not reader.finished():
        raise ControlUnavailable("the recordings snapshot has bytes after its last frame")
    return tuple(frames)


def arm_recordings(frames: int, port: int = DEFAULT_PORT, timeout: float = 5.0) -> None:
    request_bytes("POST", f"/recordings?frames={frames}", port, timeout)


def fetch_recordings(port: int = DEFAULT_PORT, timeout: float = 20.0) -> bytes:
    """The last completed snapshot as framed, for keeping and parsing both."""
    return request_bytes("GET", "/recordings", port, timeout)
