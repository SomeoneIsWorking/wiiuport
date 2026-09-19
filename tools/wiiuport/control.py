"""Client for the running product's control channel.

This is how a tool asks the runtime what it is doing while it runs, instead of
launching it and reading its log afterwards. A log-scraping loop can only ask
what the script already knew to ask, and cannot ask anything of a run in
progress.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request
from dataclasses import dataclass

DEFAULT_PORT = 21337
"""The port maintainer tools use. The product opens none unless one is
configured, so this is a convention between tools, not a default the product
carries."""


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

    @property
    def recorded_anything(self) -> bool:
        return self.framesObserved > 0 and self.displayListsSeen > 0

    def render(self) -> str:
        return (
            f"frames {self.framesObserved} (refused incomplete "
            f"{self.framesRefusedIncomplete}), display lists {self.displayListsSeen}, "
            f"uniform assemblies {self.uniformAssembliesSeen}; last frame held "
            f"{self.lastFrameDisplayLists} lists and "
            f"{self.lastFrameUniformAssemblies} assemblies in {self.lastFrameBytes} bytes; "
            f"replays {self.replaysRun} submitting {self.replayListsSubmitted} lists "
            f"({self.replayListsRefused} refused)"
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
    candidates: tuple[TransformCandidate, ...]

    def render(self) -> str:
        totals = (
            f"{self.candidatesFound} candidates from {self.spansExamined} spans examined "
            f"across {self.shadersTracked} shaders over {self.framesObserved} frames"
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
        elif not any(c.is_shared for c in self.candidates):
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


def _require(url: str, payload: dict, fields: object, what: str) -> None:
    missing = set(fields) - set(payload)
    if missing:
        raise ControlUnavailable(
            f"{url} answered without {sorted(missing)}, so the runtime and this client "
            f"disagree about what {what} is"
        )


def read_counters(port: int = DEFAULT_PORT, timeout: float = 2.0) -> Counters:
    """Read /counters, refusing by reason rather than returning empty."""
    payload = _get("/counters", port, timeout)
    _require(
        f"http://127.0.0.1:{port}/counters", payload, Counters.__annotations__, "a counter set"
    )
    return Counters(**{field: int(payload[field]) for field in Counters.__annotations__})


def read_transforms(port: int = DEFAULT_PORT, timeout: float = 5.0) -> TransformReport:
    """Read /transforms, refusing by reason rather than returning empty."""
    url = f"http://127.0.0.1:{port}/transforms"
    payload = _get("/transforms", port, timeout)
    _require(url, payload, TransformReport.__annotations__, "a transform report")
    candidates = []
    for entry in payload["candidates"]:
        _require(url, entry, TransformCandidate.__annotations__, "a transform candidate")
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
        candidates=tuple(candidates),
    )
