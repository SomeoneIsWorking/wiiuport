#!/usr/bin/env python
"""Drive one offscreen capture run and report what it recorded.

Maintainer tool. It never opens a window, never touches the operator's Cemu
installation, and never routes through run.sh.

The disc image is supplied by the caller and is never recorded in this
repository: it is the user's own copy of a copyrighted title.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

from wiiuport.headless import HeadlessSession, LogType, log_flags
from wiiuport.paths import find_layout

ENV_GAME = "WIIUPORT_GAME"
CAPTURE_NAME = "uniform-capture.bin"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help=f"disc image; defaults to ${ENV_GAME}")
    parser.add_argument("--keys", type=Path, default=Path.home() / ".local/share/Cemu/keys.txt")
    parser.add_argument("--seconds", type=int, default=120, help="how long to let it run")
    args = parser.parse_args(argv)

    game = args.game or (Path(os.environ[ENV_GAME]) if ENV_GAME in os.environ else None)
    if game is None:
        print(
            f"refused: no disc image given. Pass --game or set {ENV_GAME}. This tool does "
            "not guess a path, and the image is never stored in this repository.",
            file=sys.stderr,
        )
        return 2
    if not game.is_file():
        print(f"refused: {game} is not a file", file=sys.stderr)
        return 2

    layout = find_layout()
    binary = layout.cemu_binary
    if not binary.is_file():
        print(
            f"refused: no runtime at {binary}. Build it, or download the CI artifact.",
            file=sys.stderr,
        )
        return 2

    session = HeadlessSession(
        layout=layout, activity="uniform-capture", logflag=log_flags(LogType.UNIFORM_CAPTURE)
    )
    with session:
        session.prepare(keys_source=args.keys)
        result = session.run([str(binary), "--game", str(game)], timeout_seconds=args.seconds)

    print(f"run: exit={result.exit_code} reached_binary={result.reached_the_binary}")
    log = session.data_home / "Cemu" / "log.txt"
    if log.is_file():
        for line in log.read_text(errors="replace").splitlines():
            if "UniformCapture" in line:
                print(f"  {line}")

    produced = session.data_home / "Cemu" / CAPTURE_NAME
    if not produced.is_file():
        print(
            f"refused: the run left no {CAPTURE_NAME}. Capture was requested, so this is a "
            "failure of the instrument, not an empty result.",
            file=sys.stderr,
        )
        return 1
    destination = layout.activity_dir("uniform-capture") / CAPTURE_NAME
    shutil.copyfile(produced, destination)
    print(f"capture: {destination} ({destination.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
