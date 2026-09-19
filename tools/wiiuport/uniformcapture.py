"""Read the runtime's uniform capture and separate frame-constant slots.

The capture file is written by ``LatteUniformCapture`` in the fork: one
self-describing record per draw, holding the assembled uniform buffer after
both uniform modes have converged on it.

The question it answers is which slots hold a camera and which hold per-actor
transforms. Call-shape statistics cannot tell them apart, because both are
matrices written at a similar rate. Values can:

* a **camera** is the same for every draw within one frame, and differs between
  frames when the viewpoint moves;
* an **actor** transform differs between draws within the same frame.

A slot that never changes at all, across frames as well as draws, is neither --
it is a constant, and reporting it as a camera would be the easiest mistake
here to make.
"""

from __future__ import annotations

import struct
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

RECORD_MAGIC = 0x554E4946
"""'UNIF'. Checked on every record so a desynchronised read refuses rather than
returning plausible nonsense."""

_HEADER = struct.Struct("<7I2Q")


class CaptureUnreadable(RuntimeError):
    """The file is absent, truncated, or not a capture. Never reported as empty."""


@dataclass(frozen=True)
class DrawRecord:
    """One draw's assembled uniform buffer, with the layout needed to read it."""

    frame: int
    stage: int
    base_hash: int
    aux_hash: int
    loc_uniform_register: int
    count_uniform_register: int
    loc_remapped: int
    payload: bytes

    @property
    def shader(self) -> tuple[int, int, int]:
        """Same shader and stage means same buffer layout, so grouping happens here."""
        return (self.stage, self.base_hash, self.aux_hash)

    def floats(self) -> tuple[float, ...]:
        return struct.unpack(
            f"<{len(self.payload) // 4}f", self.payload[: len(self.payload) // 4 * 4]
        )


def _as_signed(value: int) -> int:
    """The runtime writes sint32 layout offsets through a uint32 field, so -1
    arrives as 0xFFFFFFFF and must not be read as four billion."""
    return value - (1 << 32) if value >= (1 << 31) else value


def read_records(path: Path) -> Iterator[DrawRecord]:
    """Yield every record, refusing on anything it cannot read in full."""
    if not path.is_file():
        raise CaptureUnreadable(
            f"{path} does not exist, so no draws were inspected. Run the runtime with the "
            "uniform-capture log type enabled first."
        )
    data = path.read_bytes()
    if not data:
        raise CaptureUnreadable(
            f"{path} is empty. The runtime opened it and wrote nothing, which means capture "
            "was enabled but no draw reached the assembly point."
        )
    offset = 0
    index = 0
    while offset < len(data):
        if offset + _HEADER.size > len(data):
            raise CaptureUnreadable(
                f"{path} ends mid-header after {index} complete records; it is truncated."
            )
        fields = _HEADER.unpack_from(data, offset)
        magic, frame, stage, size, loc_reg, count_reg, loc_remapped, base_hash, aux_hash = fields
        if magic != RECORD_MAGIC:
            raise CaptureUnreadable(
                f"{path} record {index} has magic {magic:#x}, expected {RECORD_MAGIC:#x}; "
                "the stream is desynchronised and later records cannot be trusted."
            )
        offset += _HEADER.size
        if offset + size > len(data):
            raise CaptureUnreadable(
                f"{path} record {index} claims {size} payload bytes but only "
                f"{len(data) - offset} remain; it is truncated."
            )
        yield DrawRecord(
            frame=frame,
            stage=stage,
            base_hash=base_hash,
            aux_hash=aux_hash,
            loc_uniform_register=_as_signed(loc_reg),
            count_uniform_register=_as_signed(count_reg),
            loc_remapped=_as_signed(loc_remapped),
            payload=data[offset : offset + size],
        )
        offset += size
        index += 1


@dataclass(frozen=True)
class SlotVerdict:
    """One float offset's behaviour, with the counts behind the classification."""

    offset: int
    draws: int
    distinct_within_frames: int
    distinct_across_frames: int

    @property
    def classification(self) -> str:
        if self.distinct_within_frames > 1:
            return "per-draw"
        if self.distinct_across_frames > 1:
            return "frame-constant"
        return "invariant"


@dataclass(frozen=True)
class ShaderAnalysis:
    """Every slot of one shader, plus the denominators that make it readable."""

    shader: tuple[int, int, int]
    draws: int
    frames: int
    slots: tuple[SlotVerdict, ...]

    def of(self, classification: str) -> tuple[SlotVerdict, ...]:
        return tuple(s for s in self.slots if s.classification == classification)

    @property
    def summary(self) -> str:
        stage, base, aux = self.shader
        return (
            f"stage {stage} shader {base:016x}:{aux:016x}: {self.draws} draws over "
            f"{self.frames} frames, {len(self.slots)} float slots "
            f"({len(self.of('frame-constant'))} frame-constant, "
            f"{len(self.of('per-draw'))} per-draw, {len(self.of('invariant'))} invariant)"
        )


def analyse(records: list[DrawRecord]) -> list[ShaderAnalysis]:
    """Classify every slot of every shader seen, largest shader first."""
    by_shader: dict[tuple[int, int, int], list[DrawRecord]] = {}
    for record in records:
        by_shader.setdefault(record.shader, []).append(record)

    analyses: list[ShaderAnalysis] = []
    for shader, draws in by_shader.items():
        width = min(len(d.payload) // 4 for d in draws)
        frames = sorted({d.frame for d in draws})
        per_frame: dict[int, list[tuple[float, ...]]] = {}
        for draw in draws:
            per_frame.setdefault(draw.frame, []).append(draw.floats())

        slots: list[SlotVerdict] = []
        for offset in range(width):
            within = 1
            frame_values: set[float] = set()
            for values in per_frame.values():
                column = {v[offset] for v in values}
                within = max(within, len(column))
                frame_values |= column
            slots.append(
                SlotVerdict(
                    offset=offset,
                    draws=len(draws),
                    distinct_within_frames=within,
                    distinct_across_frames=len(frame_values),
                )
            )
        analyses.append(ShaderAnalysis(shader, len(draws), len(frames), tuple(slots)))

    analyses.sort(key=lambda a: a.draws, reverse=True)
    return analyses


def rank_for_review(results: list[ShaderAnalysis]) -> list[ShaderAnalysis]:
    """Order shaders so the camera-shaped ones are read first.

    A capture from real gameplay holds hundreds of shaders and any report has
    to elide most of them. Eliding by capture order would hide exactly the
    interesting case, since a frame-constant slot is the one thing the whole
    capture exists to find; the boring case is the shader that has none.
    """
    return sorted(
        results,
        key=lambda r: (-len(r.of("frame-constant")), -r.draws, r.shader),
    )


def without_frame_constant_slots(results: list[ShaderAnalysis]) -> list[ShaderAnalysis]:
    return [r for r in results if not r.of("frame-constant")]


def clear_previous_capture(*paths: Path) -> list[Path]:
    """Remove captures left by an earlier run, returning what was removed.

    Capture runs reuse one fixed scratch path, so a previous run's file sits
    there until something deletes it. Without this a run that captured nothing
    copies the older file out and reports it as its own result -- measured
    once, and a wrong measurement is worse than a missing one.
    """
    removed = []
    for path in paths:
        if path.is_file():
            path.unlink()
            removed.append(path)
    return removed
