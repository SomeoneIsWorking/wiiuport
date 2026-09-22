"""Photograph an offscreen display, so "shown" can be told from "blank".

A screen that reports itself as up and draws nothing looks identical over the
control channel. This reads the pixels the display actually holds, and the
uniformity test is the point: an all-one-colour capture is what a failed
renderer, a missing font, and an unmapped window all produce.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass

from wiiuport.image import CHANNELS, Image


class ScreenshotUnavailable(RuntimeError):
    """The display could not be photographed, named precisely."""


@dataclass(frozen=True)
class Spread:
    """How much of the captured image is not one flat colour."""

    distinct_colours: int
    dominant_share: float
    pixels: int

    def render(self) -> str:
        return (
            f"{self.pixels} pixels in {self.distinct_colours} colours; "
            f"the most common covers {self.dominant_share:.1%}"
        )


def capture_display(display: int, timeout: float = 20.0) -> Image:
    """The root window of an X display, as RGB."""
    tool = shutil.which("import")
    if tool is None:
        raise ScreenshotUnavailable(
            "ImageMagick's `import` is not installed, so an offscreen display cannot be "
            "photographed. Install it:\n  sudo dnf install ImageMagick"
        )
    finished = subprocess.run(
        # -depth 8 because ImageMagick writes 16-bit PPM by default, which is
        # not what any of this reads.
        [tool, "-display", f":{display}", "-window", "root", "-depth", "8", "ppm:-"],
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if finished.returncode != 0:
        reason = finished.stderr.decode("utf-8", "replace").strip()
        raise ScreenshotUnavailable(f"display :{display} could not be read: {reason}")
    return decode_ppm(finished.stdout)


def spread(image: Image) -> Spread:
    """Count the colours, because a flat image is the failure this looks for."""
    counts: dict[bytes, int] = {}
    for start in range(0, len(image.rgb), CHANNELS):
        pixel = image.rgb[start : start + CHANNELS]
        counts[pixel] = counts.get(pixel, 0) + 1
    pixels = image.width * image.height
    dominant = max(counts.values()) if counts else 0
    return Spread(
        distinct_colours=len(counts),
        dominant_share=dominant / pixels if pixels else 0.0,
        pixels=pixels,
    )


def decode_ppm(body: bytes) -> Image:
    """Binary PPM (P6) only, refusing anything else by what it actually saw."""
    fields: list[bytes] = []
    offset = 0
    while len(fields) < 4:
        while offset < len(body) and body[offset : offset + 1].isspace():
            offset += 1
        if offset < len(body) and body[offset : offset + 1] == b"#":
            while offset < len(body) and body[offset : offset + 1] not in (b"\n", b"\r"):
                offset += 1
            continue
        start = offset
        while offset < len(body) and not body[offset : offset + 1].isspace():
            offset += 1
        if start == offset:
            raise ScreenshotUnavailable(
                f"the screenshot is {len(body)} bytes and its header ends early; "
                "it is not a binary PPM"
            )
        fields.append(body[start:offset])
    magic, width_text, height_text, depth_text = fields
    if magic != b"P6":
        raise ScreenshotUnavailable(f"the screenshot starts with {magic!r}, not a binary PPM")
    if depth_text != b"255":
        raise ScreenshotUnavailable(
            f"the screenshot is {depth_text.decode()} levels per channel; only 8-bit is read"
        )
    width = int(width_text)
    height = int(height_text)
    pixels = body[offset + 1 :]
    expected = width * height * CHANNELS
    if len(pixels) != expected:
        raise ScreenshotUnavailable(
            f"the screenshot claims {width}x{height} ({expected} bytes) but carries "
            f"{len(pixels)}; it is truncated"
        )
    return Image(width=width, height=height, rgb=pixels)
