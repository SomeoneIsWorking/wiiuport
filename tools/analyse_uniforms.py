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
from wiiuport.uniformcapture import (
    CaptureUnreadable,
    ShaderAnalysis,
    analyse,
    rank_for_review,
    read_records,
    without_frame_constant_slots,
)


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
    ranked = rank_for_review(results)
    dull = without_frame_constant_slots(ranked)
    interesting = [r for r in ranked if r.of("frame-constant")]
    print(
        f"{len(records)} draws across {len(results)} shaders in {path}: "
        f"{len(interesting)} with frame-constant slots, {len(dull)} without"
    )
    if not interesting:
        print(
            "no shader holds a slot that is constant within a frame and changes between "
            "frames, so no camera is visible in this capture. Either the window did not "
            "reach a drawn scene, or the view did not move across the frames captured."
        )
    # Every shader with a frame-constant slot is shown; only the dull tail is
    # capped, because the cap exists to keep the report readable and not to
    # decide what is in it.
    shown = interesting + dull[: args.shaders]
    for result in shown:
        _print_shader(result, args.slots)
    if len(shown) < len(ranked):
        print(f"({len(ranked) - len(shown)} further shaders without frame-constant slots)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
