#!/usr/bin/env python3
"""Measure 60 Hz on the player's save: present percentiles under load, with the hardware.

Boots headless with the save, presses A through the front end until the title leaves it, then
runs Link in circles while the camera turns for the measured window.
"""

from __future__ import annotations

import argparse
import json
import platform
import sys
import time
from pathlib import Path

from wiiuport.drive import send_input
from wiiuport.gameplay import press_into_world
from wiiuport.headless import Display, HeadlessSession
from wiiuport.paths import find_layout
from wiiuport.perf import Percentiles, Window

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    read_gate,
    read_paint,
    request_bytes,
    runtime_env,
    wait_for_channel,
    wait_game_frames,
)
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys, resolve_save


def cpu_name() -> str:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown CPU"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--save", type=Path, help="save folder; defaults to $WIIUPORT_SAVE")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--seconds", type=float, default=60.0, help="measured window")
    args = parser.parse_args(argv)
    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
        save = resolve_save(args.save)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2
    if save is None:
        print("refused: a save is needed; a new game stops at name entry.", file=sys.stderr)
        return 2
    layout = find_layout()
    session = HeadlessSession(
        layout=layout,
        activity="perf",
        runtime_env=runtime_env(args.port),
        display_server=Display.GPU,
    )
    with session, session.launch(layout.shell_command(game)):
        session.prepare(keys_source=keys, save_source=save)
        if not wait_for_channel(args.port, 90):
            print("refused: the control channel never answered.", file=sys.stderr)
            return 1
        try:
            if press_into_world(args.port) is None:
                print("refused: the title never left its front end.", file=sys.stderr)
                return 1
            wait_game_frames(300, port=args.port)
            send_input(port=args.port, leftx=0.7, lefty=0.7, rightx=1.0)
            wait_game_frames(60, port=args.port)
            request_bytes("POST", "/pacing", args.port, 5)
            paints = read_paint(args.port).paints
            ticks = read_gate(args.port).ticks or 0
            started = time.monotonic()
            time.sleep(args.seconds)
            pacing = request_bytes("GET", "/pacing", args.port, 5)
            elapsed = time.monotonic() - started
            window_paints = read_paint(args.port).paints - paints
            window_ticks = (read_gate(args.port).ticks or 0) - ticks
            send_input(port=args.port, release=1)
        except ControlUnavailable as unavailable:
            print(f"refused: {unavailable}", file=sys.stderr)
            return 1
        device = session.rendered_on()
    report = json.loads(pacing)
    scan_out = Percentiles.parse(report["scanOut"])
    window = Window(
        seconds=elapsed,
        paints=window_paints,
        ticks=window_ticks,
        present=Percentiles.parse(report["present"]),
        scan_out=scan_out if scan_out.intervals else None,
    )
    print(f"hardware: {cpu_name()}; {device}")
    print(window.render())
    failures = window.failures()
    for failure in failures:
        print(f"not sustained: {failure}", file=sys.stderr)
    print("60 Hz sustained" if not failures else "60 Hz not sustained")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
