#!/usr/bin/env python
"""Name the camera transform in a uniform capture, or say why there is none.

Reports every frame-constant 3x4 whose rotation is orthonormal, ranked by how
many unrelated shaders carry the same values in the same frame. That sharing
is what separates a view from a moving object's world matrix.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from wiiuport.paths import find_layout
from wiiuport.transforms import find_transform_candidates, group_by_value
from wiiuport.uniformcapture import CaptureUnreadable, read_records


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", nargs="?", type=Path, help="path to uniform-capture.bin")
    parser.add_argument("--limit", type=int, default=10, help="how many candidates to show")
    args = parser.parse_args(argv)

    path = args.capture
    if path is None:
        path = find_layout().activity_dir("uniform-capture") / "uniform-capture.bin"

    try:
        records = list(read_records(path))
    except CaptureUnreadable as unreadable:
        print(f"refused: {unreadable}", file=sys.stderr)
        return 1

    shaders = len({r.shader for r in records})
    frames = len({r.frame for r in records})
    candidates = find_transform_candidates(records)
    shared = [c for c in candidates if c.is_shared]
    print(
        f"{len(records)} draws across {shaders} shaders over {frames} frames in {path}\n"
        f"{len(candidates)} frame-constant 3x4 transforms with an orthonormal rotation, "
        f"{len(shared)} of them shared between shaders"
    )
    if frames < 2:
        print(
            "only one frame was captured, so nothing here is known to change and no "
            "candidate can be told from a constant"
        )
    if not candidates:
        print(
            "no slot holds a frame-constant 3x4 whose rotation is orthonormal. Either the "
            "capture never reached a drawn scene, or this title does not pass its view as a "
            "3x4 in the assembled uniform buffer."
        )
        return 0
    if not shared:
        print(
            "every candidate appears in one shader only, so none is shown to be a view "
            "rather than that object's own transform"
        )
    transforms = group_by_value(records, candidates)
    print(f"they are {len(transforms)} distinct transforms:")
    for transform in transforms[: args.limit]:
        print(f"  {transform.summary}")
    if len(transforms) > args.limit:
        print(f"  ({len(transforms) - args.limit} further transforms not shown)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
