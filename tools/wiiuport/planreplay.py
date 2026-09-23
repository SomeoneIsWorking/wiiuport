"""What planning a kept snapshot again came to, read from wiiuport_plan_replay.

The executable plans the frames through the shipping planner; this reads its
one JSON line and says what it means per search and per frame, so two versions
of the planner can be compared on the same frames.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .cxxtests import build_target
from .paths import Layout

TARGET = "wiiuport_plan_replay"


class PlanReplayFailed(RuntimeError):
    """The snapshot was not planned, so nothing was measured."""


@dataclass(frozen=True)
class PlanReplayReport:
    frames: int
    framesPlanned: int
    outcomes: dict[str, int]
    partnersDerived: int
    partnersSearched: int
    partnersReidentified: int
    reidentifyAttempts: int
    partnerCandidates: int
    nearestCandidates: int
    planningNanoseconds: int

    @classmethod
    def parse(cls, line: str) -> PlanReplayReport:
        report = cls(**json.loads(line))
        # Planning needs two whole frames before the one planned: a snapshot
        # too short for that measured nothing, however fast it was.
        if report.framesPlanned == 0:
            raise PlanReplayFailed(
                f"none of the {report.frames} frames was planned; a snapshot needs "
                "three frames before any is"
            )
        return report

    def render(self) -> str:
        objects = sum(self.outcomes.values())
        outcomes = ", ".join(f"{name} {count}" for name, count in self.outcomes.items())
        per_frame_ms = self.planningNanoseconds / self.frames / 1e6
        searches = (
            f"{self.partnerCandidates} draws compared for partners over "
            f"{self.partnersSearched} searches "
            f"({self.partnerCandidates / max(1, self.partnersSearched):.1f} each), "
            f"{self.nearestCandidates} for identity over {self.reidentifyAttempts} "
            f"({self.nearestCandidates / max(1, self.reidentifyAttempts):.1f} each)"
        )
        return "\n".join(
            [
                f"planned {self.framesPlanned} of {self.frames} frames: {objects} objects ({outcomes})",
                (
                    f"partners {self.partnersDerived} derived, {self.partnersSearched} searched "
                    f"({self.partnersReidentified} found by their values)"
                ),
                searches,
                f"planning took {per_frame_ms:.3f} ms a frame, the fastest of the repeats",
            ]
        )


def plan_again(layout: Layout, recordings: Path, repeats: int) -> PlanReplayReport:
    """Build the replay and plan `recordings` through it `repeats` times."""
    if not recordings.is_file():
        raise PlanReplayFailed(f"no recordings snapshot at {recordings}")
    build = build_target(layout, TARGET)
    ran = subprocess.run(
        [str(build / "tools" / "cxx" / TARGET), str(recordings), str(repeats)],
        capture_output=True,
        text=True,
        check=False,
    )
    if ran.returncode != 0:
        raise PlanReplayFailed(f"{TARGET} failed: {(ran.stdout + ran.stderr).strip()}")
    return PlanReplayReport.parse(ran.stdout.strip())
