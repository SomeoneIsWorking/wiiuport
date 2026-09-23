"""The object census: one planned frame's objects, shader by shader.

The interpolation counters say how many objects were drawn un-blended; the
census says which shaders drew them, so a kind of object the blend does not
understand shows up as one row rather than as a total.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, request_bytes, require_fields

# What an object can come to, as the runtime names them. An outcome the runtime
# reports that is not here, or one missing, is refused rather than dropped.
OUTCOMES = ("blended", "held", "unmatched", "unverified", "outside", "shading")
UNBLENDED = ("unmatched", "unverified", "outside")

_ROW_FIELDS = (
    "stageIndex",
    "baseHash",
    "auxHash",
    "draws",
    "outcomes",
    "mostValues",
    "withoutBlocks",
)
_CENSUS_FIELDS = ("frames", "objects", "outcomes", "shaders", "rows")


def _outcomes(url: str, payload: dict) -> dict[str, int]:
    if set(payload) != set(OUTCOMES):
        raise ControlUnavailable(
            f"{url} named outcomes {sorted(payload)}, and this client knows {sorted(OUTCOMES)}"
        )
    return {name: int(payload[name]) for name in OUTCOMES}


@dataclass(frozen=True)
class CensusRow:
    stage: int
    base_hash: int
    aux_hash: int
    draws: int
    outcomes: dict[str, int]
    most_values: int
    without_blocks: int

    @property
    def unblended(self) -> int:
        return sum(self.outcomes[name] for name in UNBLENDED)

    def render(self) -> str:
        counts = ", ".join(
            f"{name} {self.outcomes[name]}" for name in OUTCOMES if self.outcomes[name]
        )
        return (
            f"stage {self.stage} {self.base_hash:016x}/{self.aux_hash:016x}: "
            f"{self.draws} draws ({counts}); up to {self.most_values} values, "
            f"{self.without_blocks} without blocks"
        )


@dataclass(frozen=True)
class ObjectCensus:
    frames: int
    objects: int
    outcomes: dict[str, int]
    shaders: int
    rows: tuple[CensusRow, ...]

    @classmethod
    def parse(cls, url: str, payload: dict) -> ObjectCensus:
        require_fields(url, payload, _CENSUS_FIELDS, "the object census")
        rows = []
        for row in payload["rows"]:
            require_fields(url, row, _ROW_FIELDS, "a census row")
            rows.append(
                CensusRow(
                    stage=int(row["stageIndex"]),
                    base_hash=int(row["baseHash"]),
                    aux_hash=int(row["auxHash"]),
                    draws=int(row["draws"]),
                    outcomes=_outcomes(url, row["outcomes"]),
                    most_values=int(row["mostValues"]),
                    without_blocks=int(row["withoutBlocks"]),
                )
            )
        census = cls(
            frames=int(payload["frames"]),
            objects=int(payload["objects"]),
            outcomes=_outcomes(url, payload["outcomes"]),
            shaders=int(payload["shaders"]),
            rows=tuple(rows),
        )
        if sum(census.outcomes.values()) != census.objects:
            raise ControlUnavailable(
                f"{url} counted {census.objects} objects but {sum(census.outcomes.values())} "
                "outcomes: every object has exactly one"
            )
        return census

    def render(self) -> str:
        unblended = sum(self.outcomes[name] for name in UNBLENDED)
        counts = ", ".join(f"{name} {self.outcomes[name]}" for name in OUTCOMES)
        heading = (
            f"object census over {self.frames} frames: {self.objects} objects under {self.shaders} shaders ({counts}); "
            f"{unblended} un-blended; listing {len(self.rows)} shaders, most un-blended first:"
        )
        lines = [heading]
        lines.extend(f"  {row.render()}" for row in self.rows)
        return "\n".join(lines)


def request_census(frames: int, port: int = DEFAULT_PORT, timeout: float = 5.0) -> None:
    """Ask for a census of the next `frames` frames planned."""
    request_bytes("POST", f"/objects?frames={frames}", port, timeout)


def read_census(port: int = DEFAULT_PORT, timeout: float = 5.0) -> ObjectCensus:
    """The census last taken; refused, by the runtime's reason, until one is."""
    url = f"http://127.0.0.1:{port}/objects"
    try:
        payload = json.loads(request_bytes("GET", "/objects", port, timeout))
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )
    return ObjectCensus.parse(url, payload)
