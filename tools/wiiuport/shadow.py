"""Whether an in-between frame changes guest memory, asked of a running runtime.

The runtime makes the measurement (``POST /shadowcheck``) on a frame the gate
holds; this holds the gate, asks, and reads the answer back as a report that
refuses to call a check clean when it was not made.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

from .control import ControlUnavailable, request_bytes, require_fields

_FIELDS = (
    "clean",
    "rounds",
    "inBetweensDrawn",
    "bytesCompared",
    "controlPages",
    "inBetweenPages",
    "charged",
    "inBetweenOnlyCount",
    "inBetweenOnly",
    "refusal",
)


@dataclass(frozen=True)
class ShadowReport:
    clean: bool
    rounds: int
    inBetweensDrawn: int
    bytesCompared: int
    controlPages: int
    inBetweenPages: int
    # Pages changed in every in-between window and in no control window.
    charged: int
    inBetweenOnlyCount: int
    # (page, how many in-between windows it changed in)
    inBetweenOnly: tuple[tuple[str, int], ...]
    refusal: str

    @classmethod
    def parse(cls, url: str, payload: dict) -> ShadowReport:
        require_fields(url, payload, _FIELDS, "a shadow check")
        pages = tuple((entry["page"], entry["windows"]) for entry in payload["inBetweenOnly"])
        return cls(**{**payload, "inBetweenOnly": pages})

    def render(self) -> str:
        if self.refusal:
            return f"not measured: {self.refusal}"
        lines = [
            (
                f"{self.rounds} rounds, {self.inBetweensDrawn} in-between frames drawn, "
                f"{self.bytesCompared:,} bytes of guest memory compared"
            ),
            (
                f"pages changed: {self.controlPages} in control windows (the guest's own "
                f"threads), {self.inBetweenPages} in in-between windows"
            ),
            (
                f"pages changed only while an in-between frame was drawn: "
                f"{self.inBetweenOnlyCount}, of which {self.charged} changed in every "
                "in-between window and are charged to the frame"
            ),
        ]
        lines += [
            f"  {page}  in {windows} of {self.rounds} in-between windows"
            for page, windows in self.inBetweenOnly
        ]
        if self.inBetweensDrawn < self.rounds:
            lines.append(
                f"{self.rounds - self.inBetweensDrawn} rounds drew nothing, so they "
                "measured nothing; is continuous interpolation on?"
            )
        return "\n".join(lines)


def hold(port: int, timeout: float = 10.0) -> None:
    request_bytes("POST", "/gate?pause=1", port, timeout)


def resume(port: int, timeout: float = 10.0) -> None:
    request_bytes("POST", "/gate?resume=1", port, timeout)


def check(port: int, rounds: int, timeout: float = 120.0) -> ShadowReport:
    path = f"/shadowcheck?rounds={rounds}"
    body = request_bytes("POST", path, port, timeout)
    try:
        payload = json.loads(body.decode("utf-8"))
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{path} answered something that is not JSON: {malformed}") from (
            malformed
        )
    return ShadowReport.parse(path, payload)
