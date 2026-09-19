"""Recover camera transforms from a uniform capture by their values.

Slot classification says which slots are constant within a frame; it cannot
say which of those is a camera. Two things separate a camera from a moving
object's world matrix, and both are checked here rather than assumed:

* its rotation is a real rotation -- three orthonormal rows -- which rules out
  slots that merely happen to be frame-constant, such as colours, fog
  parameters and projection terms;
* it is shared by unrelated shaders. Every pass that draws the world is given
  the same view, while an object's transform reaches only the shaders that
  draw that object. This is the discriminator that actually decides it, and a
  candidate found in one shader alone is reported as exactly that.

Matrices are read as three rows of four -- a 3x3 rotation with translation in
the fourth column -- because that is the shape the title was measured to use.
"""

from __future__ import annotations

import math
import struct
from collections import defaultdict
from dataclasses import dataclass

from wiiuport.uniformcapture import DrawRecord, ShaderAnalysis, analyse

ROWS = 3
COLUMNS = 4
MATRIX_FLOATS = ROWS * COLUMNS

DEFAULT_ROTATION_TOLERANCE = 1e-3
"""How far a row norm or row pair may stray from orthonormal. Measured values
sit within 1e-5, so this is loose enough for half precision upstream and far
tighter than anything a colour or projection row reaches by accident."""


def floats_of(record: DrawRecord) -> tuple[float, ...]:
    count = len(record.payload) // 4
    return struct.unpack(f"<{count}f", record.payload[: count * 4])


def rotation_error(values: tuple[float, ...]) -> float:
    """How far the 3x3 part is from orthonormal, as one number.

    Returned rather than thresholded so a near miss can be seen instead of
    silently dropped.
    """
    rows = [values[r * COLUMNS : r * COLUMNS + ROWS] for r in range(ROWS)]
    error = 0.0
    for index, row in enumerate(rows):
        error = max(error, abs(math.sqrt(sum(c * c for c in row)) - 1.0))
        for other in rows[index + 1 :]:
            error = max(error, abs(sum(a * b for a, b in zip(row, other, strict=True))))
    return error


def translation_of(values: tuple[float, ...]) -> tuple[float, float, float]:
    return (values[3], values[7], values[11])


@dataclass(frozen=True)
class TransformCandidate:
    """One 3x4 transform found in one shader's buffer, with what decides it."""

    shader: tuple[int, int, int]
    offset: int
    frames: int
    shaders_sharing: int
    rotation_error: float
    translation_steps: tuple[float, ...]

    @property
    def is_shared(self) -> bool:
        return self.shaders_sharing > 1

    @property
    def mean_step(self) -> float:
        if not self.translation_steps:
            return 0.0
        return sum(self.translation_steps) / len(self.translation_steps)

    @property
    def moves(self) -> bool:
        return self.mean_step > 0.0

    @property
    def summary(self) -> str:
        stage, base, aux = self.shader
        shared = (
            f"shared by {self.shaders_sharing} shaders"
            if self.is_shared
            else "found in this shader only, so not shown to be a view"
        )
        return (
            f"stage {stage} shader {base:016x}:{aux:016x} float {self.offset} "
            f"(byte {self.offset * 4}): {shared}, rotation error {self.rotation_error:.2e}, "
            f"mean translation step {self.mean_step:.1f} over {self.frames} frames"
        )


def _frame_constant_offsets(analysis: ShaderAnalysis) -> set[int]:
    return {slot.offset for slot in analysis.of("frame-constant")}


def _per_draw_offsets(analysis: ShaderAnalysis) -> set[int]:
    return {slot.offset for slot in analysis.of("per-draw")}


def _first_draw_per_shader_frame(
    records: list[DrawRecord],
) -> dict[tuple[int, int, int], dict[int, DrawRecord]]:
    picked: dict[tuple[int, int, int], dict[int, DrawRecord]] = defaultdict(dict)
    for record in records:
        byframe = picked[record.shader]
        if record.frame not in byframe:
            byframe[record.frame] = record
    return picked


def _sharing_count(
    values: tuple[float, ...],
    frame: int,
    picked: dict[tuple[int, int, int], dict[int, DrawRecord]],
    exclude: tuple[int, int, int],
) -> int:
    """How many other shaders carry these same twelve floats in this frame."""
    count = 1
    for shader, byframe in picked.items():
        if shader == exclude or frame not in byframe:
            continue
        other = floats_of(byframe[frame])
        limit = len(other) - MATRIX_FLOATS
        if any(
            all(other[off + k] == values[k] for k in range(MATRIX_FLOATS))
            for off in range(limit + 1)
        ):
            count += 1
    return count


def find_transform_candidates(
    records: list[DrawRecord],
    *,
    rotation_tolerance: float = DEFAULT_ROTATION_TOLERANCE,
) -> list[TransformCandidate]:
    """Every frame-constant 3x4 whose rotation is real, most shared first."""
    picked = _first_draw_per_shader_frame(records)
    candidates: list[TransformCandidate] = []
    for analysis in analyse(records):
        byframe = picked[analysis.shader]
        frames = sorted(byframe)
        if not frames:
            continue
        constant = _frame_constant_offsets(analysis)
        per_draw = _per_draw_offsets(analysis)
        reference_frame = frames[0]
        reference = floats_of(byframe[reference_frame])
        for offset in range(len(reference) - MATRIX_FLOATS + 1):
            span = range(offset, offset + MATRIX_FLOATS)
            # No slot may vary between draws, or this is not one transform
            # shared by the frame. At least one must vary between frames, or
            # it is a constant matrix and nothing distinguishes it from a
            # baked-in identity. Requiring every slot to vary between frames
            # was wrong: a camera that pans without turning leaves its
            # rotation elements untouched, and would never have been found.
            if any(slot in per_draw for slot in span):
                continue
            if not any(slot in constant for slot in span):
                continue
            values = reference[offset : offset + MATRIX_FLOATS]
            error = rotation_error(values)
            if error > rotation_tolerance:
                continue
            steps = []
            previous = translation_of(values)
            for frame in frames[1:]:
                later = floats_of(byframe[frame])[offset : offset + MATRIX_FLOATS]
                current = translation_of(later)
                steps.append(math.dist(current, previous))
                previous = current
            candidates.append(
                TransformCandidate(
                    shader=analysis.shader,
                    offset=offset,
                    frames=len(frames),
                    shaders_sharing=_sharing_count(
                        values, reference_frame, picked, analysis.shader
                    ),
                    rotation_error=error,
                    translation_steps=tuple(steps),
                )
            )
    return sorted(candidates, key=lambda c: (-c.shaders_sharing, c.rotation_error, c.shader))


@dataclass(frozen=True)
class SharedTransform:
    """One distinct transform and every place it was found.

    Grouped by value because the question is how many transforms the frame
    holds, not how many shaders mention them. Listing one camera seventy-two
    times reads as seventy-two findings.
    """

    values: tuple[float, ...]
    sites: tuple[TransformCandidate, ...]

    @property
    def shaders(self) -> int:
        return len(self.sites)

    @property
    def offsets(self) -> tuple[int, ...]:
        return tuple(sorted({site.offset for site in self.sites}))

    @property
    def rotation_error(self) -> float:
        return self.sites[0].rotation_error

    @property
    def mean_step(self) -> float:
        return self.sites[0].mean_step

    @property
    def summary(self) -> str:
        where = "the same offset" if len(self.offsets) == 1 else "differing offsets"
        translation = translation_of(self.values)
        return (
            f"{self.shaders} shaders at {where} {list(self.offsets)}: rotation error "
            f"{self.rotation_error:.2e}, mean translation step {self.mean_step:.1f}, "
            f"translation ({translation[0]:.1f}, {translation[1]:.1f}, {translation[2]:.1f})"
        )


def group_by_value(
    records: list[DrawRecord], candidates: list[TransformCandidate]
) -> list[SharedTransform]:
    picked = _first_draw_per_shader_frame(records)
    grouped: dict[tuple[float, ...], list[TransformCandidate]] = defaultdict(list)
    for candidate in candidates:
        frames = sorted(picked[candidate.shader])
        values = floats_of(picked[candidate.shader][frames[0]])
        grouped[values[candidate.offset : candidate.offset + MATRIX_FLOATS]].append(candidate)
    transforms = [SharedTransform(values=v, sites=tuple(s)) for v, s in grouped.items()]
    return sorted(transforms, key=lambda t: (-t.shaders, t.rotation_error))
