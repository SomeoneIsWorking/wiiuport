#!/usr/bin/env python3
"""Require that drawing an in-between frame leaves guest memory as it was.

A replay re-issues packets whose effects the guest has already consumed, and
the fork withholds every class of them it knows. This checks the claim rather
than the list: it drives the title into the world, holds it at a frame's end,
and has the runtime copy guest memory, draw an in-between frame through the
shipping path, and compare -- alternating with control windows that draw
nothing, because the guest's audio and timer threads write memory throughout.
Any page that changed only while an in-between frame was drawn fails it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from wiiuport.gameplay import INTERVAL_SECONDS, PRESSES, press_into_world
from wiiuport.headless import HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys, resolve_save

from wiiuport import shadow
from wiiuport.control import DEFAULT_PORT, ControlUnavailable, runtime_env, wait_for_channel


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--save", type=Path, help="save folder; defaults to $WIIUPORT_SAVE")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=150)
    parser.add_argument("--presses", type=int, default=PRESSES)
    parser.add_argument("--interval", type=float, default=INTERVAL_SECONDS)
    parser.add_argument("--rounds", type=int, default=8)
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

    session = HeadlessSession(
        layout=layout, activity="shadow-check", runtime_env=runtime_env(args.port)
    )
    with session:
        session.prepare(keys_source=keys, save_source=save)
        with session.launch(layout.shell_command(game)) as running:
            if not wait_for_channel(args.port, args.boot):
                print("refused: the channel never answered while booting.", file=sys.stderr)
                return 1
            try:
                pressed = press_into_world(
                    args.port,
                    presses=args.presses,
                    interval=args.interval,
                    still_running=lambda: running.poll() is None,
                )
                if pressed < args.presses:
                    print("refused: the runtime exited before reaching the world.", file=sys.stderr)
                    return 1
                shadow.hold(args.port)
                report = shadow.check(args.port, args.rounds)
                shadow.resume(args.port)
            except ControlUnavailable as unavailable:
                print(f"refused: {unavailable}", file=sys.stderr)
                return 1

    print(report.render())
    return 0 if report.clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
