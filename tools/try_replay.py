#!/usr/bin/env python3
"""Fire one replay into a running title and report what the runtime did.

The first question a replay has to answer is whether a recorded frame can be
re-fed to the command processor at all without disturbing the run. So this
arms exactly one replay, over the control channel, and then keeps reading the
counters: a runtime that died on the submission stops answering, and a
submission that quietly drew nothing shows up as a replay that ran with zero
lists submitted.
"""

from __future__ import annotations

import argparse
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from wiiuport.headless import HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, read_counters

ENV_CONTROL_PORT = "WIIUPORT_CONTROL_PORT"


def arm_replay(port: int, timeout: float = 5.0) -> str:
    request = urllib.request.Request(f"http://127.0.0.1:{port}/replay", method="POST", data=b"")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read().decode("utf-8").strip()
    except urllib.error.URLError as unreachable:
        raise ControlUnavailable(f"could not arm a replay: {unreachable.reason}") from unreachable


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--settle", type=int, default=45, help="seconds before arming")
    parser.add_argument("--after", type=int, default=30, help="seconds to watch afterwards")
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
        activity="replay-probe",
        runtime_env={ENV_CONTROL_PORT: str(args.port)},
    )
    with session:
        session.prepare(keys_source=keys)
        with session.launch(layout.shell_command(game)) as running:
            deadline = time.monotonic() + args.settle
            before = None
            while time.monotonic() < deadline:
                time.sleep(5)
                try:
                    before = read_counters(args.port)
                except ControlUnavailable:
                    continue
            if before is None:
                print(
                    "refused: the channel never answered before arming, so nothing was measured.",
                    file=sys.stderr,
                )
                return 1
            print(f"before: {before.render()}")

            try:
                print(f"armed:  {arm_replay(args.port)}")
            except ControlUnavailable as unavailable:
                print(f"refused: {unavailable}", file=sys.stderr)
                return 1

            survived = 0
            after = before
            watch = time.monotonic() + args.after
            while time.monotonic() < watch and running.poll() is None:
                time.sleep(5)
                try:
                    after = read_counters(args.port)
                    survived += 1
                except ControlUnavailable as unavailable:
                    print(f"  channel stopped answering: {unavailable}")
                    break
            exit_code = running.poll()

    print(f"after:  {after.render()}")
    print(
        f"replays run {after.replaysRun}, lists submitted {after.replayListsSubmitted}, "
        f"refused {after.replayListsRefused}"
    )
    print(f"answered {survived} times after arming; process exit {exit_code}")

    if after.replaysRun == 0:
        print(
            "refused: the replay never ran, so the arming never reached a frame end.",
            file=sys.stderr,
        )
        return 1
    if after.replayListsSubmitted == 0:
        print(
            "refused: the replay ran and submitted nothing, so the recorded frame "
            "held no list the command processor would take.",
            file=sys.stderr,
        )
        return 1
    if survived == 0:
        print(
            "refused: the runtime stopped answering immediately after the replay, "
            "so the submission did not survive.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
