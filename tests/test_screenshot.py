"""A photograph of a display must be refused unless it is one whole image.

The uniformity measure is the reason this module exists: a window that drew
nothing and a window that drew the setup screen are the same over the control
channel, and differ only in pixels.
"""

from __future__ import annotations

import pytest
from wiiuport.image import Image
from wiiuport.screenshot import ScreenshotUnavailable, decode_ppm, spread


def ppm(width: int, height: int, payload: bytes, header: bytes = b"P6") -> bytes:
    return header + f" {width} {height} 255\n".encode() + payload


def test_a_whole_ppm_decodes():
    image = decode_ppm(ppm(2, 1, bytes([1, 2, 3, 4, 5, 6])))
    assert (image.width, image.height) == (2, 1)
    assert image.rgb == bytes([1, 2, 3, 4, 5, 6])


def test_comments_and_spread_out_whitespace_are_read():
    body = b"P6\n# written by something helpful\n2\n1\n255\n" + bytes(6)
    assert decode_ppm(body).width == 2


def test_a_truncated_screenshot_is_refused_by_size():
    with pytest.raises(ScreenshotUnavailable, match="truncated"):
        decode_ppm(ppm(2, 1, bytes([1, 2, 3])))


def test_a_foreign_format_is_refused_rather_than_misread():
    with pytest.raises(ScreenshotUnavailable, match="not a binary PPM"):
        decode_ppm(ppm(1, 1, bytes(3), header=b"P3"))


def test_sixteen_bit_output_is_refused_by_what_it_is():
    with pytest.raises(ScreenshotUnavailable, match="65535 levels per channel"):
        decode_ppm(b"P6 1 1 65535\n" + bytes(6))


def test_a_header_that_ends_early_is_refused():
    with pytest.raises(ScreenshotUnavailable, match="header ends early"):
        decode_ppm(b"P6 2 1")


def test_a_flat_image_is_one_colour_covering_everything():
    measured = spread(Image(width=2, height=2, rgb=bytes([7, 7, 7]) * 4))
    assert measured.distinct_colours == 1
    assert measured.dominant_share == 1.0
    assert measured.pixels == 4


def test_a_drawn_image_spreads_across_colours():
    rgb = bytes([0, 0, 0]) + bytes([1, 1, 1]) + bytes([2, 2, 2]) + bytes([0, 0, 0])
    measured = spread(Image(width=2, height=2, rgb=rgb))
    assert measured.distinct_colours == 3
    assert measured.dominant_share == 0.5
