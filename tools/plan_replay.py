#!/usr/bin/env python3
"""Plan a kept recordings snapshot again through the shipping object planner.

A run on the title lands in a different scene each time, and planning costs
several times more in one scene than another, so a change to the planner is
measured on the same frames: continuous_run.py keeps its last snapshot as
recordings.bin, and this plans it again offline -- same counts every time, the
time taken as the fastest of several repeats.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from wiiuport.cxxtests import CxxTestsUnavailable
from wiiuport.paths import find_layout
from wiiuport.planreplay import PlanReplayFailed, plan_again


def main(argv: list[str] | None = None) -> int:
    layout = find_layout()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "recordings",
        type=Path,
        nargs="?",
        default=layout.root / "scratch" / "continuous-run" / "recordings.bin",
        help="a WIIUREC1 snapshot; defaults to the last continuous run's",
    )
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args(argv)
    try:
        report = plan_again(layout, args.recordings, args.repeats)
    except (PlanReplayFailed, CxxTestsUnavailable) as failure:
        print(failure, file=sys.stderr)
        return 1
    print(report.render())
    return 0


if __name__ == "__main__":
    sys.exit(main())
