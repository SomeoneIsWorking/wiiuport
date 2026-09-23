"""Replayed draws' vertex outcomes, vertex shader by vertex shader.

The interpolation counters say how many draws' vertices stepped at the
title's rate; this says which meshes did, and why. The runtime keeps running
totals, so a window is the difference of two reads.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, request_bytes, require_fields

# What a draw's vertices can come to, as the runtime names them. One the
# runtime reports that is not here, or one missing, is refused.
OUTCOMES = (
    "blended",
    "unchanged",
    "held",
    "unverified",
    "noPartner",
    "shapeDiffers",
    "outside",
    "notFloats",
    "started",
)
# Drawn as the title drew it although its vertices may have moved.
STEPPED = ("unverified", "noPartner", "shapeDiffers", "outside", "started")


@dataclass(frozen=True)
class ShaderRow:
    base_hash: int
    draws: dict[str, int]

    @property
    def stepped(self) -> int:
        return sum(self.draws[name] for name in STEPPED)

    def render(self) -> str:
        counts = ", ".join(f"{name} {self.draws[name]}" for name in OUTCOMES if self.draws[name])
        return f"{self.base_hash:016x}: {counts}"


def parse(url: str, payload: dict) -> dict[int, ShaderRow]:
    require_fields(url, payload, ("shaders",), "the vertex census")
    rows: dict[int, ShaderRow] = {}
    for shader in payload["shaders"]:
        require_fields(url, shader, ("baseHash", "draws"), "a vertex shader's row")
        if set(shader["draws"]) != set(OUTCOMES):
            raise ControlUnavailable(
                f"{url} named outcomes {sorted(shader['draws'])}, and this client knows "
                f"{sorted(OUTCOMES)}"
            )
        base_hash = int(shader["baseHash"])
        rows[base_hash] = ShaderRow(
            base_hash, {name: int(shader["draws"][name]) for name in OUTCOMES}
        )
    return rows


def window(before: dict[int, ShaderRow], after: dict[int, ShaderRow]) -> list[ShaderRow]:
    """What each shader's draws came to between two reads, most stepped first;
    a shader whose totals went down is refused: the runtime restarted."""
    rows = []
    for base_hash, row in after.items():
        earlier = before.get(base_hash, ShaderRow(base_hash, dict.fromkeys(OUTCOMES, 0)))
        draws = {name: row.draws[name] - earlier.draws[name] for name in OUTCOMES}
        if any(count < 0 for count in draws.values()):
            raise ControlUnavailable(f"shader {base_hash:016x}'s totals went down between reads")
        if any(draws.values()):
            rows.append(ShaderRow(base_hash, draws))
    return sorted(rows, key=lambda row: row.stepped, reverse=True)


def read(port: int = DEFAULT_PORT, timeout: float = 5.0) -> dict[int, ShaderRow]:
    url = f"http://127.0.0.1:{port}/vertices"
    try:
        payload = json.loads(request_bytes("GET", "/vertices", port, timeout))
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )
    return parse(url, payload)
