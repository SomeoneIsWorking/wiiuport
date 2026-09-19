#!/usr/bin/env python3
"""Drive the title past its front end and report whether gameplay was reached.

Reaching gameplay is the blocker in front of finding the camera, null-diffing
a replay and interpolating anything: every earlier measurement was taken from
an unattended screen. This presses through the menus and then reads the
runtime's own transform search, which is the discriminator -- a drawn scene
with a moving view is gameplay in a way that a frame count is not.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.drive import press, release
from wiiuport.headless import HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, read_counters, read_transforms

ENV_CONTROL_PORT = "WIIUPORT_CONTROL_PORT"


def wait_for_channel(port: int, seconds: int) -> bool:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        time.sleep(5)
        try:
            read_counters(port)
        except ControlUnavailable:
            continue
        return True
    return False


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=90, help="seconds to let the title boot")
    parser.add_argument("--presses", type=int, default=12, help="how many times to press through")
    parser.add_argument("--interval", type=int, default=8, help="seconds between presses")
    parser.add_argument("--settle", type=int, default=60, help="seconds to watch afterwards")
    args = parser.parse_args(argv)

    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2

    layout = find_layout()
    binary = layout.cemu_binary
    if not binary.is_file():
        print(f"refused: no runtime at {binary}. Build it first.", file=sys.stderr)
        return 2

    session = HeadlessSession(
        layout=layout,
        activity="reach-gameplay",
        runtime_env={ENV_CONTROL_PORT: str(args.port)},
    )
    report = None
    counters = None
    with session:
        session.prepare(keys_source=keys)
        with session.launch([str(binary), "--game", str(game)]) as running:
            if not wait_for_channel(args.port, args.boot):
                print(
                    "refused: the channel never answered while booting, so nothing was "
                    "driven and nothing was measured.",
                    file=sys.stderr,
                )
                return 1
            # Alternating A and Plus covers a logo that takes either, a file
            # select that takes A, and a start prompt that takes Plus.
            for index in range(args.presses):
                if running.poll() is not None:
                    break
                button = "a" if index % 2 == 0 else "plus"
                try:
                    press(button, port=args.port)
                except ControlUnavailable as unavailable:
                    print(f"refused: {unavailable}", file=sys.stderr)
                    return 1
                time.sleep(args.interval)
            deadline = time.monotonic() + args.settle
            while time.monotonic() < deadline and running.poll() is None:
                time.sleep(10)
                try:
                    counters = read_counters(args.port)
                    report = read_transforms(args.port)
                except ControlUnavailable:
                    continue
            try:
                release(port=args.port)
            except ControlUnavailable:
                pass
            exit_code = running.poll()

    if report is None or counters is None:
        print(
            "refused: the channel stopped answering after the presses, so whether they "
            "reached anything is unknown.",
            file=sys.stderr,
        )
        return 1
    print(counters.render())
    print(
        f"input: {counters.inputPressesQueued} presses queued, "
        f"{counters.inputPollsAnswered} of {counters.inputPollsSeen} gamepad reads driven"
    )
    print(report.render())
    print(f"process exit {exit_code}")

    if counters.inputPollsSeen == 0:
        print(
            "refused: the title never read the gamepad, so the injection point was "
            "never reached and no press could have arrived.",
            file=sys.stderr,
        )
        return 1
    if counters.inputPollsAnswered == 0:
        print(
            "refused: gamepad reads happened but none was driven, so the presses never "
            "reached the emulated controller.",
            file=sys.stderr,
        )
        return 1
    # From the report's own total, not the listed subset: the list is capped
    # for readability and a gate must not depend on where it was cut.
    if report.sharedAndMoving == 0:
        print(
            "no shared, moving view transform was found, so this run is not shown to have "
            "reached gameplay. The input arrived; where it led is the open question.",
            file=sys.stderr,
        )
        return 1
    print(f"reached gameplay: {report.sharedAndMoving} shared, moving view candidates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
