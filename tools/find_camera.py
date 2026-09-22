#!/usr/bin/env python3
"""Ask the running title which of its transforms behaves like a camera.

The search runs inside the runtime, not here: the view has to be located at
run time by what its values do, because the offset differs per shader and does
not survive a shader cache change. This tool launches the title, lets it reach
a drawn scene, and reports what the runtime found -- including, when it found
nothing, what it looked at.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.headless import HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, read_transforms, runtime_env


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--settle", type=int, default=90, help="seconds to watch before reporting")
    args = parser.parse_args(argv)

    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2

    layout = find_layout()
    binary = layout.shell_binary
    if not binary.is_file():
        print(f"refused: no runtime at {binary}. Build it first.", file=sys.stderr)
        return 2

    session = HeadlessSession(
        layout=layout,
        activity="camera-search",
        runtime_env=runtime_env(args.port),
    )
    report = None
    with session:
        session.prepare(keys_source=keys)
        with session.launch(layout.shell_command(game)) as running:
            deadline = time.monotonic() + args.settle
            while time.monotonic() < deadline and running.poll() is None:
                time.sleep(10)
                try:
                    report = read_transforms(args.port)
                except ControlUnavailable:
                    continue
            exit_code = running.poll()

    if report is None:
        print(
            "refused: the channel never answered, so the search was never read. The "
            "runtime is not running, or was started without a control port.",
            file=sys.stderr,
        )
        return 1
    print(report.render())
    print(f"process exit {exit_code}")
    if report.framesObserved == 0:
        print(
            "refused: the runtime watched no complete frame, so nothing was searched.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
