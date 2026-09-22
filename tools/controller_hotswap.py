#!/usr/bin/env python3
"""Plug a gamepad into the running product, then pull it out, and report both.

The automatic mapping's negative branch -- "no gamepad is plugged in" -- is
the one that runs on a machine with nothing attached, so it proves nothing
about the positive one. This creates a real input device through the kernel
while the title is running, so the host discovers it through the same path a
physical pad arrives on, and then removes it. Both transitions have to be
visible in the runtime's own counters or this refuses and says which one was
not seen.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.headless import HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys
from wiiuport.virtualpad import VirtualPad, VirtualPadUnavailable

from wiiuport.control import (
    DEFAULT_PORT,
    ControllerStatus,
    ControlUnavailable,
    read_controllers,
    read_counters,
    runtime_env,
)


def wait_for(port: int, seconds: int, predicate) -> ControllerStatus | None:
    """Poll /controllers until the predicate holds, or give up and say so."""
    deadline = time.monotonic() + seconds
    status = None
    while time.monotonic() < deadline:
        time.sleep(2)
        try:
            status = read_controllers(port)
        except ControlUnavailable:
            continue
        if predicate(status):
            return status
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=120, help="seconds to let the title boot")
    parser.add_argument("--settle", type=int, default=30, help="seconds to wait per transition")
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
        print(f"refused: no product binary at {binary}; build it first", file=sys.stderr)
        return 2

    pad = VirtualPad()
    session = HeadlessSession(
        layout=layout,
        activity="controller-hotswap",
        runtime_env={**runtime_env(args.port), "SDL_VIDEODRIVER": "x11"},
    )
    with session:
        session.prepare(keys_source=keys)
        with session.launch(layout.shell_command(game)) as running:
            deadline = time.monotonic() + args.boot
            while time.monotonic() < deadline:
                time.sleep(5)
                if running.poll() is not None:
                    print(f"refused: the product exited with {running.returncode}", file=sys.stderr)
                    return 1
                try:
                    if read_counters(args.port).framesObserved > 0:
                        break
                except ControlUnavailable:
                    continue
            else:
                print("refused: the product never reported a frame", file=sys.stderr)
                return 1

            before = read_controllers(args.port)
            print(f"before plugging anything in: {before.render()}")
            if not before.hostReports:
                print(
                    "refused: this build has no host reporting controllers, so the check "
                    "cannot tell an unattached pad from an unimplemented one",
                    file=sys.stderr,
                )
                return 1

            try:
                pad.plug()
            except VirtualPadUnavailable as unavailable:
                print(f"refused: {unavailable}", file=sys.stderr)
                return 2
            if not pad.is_visible():
                print(
                    "refused: the kernel created the gamepad but is not showing it to "
                    "other processes, so the host was never given a chance to see it",
                    file=sys.stderr,
                )
                return 1

            attached = wait_for(
                args.port,
                args.settle,
                lambda status: status.devicesAttached > before.devicesAttached,
            )
            if attached is None:
                print(
                    f"the gamepad was plugged in and visible, and the host did not attach it "
                    f"within {args.settle}s: {read_controllers(args.port).render()}",
                    file=sys.stderr,
                )
                return 1
            print(f"after plugging one in: {attached.render()}")
            if not attached.attachedDevice or attached.bindings == 0:
                print(
                    f"refused: the host counted an attach but named no device or no "
                    f"bindings: {attached.render()}",
                    file=sys.stderr,
                )
                return 1

            pad.unplug()
            lost = wait_for(
                args.port, args.settle, lambda status: status.devicesLost > attached.devicesLost
            )
            if lost is None:
                print(
                    f"the gamepad was unplugged and the host did not notice within "
                    f"{args.settle}s: {read_controllers(args.port).render()}",
                    file=sys.stderr,
                )
                return 1
            print(f"after pulling it out: {lost.render()}")

    print(
        f"the host attaches a gamepad that arrives while the title runs "
        f"({attached.attachedDevice}, {attached.bindings} bindings) and notices it leaving"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
