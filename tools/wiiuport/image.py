"""Read a captured frame from the control channel and write it as a PNG.

The channel serves raw RGB behind a small self-describing header, so a body
that is truncated or from a different runtime is refused rather than rendered
as noise at the wrong dimensions. PNG is written here rather than pulled in as
a dependency: it is a container around one zlib stream, and the whole of it is
below.
"""

from __future__ import annotations

import json
import struct
import urllib.error
import urllib.request
import zlib
from dataclasses import dataclass
from pathlib import Path

from wiiuport.control import DEFAULT_PORT, ControlUnavailable

MAGIC = b"WIIUIMG1"
HEADER_BYTES = 16
CHANNELS = 3


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    rgb: bytes

    def png_bytes(self) -> bytes:
        """Encode as a PNG: signature, IHDR, one IDAT, IEND."""
        raw = bytearray()
        stride = self.width * CHANNELS
        for row in range(self.height):
            raw.append(0)  # filter type 0, because the images are compared not shipped
            raw += self.rgb[row * stride : (row + 1) * stride]

        def chunk(kind: bytes, payload: bytes) -> bytes:
            return (
                struct.pack(">I", len(payload))
                + kind
                + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
            )

        header = struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0)
        return (
            b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b"")
        )

    def write_png(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self.png_bytes())
        return path


def decode(body: bytes) -> Image:
    """Refuse anything that is not exactly one whole image."""
    if len(body) < HEADER_BYTES:
        raise ControlUnavailable(
            f"the capture body is {len(body)} bytes, shorter than its {HEADER_BYTES}-byte "
            "header, so nothing was captured and nothing can be read"
        )
    if body[:8] != MAGIC:
        raise ControlUnavailable(
            f"the capture body starts with {body[:8]!r}, not {MAGIC!r}; this runtime and "
            "this client disagree about the framing"
        )
    width, height = struct.unpack("<II", body[8:HEADER_BYTES])
    expected = width * height * CHANNELS
    actual = len(body) - HEADER_BYTES
    if actual != expected:
        raise ControlUnavailable(
            f"the capture claims {width}x{height} ({expected} bytes of RGB) but carries "
            f"{actual}; it is truncated or the dimensions are wrong"
        )
    if expected == 0:
        raise ControlUnavailable(
            f"the capture is {width}x{height}, so it holds no pixels. An image of no size "
            "is not a frame that was captured."
        )
    return Image(width=width, height=height, rgb=body[HEADER_BYTES:])


def read_capture(port: int = DEFAULT_PORT, timeout: float = 20.0, slot: int = 0) -> Image:
    """Fetch a captured frame, refusing by reason rather than empty."""
    url = f"http://127.0.0.1:{port}/capture?slot={slot}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return decode(response.read())
    except urllib.error.HTTPError as refused:
        body = refused.read().decode("utf-8", "replace").strip()
        raise ControlUnavailable(f"{url} was refused ({refused.code}): {body}") from refused
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(
            f"{url} did not answer ({unreachable.reason}). The runtime is not running, "
            "or was started without WIIUPORT_CONTROL_PORT."
        ) from unreachable


def arm_capture(port: int = DEFAULT_PORT, timeout: float = 5.0, slot: int = 0) -> bool:
    url = f"http://127.0.0.1:{port}/capture?slot={slot}"
    request = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status == 200
    except urllib.error.HTTPError as refused:
        raise ControlUnavailable(
            f"{url} refused to arm a capture ({refused.code}); the renderer may not exist yet"
        ) from refused
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(f"{url} did not answer ({unreachable.reason})") from unreachable


def arm_interpolated_frame(port: int = DEFAULT_PORT, t: float = 0.5, timeout: float = 5.0) -> int:
    """Ask the runtime for one frame between the last two: the last frame's
    geometry replayed with the view blended at `t`. Captured into the same two
    slots a null diff uses, so slot 0 is the title's frame and slot 1 the
    interpolated one. Returns how many shaders the blend was written into,
    which is zero only if the runtime armed without finding the view -- and it
    refuses rather than doing that."""
    url = f"http://127.0.0.1:{port}/interpolate?t={t}"
    request = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return int(json.loads(response.read().decode("utf-8"))["slots"])
    except urllib.error.HTTPError as refused:
        body = refused.read().decode("utf-8", "replace").strip()
        raise ControlUnavailable(f"{url} refused ({refused.code}): {body}") from refused
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(f"{url} did not answer ({unreachable.reason})") from unreachable


def arm_null_diff(port: int = DEFAULT_PORT, timeout: float = 5.0, redraw: bool = True) -> bool:
    """Ask the runtime to capture one frame twice: as the title presented it
    and as a replay of that same frame redrew it. Both halves are armed around
    one frame boundary inside the runtime, which is the only place that knows
    where that boundary is."""
    url = f"http://127.0.0.1:{port}/nulldiff?redraw={1 if redraw else 0}"
    request = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status == 200
    except urllib.error.HTTPError as refused:
        body = refused.read().decode("utf-8", "replace").strip()
        raise ControlUnavailable(f"{url} refused ({refused.code}): {body}") from refused
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(f"{url} did not answer ({unreachable.reason})") from unreachable


def bounding_box(before: Image, after: Image) -> str:
    """Where the differing pixels are. A difference spread over the whole
    frame and one confined to a small box are different faults, and the
    numbers alone cannot tell them apart."""
    width = before.width
    stride = width * 3
    minx = miny = None
    maxx = maxy = -1
    touched = 0
    for y in range(before.height):
        row_a = before.rgb[y * stride : (y + 1) * stride]
        row_b = after.rgb[y * stride : (y + 1) * stride]
        if row_a == row_b:
            continue
        touched += 1
        for x in range(width):
            i = x * 3
            if row_a[i : i + 3] != row_b[i : i + 3]:
                minx = x if minx is None else min(minx, x)
                maxx = max(maxx, x)
                miny = y if miny is None else min(miny, y)
                maxy = max(maxy, y)
    if minx is None:
        return "nowhere"
    return (
        f"x {minx}..{maxx}, y {miny}..{maxy} ({maxx - minx + 1}x{maxy - miny + 1}), "
        f"{touched} rows touched"
    )


def compare(before: Image, after: Image) -> tuple[int, int, float]:
    """Differing bytes, the largest single difference, and the mean.

    Reported together because one changed pixel and a different image are
    both "not identical" and nothing else distinguishes them.
    """
    if (before.width, before.height) != (after.width, after.height):
        raise ControlUnavailable(
            f"the two captures are {before.width}x{before.height} and "
            f"{after.width}x{after.height}; a replay that changed the resolution is a "
            "finding in itself and they cannot be compared byte for byte"
        )
    differing = 0
    largest = 0
    total = 0
    for a, b in zip(before.rgb, after.rgb, strict=True):
        delta = abs(a - b)
        if delta:
            differing += 1
            total += delta
            largest = max(largest, delta)
    return differing, largest, total / len(before.rgb)
