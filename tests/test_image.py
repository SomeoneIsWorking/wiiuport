"""The capture framing must refuse what it cannot read in full."""

from __future__ import annotations

import struct
import zlib

import pytest
from wiiuport.image import HEADER_BYTES, MAGIC, Image, decode

from wiiuport.control import ControlUnavailable


def framed(width: int, height: int, payload: bytes) -> bytes:
    return MAGIC + struct.pack("<II", width, height) + payload


def test_a_whole_image_decodes():
    image = decode(framed(2, 1, bytes([1, 2, 3, 4, 5, 6])))
    assert (image.width, image.height) == (2, 1)
    assert image.rgb == bytes([1, 2, 3, 4, 5, 6])


def test_a_truncated_body_is_refused_by_size():
    with pytest.raises(ControlUnavailable, match="truncated or the dimensions are wrong"):
        decode(framed(2, 1, bytes([1, 2, 3])))


def test_a_foreign_framing_is_refused_rather_than_misread():
    with pytest.raises(ControlUnavailable, match="disagree about the framing"):
        decode(b"OTHERMAG" + struct.pack("<II", 1, 1) + bytes(3))


def test_an_image_of_no_size_is_not_a_capture():
    with pytest.raises(ControlUnavailable, match="not a frame that was captured"):
        decode(framed(0, 0, b""))


def test_a_body_shorter_than_its_header_is_refused():
    with pytest.raises(ControlUnavailable, match="shorter than its"):
        decode(MAGIC[:4])


def test_the_png_is_a_png_that_decodes_to_the_same_pixels():
    # Written by hand, so it is checked by rebuilding the pixels from the
    # encoded bytes rather than by trusting the writer that made them.
    pixels = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9])
    png = Image(width=2, height=2, rgb=pixels).png_bytes()
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    assert png[12:16] == b"IHDR"
    assert struct.unpack(">II", png[16:24]) == (2, 2)
    start = png.index(b"IDAT") + 4
    length = struct.unpack(">I", png[start - 8 : start - 4])[0]
    raw = zlib.decompress(png[start : start + length])
    # One filter byte per row, filter 0, then the row's pixels unchanged.
    assert raw == bytes([0]) + pixels[:6] + bytes([0]) + pixels[6:]
    assert png.endswith(b"IEND" + struct.pack(">I", zlib.crc32(b"IEND") & 0xFFFFFFFF))


def test_the_header_length_is_the_one_the_runtime_writes():
    assert HEADER_BYTES == 16
