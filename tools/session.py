#!/usr/bin/env python3
"""Keep one offscreen runtime up in the world, to drive and probe over HTTP.

Every other tool launches, runs a fixed script and tears down. This one boots
the product headless on the GPU with the staged save, presses into the world,
prints the control port and the PID to stop it by, and then only waits: the
operator (or an agent) walks it with `POST /input`, reads `/interpolation`,
`/draws` and `/objects`, and captures with `/capture` and `/restorecheck`,
while the same process keeps running.

It stops when the runtime exits or this tool is sent SIGTERM or SIGINT, and it
always ends the runtime by the PID it launched.
"""

from __future__ import annotations

import argparse
import signal
import sys
import time
from pathlib import Path
from types import FrameType

from wiiuport.gameplay import INTERVAL_SECONDS, PRESSES, press_into_world
from wiiuport.headless import HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys, resolve_save

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, runtime_env, wait_for_channel

ACTIVITY = "session"


class Stopped(Exception):
    """This tool was asked to stop."""


def _stop(_signum: int, _frame: FrameType | None) -> None:
    raise Stopped


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--save", type=Path, help="save folder; defaults to $WIIUPORT_SAVE")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=90, help="seconds to wait for the channel")
    parser.add_argument("--presses", type=int, default=PRESSES)
    parser.add_argument("--interval", type=float, default=INTERVAL_SECONDS)
    parser.add_argument(
        "--front-end",
        action="store_true",
        help="stay at the front end rather than pressing into the world",
    )
    args = parser.parse_args(argv)

    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
        save = resolve_save(args.save)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2
    layout = find_layout()
    if not layout.shell_binary.is_file():
        print(f"refused: no runtime at {layout.shell_binary}. Build it first.", file=sys.stderr)
        return 2

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    session = HeadlessSession(
        layout=layout, activity=ACTIVITY, runtime_env=runtime_env(args.port, continuous=True)
    )
    try:
        with session:
            session.prepare(keys_source=keys, save_source=save)
            with session.launch(layout.shell_command(game)) as running:
                print(
                    f"runtime pid {running.pid}, log {session.session_dir / 'run.log'}", flush=True
                )
                if not wait_for_channel(args.port, args.boot):
                    print("refused: the channel never answered while booting.", file=sys.stderr)
                    return 1
                if not args.front_end:
                    made = press_into_world(
                        args.port,
                        presses=args.presses,
                        interval=args.interval,
                        still_running=lambda: running.poll() is None,
                    )
                    if made < args.presses:
                        print(f"refused: the runtime exited after {made} presses.", file=sys.stderr)
                        return 1
                print(
                    f"ready: http://127.0.0.1:{args.port}/ (stop this tool by its pid)", flush=True
                )
                while running.poll() is None:
                    time.sleep(1)
                print(f"the runtime exited with {running.returncode}", file=sys.stderr)
                return 1
    except Stopped:
        return 0
    except ControlUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
