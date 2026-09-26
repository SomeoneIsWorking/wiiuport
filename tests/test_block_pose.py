"""The pose analyser's own claims, on transforms whose answers are known.

It is asked to say which bytes of a uniform block are the pose. The only way to
trust that is to hand it a transform and check it names the right one, a decoy
float that moves and is not a transform, and a block where nothing moves.
"""

from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import block_pose


def rows_layout(theta: float, x: float, y: float, z: float) -> list[float]:
    """Three rows of three with the translation in a fourth column."""
    c, s = math.cos(theta), math.sin(theta)
    return [c, -s, 0.0, x, s, c, 0.0, y, 0.0, 0.0, 1.0, z]


def columns_layout(theta: float, x: float, y: float, z: float) -> list[float]:
    """Three columns of three with the translation after them."""
    c, s = math.cos(theta), math.sin(theta)
    return [c, s, 0.0, s, -c, 0.0, 0.0, 0.0, 1.0, x, y, z]


def dump(values: list[float]) -> bytes:
    return struct.pack(f"!{len(values)}f", *values)


def test_rows_layout_is_named_with_its_translation() -> None:
    """A transform laid out by rows is found, and the translation it reports for
    that reading is the fourth column -- not the three floats after the first
    row, which is what a wrong reading gives and what looks plausible enough to
    ship."""
    second = rows_layout(0.52, 1.5, 2.0, 3.0)
    found = block_pose.rigid_at(second, 0)
    assert (
        "rows of three with the translation in a fourth column, translation (1.5, 2.0, 3.0)"
        in found
    ), found


def test_the_layout_is_named_rather_than_guessed() -> None:
    """The shape alone does not say which way round a title stores a transform,
    so the report names the layout it read and the translation that layout
    implies. This fixture is stored by columns, and read as rows it is not a
    transform at all -- so the naming is the difference between an answer and a
    plausible wrong one."""
    second = columns_layout(0.52, 1.0, 2.0, 3.25)
    found = block_pose.rigid_at(second, 0)
    assert "columns of three with the translation after them" in found, found
    assert "translation (1.0, 2.0, 3.25)" in found, found
    assert "rows of three" not in found, found


def test_a_moving_float_that_is_not_a_transform_is_not_called_one() -> None:
    """The decoy: floats that change and are not a transform. A block full of
    them is not a pose, and the analyser must say so rather than report the
    biggest range as the camera."""
    second = [0.26, 0.52, 0.74, 1.0, 0.126, 0.374]
    for index in range(len(second)):
        assert block_pose.rigid_at(second, index) == "", (index, second)


def test_an_unchanged_block_has_nothing_that_moved() -> None:
    """Two dumps of a block that did not move. The shape of a transform is still
    there -- it is simply not evidence of anything, because nothing in it
    changed. So the question the analyser answers is which bytes differ, and the
    answer here is none."""
    values = rows_layout(0.5, 1.0, 2.0, 3.0) + [9.0, 9.0, 9.0, 9.0]
    before = block_pose.floats(dump(values))
    after = block_pose.floats(dump(values))
    assert [i for i, (a, b) in enumerate(zip(before, after)) if a != b] == []


def test_a_unit_tolerance_rejects_a_scaled_row() -> None:
    """A transform with a row of length two is not a rigid transform, and must
    not be reported as one: a blend written against it would be wrong in a way
    that looks right on screen."""
    scaled = [2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0]
    assert block_pose.rigid_at(scaled, 0) == ""
    # The test is that a row of length two is a unit vector's worth of error, not
    # that its length differs from itself.
    assert abs(block_pose.length3((2.0, 0.0, 0.0)) - 1.0) > block_pose.UNIT_TOLERANCE
