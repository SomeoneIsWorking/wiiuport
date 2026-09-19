"""Recovering a camera from captured values.

Every test here builds records whose answer is known, including the case
where there is no camera at all: a finder that cannot come back empty would
report one from any capture handed to it.
"""

from __future__ import annotations

import math
import struct

from wiiuport.transforms import (
    find_transform_candidates,
    group_by_value,
    rotation_error,
    translation_of,
)
from wiiuport.uniformcapture import DrawRecord

IDENTITY = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0)


def _turned(angle: float, translation: tuple[float, float, float]) -> tuple[float, ...]:
    c, s = math.cos(angle), math.sin(angle)
    return (
        c, -s, 0.0, translation[0],
        s, c, 0.0, translation[1],
        0.0, 0.0, 1.0, translation[2],
    )  # fmt: skip


def _record(
    frame: int, base_hash: int, values: tuple[float, ...], offset: int, width: int
) -> DrawRecord:
    floats = [0.0] * width
    floats[offset : offset + len(values)] = values
    return DrawRecord(
        frame=frame,
        stage=0,
        base_hash=base_hash,
        aux_hash=0,
        loc_uniform_register=-1,
        count_uniform_register=0,
        loc_remapped=0,
        payload=struct.pack(f"<{width}f", *floats),
    )


def test_a_rotation_has_no_error_and_a_scale_matrix_does() -> None:
    assert rotation_error(IDENTITY) < 1e-9
    doubled = tuple(v * 2 for v in IDENTITY)
    assert rotation_error(doubled) > 0.9


def test_translation_is_the_fourth_column() -> None:
    assert translation_of(_turned(0.3, (7.0, 8.0, 9.0))) == (7.0, 8.0, 9.0)


def test_a_view_shared_by_two_shaders_is_found_in_both() -> None:
    records = []
    for frame in (0, 1, 2):
        view = _turned(0.01 * frame, (100.0 * frame, 0.0, 0.0))
        # Two draws per shader per frame, so frame-constant is a real finding
        # and not an artefact of there being one sample.
        for draw in range(2):
            records.append(_record(frame, 0xAAAA, view, offset=4, width=24))
            records.append(_record(frame, 0xBBBB, view, offset=12, width=24))
            assert draw in (0, 1)
    candidates = find_transform_candidates(records)
    assert {c.shader[1] for c in candidates} == {0xAAAA, 0xBBBB}
    assert all(c.shaders_sharing == 2 for c in candidates)
    assert all(c.is_shared for c in candidates)
    assert all(c.moves for c in candidates)


def test_a_transform_in_one_shader_alone_is_not_shown_to_be_a_view() -> None:
    records = [
        _record(frame, 0xAAAA, _turned(0.01 * frame, (5.0 * frame, 0.0, 0.0)), 0, 12)
        for frame in (0, 1, 2)
        for _ in range(2)
    ]
    candidates = find_transform_candidates(records)
    assert [c.shaders_sharing for c in candidates] == [1]
    assert not candidates[0].is_shared


def test_a_capture_with_no_rotation_anywhere_finds_nothing() -> None:
    """The negative that matters. Values that change but never form a
    rotation must come back empty rather than as a best guess."""
    records = [
        _record(frame, 0xAAAA, tuple(float(i + frame) for i in range(12)), 0, 12)
        for frame in (0, 1, 2)
        for _ in range(2)
    ]
    assert find_transform_candidates(records) == []


def test_a_matrix_that_never_changes_at_all_is_not_reported() -> None:
    """An identity that is written every frame is a constant, not a view. It
    is the easiest false positive available here, since it passes the
    orthonormality test perfectly."""
    records = [_record(frame, 0xAAAA, IDENTITY, 0, 12) for frame in (0, 1, 2) for _ in range(2)]
    assert find_transform_candidates(records) == []


def test_a_camera_that_pans_without_turning_is_still_found() -> None:
    """Its rotation elements never change, so requiring every slot of the
    window to vary between frames would lose it."""
    records = [
        _record(frame, shader, _turned(0.0, (100.0 * frame, 0.0, 0.0)), 0, 12)
        for frame in (0, 1, 2)
        for _ in range(2)
        for shader in (0xAAAA, 0xBBBB)
    ]
    candidates = find_transform_candidates(records)
    assert len(candidates) == 2
    assert all(c.is_shared and c.moves for c in candidates)


def test_the_same_values_in_two_shaders_group_as_one_transform() -> None:
    records = []
    for frame in (0, 1):
        view = _turned(0.01 * frame, (100.0 * frame, 0.0, 0.0))
        for _ in range(2):
            records.append(_record(frame, 0xAAAA, view, offset=4, width=24))
            records.append(_record(frame, 0xBBBB, view, offset=12, width=24))
    grouped = group_by_value(records, find_transform_candidates(records))
    assert len(grouped) == 1
    assert grouped[0].shaders == 2
    assert grouped[0].offsets == (4, 12)
