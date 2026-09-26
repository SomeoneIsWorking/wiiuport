"""The null case's discriminator, on captures whose answers are known.

Two paints of one tick must be byte-identical, and a paint of the next tick must
differ. Stated that way it is a test; stated as "two frames look the same" it is
an observation, and an observation cannot fail. The renderer here exists to make
each of the three outcomes name itself, and these tests are what make it
trustworthy: the holding case says so, and so do the two ways it can be broken --
a capture path that cannot see the world move, and a same-tick pair that differs.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import paint_run

Capture = paint_run.Capture


def shot(slot: int, paints: int, tick: int, image: bytes) -> Capture:
    return Capture(slot=slot, paints=paints, tick=tick, image=image)


def test_two_paints_of_one_tick_are_reported_as_holding() -> None:
    """The null case: same tick, same bytes. The report says it held rather than
    repeating the numbers and leaving the reading to whoever reads them."""
    first = shot(0, 40, 7, b"abcdefgh")
    second = shot(1, 41, 7, b"abcdefgh")
    report = paint_run.render_captures([first, second])
    assert "the same tick 7" in report, report
    assert "differ" not in report, report


def test_a_moved_tick_with_identical_bytes_is_called_a_failure() -> None:
    """Two captures a tick apart that are identical cannot be the null case
    holding: something did not move. Reported as what it is, because reporting it
    as a pass is exactly how a capture path that reads nothing passes."""
    first = shot(0, 40, 7, b"abcdefgh")
    second = shot(1, 41, 8, b"abcdefgh")
    report = paint_run.render_captures([first, second])
    assert "cannot see the world move" in report, report


def test_one_tick_painting_two_different_pictures_is_called_a_failure() -> None:
    """The other direction: the same world painted twice must be the same
    picture. Two of a tick's paints differing means the paint is not the picture
    it claims to be -- a flip landing between them, or a capture that caught the
    swap."""
    first = shot(0, 40, 7, b"abcdefgh")
    second = shot(1, 41, 7, b"abcdefgX")
    report = paint_run.render_captures([first, second])
    assert "the same world painted twice is not the same picture" in report, report


def test_three_captures_read_as_two_comparisons() -> None:
    """A run takes three: the first two are the null case, the third is the
    control that says the comparison can tell two things apart at all. Both pairs
    are reported, with their own ticks."""
    shots = [
        shot(0, 40, 7, b"abcdefgh"),
        shot(1, 41, 7, b"abcdefgh"),
        shot(2, 42, 8, b"abcdefgX"),
    ]
    report = paint_run.render_captures(shots)
    assert report.count(";") == 1, report
    assert "the same tick 7" in report, report
    assert "ticks 7 then 8" in report, report


def test_one_capture_is_refused_rather_than_compared_with_nothing() -> None:
    """Nothing to compare is said, not treated as agreement."""
    report = paint_run.render_captures([shot(0, 40, 7, b"abcdefgh")])
    assert "nothing was compared" in report, report
