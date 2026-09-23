"""A restore check passes only on an exact restore and a control that differs."""

from __future__ import annotations

import pytest
from wiiuport.image import Image
from wiiuport.restore_check import comparison, judge


def image(*pixels: int) -> Image:
    return Image(width=len(pixels), height=1, rgb=bytes(v for p in pixels for v in (p, p, p)))


def test_an_exact_restore_against_a_differing_control_passes():
    restored = comparison(image(1, 2, 3), image(1, 2, 3), in_between=False)
    control = comparison(image(1, 2, 3), image(1, 9, 3), in_between=True)
    assert judge(restored, control) == []
    assert "3 of 9 bytes differ" in control.render()


def test_a_frame_that_did_not_come_back_fails_and_says_where():
    restored = comparison(image(1, 2, 3), image(1, 2, 4), in_between=False)
    control = comparison(image(1, 2, 3), image(9, 2, 3), in_between=True)
    (problem,) = judge(restored, control)
    assert "did not come back: 3 bytes differ at x 2..2" in problem


def test_a_control_identical_to_the_title_fails_the_check():
    restored = comparison(image(1, 2), image(1, 2), in_between=False)
    control = comparison(image(1, 2), image(1, 2), in_between=True)
    (problem,) = judge(restored, control)
    assert "cannot tell a frame that came back" in problem


def test_the_two_comparisons_cannot_be_swapped():
    restored = comparison(image(1), image(1), in_between=False)
    control = comparison(image(1), image(2), in_between=True)
    with pytest.raises(ValueError, match="restored comparison first"):
        judge(control, restored)
