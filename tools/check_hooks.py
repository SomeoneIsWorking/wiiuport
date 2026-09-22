#!/usr/bin/env python3
"""Ask the running runtime what it has recorded, while it runs.

A recording runtime that installed its hooks and then saw nothing looks exactly
like one that works, until its counters are read. This starts the shipping
binary offscreen and polls its control channel, so the answer comes from the
running product rather than from a log read after the fact.

It never routes through run.sh: that launcher is the player's, and this run
must not take the desktop.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.headless import HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, Counters, read_counters

ENV_CONTROL_PORT = "WIIUPORT_CONTROL_PORT"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="control channel port")
    parser.add_argument("--seconds", type=int, default=90, help="how long to let it run")
    parser.add_argument("--poll", type=float, default=5.0, help="seconds between reads")
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
        activity="hook-check",
        runtime_env={ENV_CONTROL_PORT: str(args.port)},
    )
    reads: list[str] = []
    answered = 0
    last: Counters | None = None
    with session:
        session.prepare(keys_source=keys)
        with session.launch([str(binary), "--game", str(game)]) as running:
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline and running.poll() is None:
                time.sleep(args.poll)
                try:
                    counters = read_counters(args.port)
                except ControlUnavailable as unavailable:
                    reads.append(f"  no answer: {unavailable}")
                    continue
                answered += 1
                reads.append(f"  {counters.render()}")
                last = counters

    attempted = len(reads)
    print(f"polled {attempted} times, {answered} answered")
    for line in reads[:2] + reads[-3:] if attempted > 5 else reads:
        print(line)

    if answered == 0 or last is None:
        print(
            "refused: the control channel never answered, so nothing was measured. "
            "Either the runtime was built without WIIUPORT_SOURCE_DIR, or it never "
            f"reached startup within {args.seconds}s.",
            file=sys.stderr,
        )
        return 1
    if not last.recorded_anything:
        print(
            "refused: the channel answered but the runtime recorded no frames and no "
            "display lists, so the hooks are installed and never fire.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
