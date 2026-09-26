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


def test_one_moving_range_is_named_with_its_bytes() -> None:
    """A blend writes a range, so one run of moving floats is the answer: which
    bytes, how many, stated as bytes because that is what a write takes."""
    before = block_pose.floats(dump(rows_layout(0.5, 1.0, 2.0, 3.0) + [9.0, 9.0, 9.0, 9.0]))
    after = block_pose.floats(dump(rows_layout(0.52, 1.5, 2.0, 3.0) + [9.0, 9.0, 9.0, 9.0]))
    differing = [i for i, (a, b) in enumerate(zip(before, after, strict=True)) if a != b]
    runs = block_pose.clusters(differing)
    # A rotation by a small angle moves four of the six floats its layout uses --
    # the two cosines and the two sines -- and the translation moves a fifth. The
    # ones between them (the zeros of the third row) do not move, and a run with a
    # gap of four spans them, which is what a stride would otherwise split.
    assert runs == [(0, 5)], runs
    first, last = runs[0]
    assert (first * 4, last * 4 + 3) == (0, 23), runs


def test_several_moving_ranges_are_kept_apart_and_not_merged() -> None:
    """Two things that move per tick are two things, and the gap between them is
    what says so. Merging them into one range would let a blend write over a
    counter it did not mean to."""
    base = rows_layout(0.5, 1.0, 2.0, 3.0)
    before = block_pose.floats(dump(base + [0.25, 0.5, 0.75, 1.0, 0.1, 0.2, 0.3, 0.4]))
    moved = rows_layout(0.52, 1.5, 2.0, 3.0)
    after = block_pose.floats(dump(moved + [0.26, 0.5, 0.75, 1.0, 0.1, 0.2, 0.3, 0.4]))
    differing = [i for i, (a, b) in enumerate(zip(before, after, strict=True)) if a != b]
    runs = block_pose.clusters(differing, gap=4)
    assert runs == [(0, 5), (12, 12)], runs
    # A hole of exactly `gap` floats is still one run and one more is two. That
    # tolerance is what stops a transform's own rows being split by the floats
    # between them: a rotation moves its four cosines and sines and not the zeros
    # of the third row, and a stride would cut it into pieces.
    assert block_pose.clusters([0, 1, 5], gap=4) == [(0, 5)]
    assert block_pose.clusters([0, 1, 6], gap=4) == [(0, 1), (6, 6)]


def test_nothing_moving_is_no_rather_than_an_empty_range() -> None:
    """A block that did not move has no range, and saying so in words beats an
    empty list a reader has to interpret as success."""
    assert block_pose.clusters([]) == []
    assert block_pose.rigid_runs([], []) == []


def test_the_rigid_run_is_the_one_that_looks_like_a_transform() -> None:
    """Of the runs that moved, the one that is a transform where it starts is
    named as such -- the other runs are a colour and a counter, and the report
    says which is which rather than counting them together."""
    before = block_pose.floats(dump(rows_layout(0.5, 1.0, 2.0, 3.0) + [0.25, 0.5, 0.75, 1.0]))
    after = block_pose.floats(dump(rows_layout(0.52, 1.5, 2.0, 3.0) + [0.26, 0.5, 0.75, 1.0]))
    differing = [i for i, (a, b) in enumerate(zip(before, after, strict=True)) if a != b]
    runs = block_pose.clusters(differing, gap=4)
    transforms = block_pose.rigid_runs(differing, after)
    assert runs == [(0, 5), (12, 12)], runs
    # The colour that moved is a single float, and a rigid transform needs
    # twelve, so it is not one and is not reported as one.
    assert transforms == [(0, 5)], transforms
