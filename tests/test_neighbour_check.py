"""The neighbour judge must pass a frame between its neighbours and nothing else."""

from __future__ import annotations

from wiiuport.image import Image
from wiiuport.neighbour_check import judge, neighbours


def image(*values: int) -> Image:
    return Image(width=len(values), height=1, rgb=bytes(v for v in values for _ in range(3)))


def test_a_frame_half_way_between_its_neighbours_passes():
    assert (
        judge(neighbours(image(0, 0, 100, 100), image(0, 50, 100, 100), image(0, 100, 100, 100)))
        == []
    )


def test_a_frame_that_repeats_a_neighbour_fails():
    problems = judge(neighbours(image(0, 0), image(0, 0), image(0, 100)))
    assert any("repeats the title's frame before" in p for p in problems)


def test_a_frame_drawn_elsewhere_fails_though_it_differs_from_both():
    problems = judge(neighbours(image(0, 0, 0), image(200, 0, 50), image(0, 100, 0)))
    assert any("from the frame before" in p for p in problems)
    assert any("from the frame after" in p for p in problems)


def test_a_scene_that_held_still_is_not_judged():
    problems = judge(neighbours(image(7, 7), image(7, 7), image(7, 7)))
    assert problems and "held still" in problems[0]
