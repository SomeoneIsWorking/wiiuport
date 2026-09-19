#!/usr/bin/env python
"""Report which uniform slots behave like a camera and which like an actor.

Reads the capture written by the runtime's LatteUniformCapture and prints, per
shader, the slots that are constant within a frame but change between frames
(camera-shaped) against those that differ between draws (actor-shaped).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from wiiuport.paths import find_layout
from wiiuport.uniformcapture import CaptureUnreadable, ShaderAnalysis, analyse, read_records


def _print_shader(result: ShaderAnalysis, limit: int) -> None:
    print(result.summary)
    if result.frames < 2:
        print(
            "  only one frame was captured, so frame-constant and invariant cannot be "
            "separated; capture more frames before reading anything into this"
        )
    for name in ("frame-constant", "per-draw"):
        slots = result.of(name)
        shown = slots[:limit]
        print(f"  {name}: {len(slots)} slots" + (f", first {len(shown)}:" if shown else ""))
        for slot in shown:
            print(
                f"    float {slot.offset} (byte {slot.offset * 4}): "
                f"{slot.distinct_within_frames} distinct within a frame, "
                f"{slot.distinct_across_frames} across all frames"
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", type=Path, help="path to uniform-capture.bin")
    parser.add_argument("--shaders", type=int, default=5, help="how many shaders to report")
    parser.add_argument("--slots", type=int, default=20, help="slots to list per class")
    args = parser.parse_args(argv)

    path = args.capture
    if path is None:
        path = find_layout().activity_dir("uniform-capture") / "uniform-capture.bin"

    try:
        records = list(read_records(path))
    except CaptureUnreadable as unreadable:
        print(f"refused: {unreadable}", file=sys.stderr)
        return 1

    results = analyse(records)
    print(f"{len(records)} draws across {len(results)} shaders in {path}")
    for result in results[: args.shaders]:
        _print_shader(result, args.slots)
    if len(results) > args.shaders:
        print(f"({len(results) - args.shaders} further shaders not shown)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
