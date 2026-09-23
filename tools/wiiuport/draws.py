"""Whether the title's draws read vertex bytes it rewrote, from GET /draws.

A uniform blend moves what uniforms place. Geometry the title writes into
vertex buffers each frame -- an effect built on the CPU -- steps at the
title's rate however the rest is blended. The runtime hashes each draw's
vertex bytes and counts, by vertex shader, the draws whose bytes no draw of
that shader read the frame before.

Draws whose vertex shader reads no uniforms are counted always: nothing else
moves them. The difference between two readings says which ran over a stretch
of play. Draws that read uniforms are counted over a census of frames asked
for with POST /draws, since hashing all of them costs a few tens of megabytes
a frame.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, request_bytes, require_fields

# Shaders named in a rendering: the few that decide what the total means.
RANKED_SHADERS = 8

Shader = tuple[int, int]


@dataclass(frozen=True)
class ShaderDraws:
    draws: int
    # Draws in a frame after one the shader also drew in, and of them those
    # whose bytes no draw of the shader read in that frame.
    compared: int
    changed: int
    bytesHashed: int

    def minus(self, earlier: ShaderDraws) -> ShaderDraws:
        return ShaderDraws(
            self.draws - earlier.draws,
            self.compared - earlier.compared,
            self.changed - earlier.changed,
            self.bytesHashed - earlier.bytesHashed,
        )


NO_DRAWS = ShaderDraws(0, 0, 0, 0)


def _parse_shaders(url: str, rows: list) -> dict[Shader, ShaderDraws]:
    by_shader = {}
    for row in rows:
        require_fields(
            url,
            row,
            ("baseHash", "auxHash", "draws", "compared", "changed", "bytesHashed"),
            "a shader's draws",
        )
        by_shader[(int(row["baseHash"]), int(row["auxHash"]))] = ShaderDraws(
            int(row["draws"]), int(row["compared"]), int(row["changed"]), int(row["bytesHashed"])
        )
    return by_shader


def _render_shaders(by_shader: dict[Shader, ShaderDraws]) -> list[str]:
    ranked = sorted(by_shader.items(), key=lambda row: row[1].draws, reverse=True)
    lines = [
        f"  {base:016x}/{aux:016x}: {draws.draws} draws, {draws.changed} of "
        f"{draws.compared} compared changed"
        for (base, aux), draws in ranked[:RANKED_SHADERS]
    ]
    changing = sorted(by_shader.items(), key=lambda row: row[1].changed, reverse=True)
    changing = [row for row in changing if row[1].changed > 0]
    lines.append("most changed:" if changing else "most changed: (none)")
    lines += [
        f"  {base:016x}/{aux:016x}: {draws.changed} of {draws.compared} compared draws changed"
        for (base, aux), draws in changing[:RANKED_SHADERS]
    ]
    return lines


def _totals(by_shader: dict[Shader, ShaderDraws]) -> ShaderDraws:
    return ShaderDraws(
        sum(row.draws for row in by_shader.values()),
        sum(row.compared for row in by_shader.values()),
        sum(row.changed for row in by_shader.values()),
        sum(row.bytesHashed for row in by_shader.values()),
    )


@dataclass(frozen=True)
class VertexCensus:
    """The draws that read uniforms, over the frames a census was asked for."""

    framesAsked: int
    framesTaken: int
    byShader: dict[Shader, ShaderDraws]

    @property
    def complete(self) -> bool:
        return self.framesAsked > 0 and self.framesTaken == self.framesAsked

    def render(self) -> str:
        if not self.complete:
            raise ControlUnavailable(
                f"the vertex census took {self.framesTaken} of {self.framesAsked} frames"
            )
        total = _totals(self.byShader)
        if total.compared == 0:
            # A census that compared nothing looked at nothing: "none
            # changed" would be a count of no draws.
            raise ControlUnavailable("the vertex census compared no draw that reads uniforms")
        share = 100 * total.changed / total.compared
        lines = [
            (
                f"over {self.framesTaken} frames, {total.changed} of {total.compared} compared "
                f"draws that read uniforms ({share:.1f}%) read vertex bytes that changed from "
                f"the frame before ({total.bytesHashed} bytes compared); under "
                f"{len(self.byShader)} vertex shaders:"
            )
        ]
        return "\n".join(lines + _render_shaders(self.byShader))


@dataclass(frozen=True)
class Draws:
    prepared: int
    # (base hash, aux hash) of the vertex shader -> its draws without
    # vertex uniforms, since the start.
    withoutUniforms: dict[Shader, ShaderDraws]
    census: VertexCensus

    @classmethod
    def parse(cls, url: str, payload: dict) -> Draws:
        require_fields(
            url, payload, ("guestDrawsPrepared", "withoutVertexUniforms", "census"), "a draw count"
        )
        census = payload["census"]
        require_fields(
            url, census, ("framesAsked", "framesTaken", "withVertexUniforms"), "a vertex census"
        )
        return cls(
            int(payload["guestDrawsPrepared"]),
            _parse_shaders(url, payload["withoutVertexUniforms"]),
            VertexCensus(
                int(census["framesAsked"]),
                int(census["framesTaken"]),
                _parse_shaders(url, census["withVertexUniforms"]),
            ),
        )

    def since(self, earlier: Draws) -> Draws:
        """What was drawn between `earlier` and this reading; the census is
        this reading's."""
        counts = {
            shader: draws.minus(earlier.withoutUniforms.get(shader, NO_DRAWS))
            for shader, draws in self.withoutUniforms.items()
        }
        return Draws(
            self.prepared - earlier.prepared,
            {shader: draws for shader, draws in counts.items() if draws.draws > 0},
            self.census,
        )

    def render(self) -> str:
        if self.prepared == 0:
            # The hook reporting each draw never fired: "none without
            # uniforms" would be a count of nothing looked at.
            raise ControlUnavailable("no draw was reported, so none can be told apart")
        total = _totals(self.withoutUniforms)
        share = 100 * total.draws / self.prepared
        lines = [
            (
                f"the title drew {self.prepared} times, {total.draws} of them with no vertex "
                f"uniforms ({share:.1f}%), which no blend moves; {total.changed} of "
                f"{total.compared} compared read vertex bytes that changed from the frame before "
                f"({total.bytesHashed} bytes compared); under "
                f"{len(self.withoutUniforms)} vertex shaders:"
            )
        ]
        return "\n".join(lines + _render_shaders(self.withoutUniforms))


def _read(method: str, path: str, port: int, timeout: float) -> Draws:
    url = f"http://127.0.0.1:{port}{path}"
    try:
        payload = json.loads(request_bytes(method, path, port, timeout))
    except json.JSONDecodeError as malformed:
        raise ControlUnavailable(f"{url} answered something that is not JSON: {malformed}") from (
            malformed
        )
    return Draws.parse(url, payload)


def read_draws(port: int = DEFAULT_PORT, timeout: float = 5.0) -> Draws:
    return _read("GET", "/draws", port, timeout)


def request_vertex_census(frames: int, port: int = DEFAULT_PORT, timeout: float = 5.0) -> None:
    """Count the next `frames` frames' draws that read uniforms."""
    _read("POST", f"/draws?frames={frames}", port, timeout)


def read_vertex_census(port: int = DEFAULT_PORT, timeout: float = 5.0) -> VertexCensus:
    """The census asked for; refused until it has taken every frame."""
    census = read_draws(port, timeout).census
    if not census.complete:
        raise ControlUnavailable(
            f"the vertex census has taken {census.framesTaken} of {census.framesAsked} frames"
        )
    return census
