"""The title's draws no uniform blend can move, read from GET /draws.

A draw whose vertex shader reads no uniforms places its geometry from vertex
data alone. The runtime counts them by vertex shader; the difference between
two readings says which ran over a stretch of play, and whether they are a few
full-screen passes, which never move, or something that does.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, request_bytes, require_fields

# Shaders named in a rendering: the few that decide what the total means.
RANKED_SHADERS = 8


@dataclass(frozen=True)
class DrawsWithoutUniforms:
    prepared: int
    # (base hash, aux hash) of the vertex shader -> draws.
    byShader: dict[tuple[int, int], int]

    @classmethod
    def parse(cls, url: str, payload: dict) -> DrawsWithoutUniforms:
        require_fields(
            url, payload, ("guestDrawsPrepared", "withoutVertexUniforms"), "a draw count"
        )
        by_shader = {}
        for row in payload["withoutVertexUniforms"]:
            require_fields(url, row, ("baseHash", "auxHash", "draws"), "a shader's draws")
            by_shader[(int(row["baseHash"]), int(row["auxHash"]))] = int(row["draws"])
        return cls(int(payload["guestDrawsPrepared"]), by_shader)

    @property
    def without(self) -> int:
        return sum(self.byShader.values())

    def since(self, earlier: DrawsWithoutUniforms) -> DrawsWithoutUniforms:
        """What was drawn between `earlier` and this reading."""
        counts = {
            shader: draws - earlier.byShader.get(shader, 0)
            for shader, draws in self.byShader.items()
        }
        return DrawsWithoutUniforms(
            self.prepared - earlier.prepared,
            {shader: draws for shader, draws in counts.items() if draws > 0},
        )

    def render(self) -> str:
        if self.prepared == 0:
            # The hook reporting each draw never fired: "none without
            # uniforms" would be a count of nothing looked at.
            raise ControlUnavailable("no draw was reported, so none can be told apart")
        share = 100 * self.without / self.prepared
        lines = [
            (
                f"the title drew {self.prepared} times, {self.without} of them with no vertex "
                f"uniforms ({share:.1f}%), which no blend moves; "
                f"under {len(self.byShader)} vertex shaders:"
            )
        ]
        ranked = sorted(self.byShader.items(), key=lambda row: row[1], reverse=True)
        lines += [
            f"  {base:016x}/{aux:016x}: {draws} draws"
            for (base, aux), draws in ranked[:RANKED_SHADERS]
        ]
        return "\n".join(lines)


def read_draws(port: int = DEFAULT_PORT, timeout: float = 5.0) -> DrawsWithoutUniforms:
    url = f"http://127.0.0.1:{port}/draws"
    try:
        payload = json.loads(request_bytes("GET", "/draws", port, timeout))
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )
    return DrawsWithoutUniforms.parse(url, payload)
