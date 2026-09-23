"""The title's draws no uniform blend can move, read from GET /draws.

A draw whose vertex shader reads no uniforms places its geometry from vertex
data alone. The runtime counts them by vertex shader, with how many of them
read vertex bytes that differ from the same draw a frame before; the difference
between two readings says which ran over a stretch of play, and whether they
are passes whose vertices never change or geometry the title rewrites -- an
effect, which moves at the title's rate however the rest is blended.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, request_bytes, require_fields

# Shaders named in a rendering: the few that decide what the total means.
RANKED_SHADERS = 8


@dataclass(frozen=True)
class ShaderDraws:
    draws: int
    # Draws whose vertex bytes differ from the same draw a frame before, and
    # draws with no same draw a frame before to compare with.
    changed: int
    unmatched: int
    bytesHashed: int

    def minus(self, earlier: ShaderDraws) -> ShaderDraws:
        return ShaderDraws(
            self.draws - earlier.draws,
            self.changed - earlier.changed,
            self.unmatched - earlier.unmatched,
            self.bytesHashed - earlier.bytesHashed,
        )


NO_DRAWS = ShaderDraws(0, 0, 0, 0)


@dataclass(frozen=True)
class DrawsWithoutUniforms:
    prepared: int
    # (base hash, aux hash) of the vertex shader -> its draws.
    byShader: dict[tuple[int, int], ShaderDraws]

    @classmethod
    def parse(cls, url: str, payload: dict) -> DrawsWithoutUniforms:
        require_fields(
            url, payload, ("guestDrawsPrepared", "withoutVertexUniforms"), "a draw count"
        )
        by_shader = {}
        for row in payload["withoutVertexUniforms"]:
            require_fields(
                url,
                row,
                ("baseHash", "auxHash", "draws", "changed", "unmatched", "bytesHashed"),
                "a shader's draws",
            )
            by_shader[(int(row["baseHash"]), int(row["auxHash"]))] = ShaderDraws(
                int(row["draws"]),
                int(row["changed"]),
                int(row["unmatched"]),
                int(row["bytesHashed"]),
            )
        return cls(int(payload["guestDrawsPrepared"]), by_shader)

    @property
    def without(self) -> int:
        return sum(shader.draws for shader in self.byShader.values())

    @property
    def changed(self) -> int:
        return sum(shader.changed for shader in self.byShader.values())

    @property
    def bytesHashed(self) -> int:
        return sum(shader.bytesHashed for shader in self.byShader.values())

    def since(self, earlier: DrawsWithoutUniforms) -> DrawsWithoutUniforms:
        """What was drawn between `earlier` and this reading."""
        counts = {
            shader: draws.minus(earlier.byShader.get(shader, NO_DRAWS))
            for shader, draws in self.byShader.items()
        }
        return DrawsWithoutUniforms(
            self.prepared - earlier.prepared,
            {shader: draws for shader, draws in counts.items() if draws.draws > 0},
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
                f"uniforms ({share:.1f}%), which no blend moves; {self.changed} of those read "
                f"vertex bytes that changed from the frame before "
                f"({self.bytesHashed} bytes compared); under {len(self.byShader)} vertex shaders:"
            )
        ]
        ranked = sorted(self.byShader.items(), key=lambda row: row[1].draws, reverse=True)
        lines += [
            (
                f"  {base:016x}/{aux:016x}: {draws.draws} draws, {draws.changed} changed, "
                f"{draws.unmatched} unmatched"
            )
            for (base, aux), draws in ranked[:RANKED_SHADERS]
        ]
        changing = sorted(self.byShader.items(), key=lambda row: row[1].changed, reverse=True)
        changing = [row for row in changing if row[1].changed > 0]
        lines.append("most changed:" if changing else "most changed: (none)")
        lines += [
            f"  {base:016x}/{aux:016x}: {draws.changed} of {draws.draws} draws changed"
            for (base, aux), draws in changing[:RANKED_SHADERS]
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
