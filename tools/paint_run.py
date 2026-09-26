#!/usr/bin/env python3
"""Measure Wind Waker HD's own display thread painting twice, at 60 Hz.

The mechanism this measures is a mod, not a re-implementation: a stand-in for
the display frame, built from words the title executes itself, that paints each
tick's world twice and asks for one vblank per flip. What has to be true of the
title before any blend rides on it:

  - the display thread paints twice as often when the stand-in is in;
  - the logic thread keeps its own 30 Hz, because the picture's rate is not the
    simulation's rate;
  - and the second paint is the same world as the first, which is the null case
    a blend has to beat.

The windows alternate: off, on, off, in the same scene and the same run, so the
off windows are the control. A run whose on window did not paint more than its
neighbours fails, because a mod that quietly did nothing must not read as a mod
that worked -- nor does a run where the logic rate left 30 Hz, which would mean
the simulation had been doubled along with the picture.
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from wiiuport.drive import press, release
from wiiuport.headless import Display, HeadlessSession, LogType, log_flags
from wiiuport.paths import find_layout

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    read_blocks,
    read_callers,
    read_paint,
    runtime_env,
    set_paint,
    wait_for_channel,
)
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys

# The title's own tick, watched so the simulation's rate is measured at the
# function that runs it rather than at a frame counter the flip also moves:
# fapGm_Execute, and the instruction it starts on.
LOGIC_TARGET = "025d42ec:7c0802a6"

# What the logic rate must stay inside, in hertz. The title runs at 30 and this
# is the falsifier's whole claim: a doubled picture must not double this.
LOGIC_RANGE = (28.5, 31.5)

# What each stand-in should do to the display's rate, and why. The frame waits
# for its own flip at the end, so a paint costs a flip however many of them
# there are per loop: painting twice with the title's two vblanks a flip
# repaints the same thirty frames a second and only doubles the work, and only
# the one-vblank stand-in can present at sixty. The first two modes are asked
# whether the title survives them at all.
EXPECTED_MULTIPLE = {1: 1.0, 2: 1.0, 3: 2.0, 4: 1.0, 5: 1.0, 6: 2.0}
RATE_TOLERANCE = 0.9


@dataclass(frozen=True)
class Window:
    """One measurement window: what the mod was set to, and what it did."""

    name: str
    installed: bool
    seconds: float
    paints_per_second: float
    logic_per_second: float
    paints: int
    logic_calls: int

    def render(self) -> str:
        return (
            f"{self.name:<6} mod {'on ' if self.installed else 'off'}: "
            f"{self.paints_per_second:6.2f} paints/s, {self.logic_per_second:6.2f} logic/s "
            f"over {self.seconds:4.1f}s ({self.paints} paints, {self.logic_calls} ticks)"
        )


def _last_log_lines(session: HeadlessSession, count: int) -> list[str]:
    """The product's own last words, for a refusal that has to say more."""
    log = session.session_dir / "data" / "Cemu" / "log.txt"
    if not log.is_file():
        return ["(the product wrote no log)"]
    lines = [line for line in log.read_text(errors="replace").splitlines() if line.strip()]
    return lines[-count:]


def _logic_calls(port: int) -> int:
    for entry in read_callers(port):
        if entry.entry == 0x025D42EC:
            return entry.calls
    return 0


def _window(name: str, port: int, seconds: float, before: tuple[int, int]) -> Window:
    time.sleep(seconds)
    after = (read_paint(port).paints, _logic_calls(port))
    elapsed = seconds
    return Window(
        name=name,
        installed=name == "on",
        seconds=elapsed,
        paints_per_second=(after[0] - before[0]) / elapsed,
        logic_per_second=(after[1] - before[1]) / elapsed,
        paints=after[0] - before[0],
        logic_calls=after[1] - before[1],
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument(
        "--save",
        type=Path,
        help="save folder; without one the run stops at the name-entry keyboard",
    )
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=90, help="seconds to let the title boot")
    parser.add_argument("--presses", type=int, default=12, help="presses through the front end")
    parser.add_argument("--press-interval", type=int, default=8)
    parser.add_argument("--settle", type=int, default=30, help="seconds to watch before measuring")
    parser.add_argument("--window", type=float, default=6.0, help="seconds per window")
    parser.add_argument(
        "--mode",
        type=int,
        choices=[1, 2, 3, 4, 5, 6],
        default=6,
        help="what the stand-in does: 1 paints once (the redirect alone), 2 paints "
        "twice at the title's two vblanks a flip, 3 paints twice at one, 4 is the "
        "indirect variant that does not run, 5 paints once and writes the title's "
        "own interval field to one, 6 paints once and asks the game's own setter "
        "for one vblank a flip",
    )
    parser.add_argument(
        "--display",
        choices=[display.value for display in Display],
        default=Display.GPU.value,
    )
    parser.add_argument(
        "--log-recompiler",
        action="store_true",
        help="turn on the recompiler's own log: what it translated, and what it "
        "refused to. Very large, so a diagnostic asks for it rather than a run",
    )
    parser.add_argument(
        "--windows",
        type=int,
        default=3,
        help="how many alternating windows; fewer makes a diagnostic quicker",
    )
    args = parser.parse_args(argv)

    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2

    layout = find_layout()
    if not layout.shell_binary.is_file():
        print(f"refused: no runtime at {layout.shell_binary}. Build it first.", file=sys.stderr)
        return 2

    env = runtime_env(args.port, continuous=False)
    env["WIIUPORT_CALLER_CENSUS"] = LOGIC_TARGET
    logflag = log_flags(LogType.RECOMPILER) if args.log_recompiler else 0
    session = HeadlessSession(
        layout=layout,
        activity="paint-run",
        runtime_env=env,
        display_server=Display(args.display),
        logflag=logflag,
    )
    windows: list[Window] = []
    state = None
    with session:
        session.prepare(keys_source=keys, save_source=args.save)
        with session.launch(layout.shell_command(game)) as running:
            if not wait_for_channel(args.port, args.boot):
                print(
                    "refused: the channel never answered while booting, so nothing was driven "
                    "and nothing was measured.",
                    file=sys.stderr,
                )
                return 1
            for index in range(args.presses):
                if running.poll() is not None:
                    break
                try:
                    press("a" if index % 2 == 0 else "plus", port=args.port)
                except ControlUnavailable as unavailable:
                    print(f"refused: {unavailable}", file=sys.stderr)
                    return 1
                time.sleep(args.press_interval)
            # Wait for the display thread to have painted at all: a title still
            # on a logo has a paint counter, but the world's is what matters and
            # only a running one has a stable rate to compare.
            deadline = time.monotonic() + args.settle
            while time.monotonic() < deadline and running.poll() is None:
                time.sleep(5)
                try:
                    if read_paint(args.port).paints > 30:
                        break
                except ControlUnavailable:
                    continue
            try:
                for name in ("off", "on", "off")[: max(args.windows, 1)]:
                    if running.poll() is not None:
                        # The product died between windows. Said plainly, with
                        # what its own log last said, because "the channel did
                        # not answer" reads the same whether the mod killed the
                        # title or the display did.
                        print(
                            f"refused: the product exited {running.returncode} before the "
                            f"{name} window; its log last said:",
                            file=sys.stderr,
                        )
                        for line in _last_log_lines(session, 4):
                            print(f"  {line}", file=sys.stderr)
                        return 1
                    set_paint(name == "on", port=args.port, mode=args.mode)
                    # One settle second, then the window's own start reading, so
                    # the first second's paints are not counted twice.
                    time.sleep(1.0)
                    start = (read_paint(args.port).paints, _logic_calls(args.port))
                    windows.append(_window(name, args.port, args.window, start))
                state = read_paint(args.port)
            except ControlUnavailable as unavailable:
                print(f"refused: {unavailable}", file=sys.stderr)
                return 1
            try:
                set_paint(False, port=args.port)
            except ControlUnavailable:
                pass
            try:
                release(port=args.port)
            except ControlUnavailable:
                pass

    if not windows or state is None:
        print("refused: the run measured no window at all.", file=sys.stderr)
        return 1
    device = session.rendered_on()
    print(f"rendered on: {device}")
    print(f"stand-in mode {args.mode} requested")
    # The title's own uniform block binder, counted over the whole run: whether
    # the per-object descriptor's cursor alternates decides if the previous
    # tick's block is still there to read when this tick paints, which is what a
    # blend is built on. Measured on the same run, not a separate one.
    try:
        census = read_blocks(args.port)
        print(census.render())
        print(f"  its two entries: {census.parity()}")
        if census.examples:
            first = census.examples[0]
            print(f"  a binding read cursor {first[0]}, block offset {first[1]}, size {first[2]}")
    except ControlUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
    print(state.render())
    for window in windows:
        print(window.render())

    off = [w.paints_per_second for w in windows if not w.installed]
    on = [w.paints_per_second for w in windows if w.installed]
    logic = [w.logic_per_second for w in windows]
    failures: list[str] = []
    wanted = EXPECTED_MULTIPLE[args.mode]
    if not on:
        failures.append("no window ran with the mod on, so nothing was compared")
    elif min(off) < 1.0:
        failures.append(f"the mod-off windows painted {min(off):.2f}/s, too few to be a control")
    elif min(on) < wanted * RATE_TOLERANCE * max(off):
        failures.append(
            f"with the mod on the display painted {min(on):.2f}/s against "
            f"{max(off):.2f}/s off, short of the {wanted:.1f}x this stand-in is for: "
            "it did not take"
        )
    elif max(on) > (wanted + 0.5) * max(off):
        failures.append(
            f"with the mod on the display painted {max(on):.2f}/s against "
            f"{max(off):.2f}/s off, more than the {wanted:.1f}x this stand-in is for: "
            "something else is painting"
        )
    for window in windows:
        if not LOGIC_RANGE[0] <= window.logic_per_second <= LOGIC_RANGE[1]:
            failures.append(
                f"the {window.name} window ran the logic at "
                f"{window.logic_per_second:.2f}/s, outside "
                f"{LOGIC_RANGE[0]}-{LOGIC_RANGE[1]}: the simulation moved with the picture"
            )
    if state.probe != "installed":
        failures.append(f"the frame probe is {state.probe}, so paints were not all counted")
    if failures:
        print("\nFAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(
        f"\npassed: the display painted {max(off):.2f}/s with the mod off and "
        f"{min(on):.2f}-{max(on):.2f}/s with it on, against the {wanted:.1f}x mode "
        f"{args.mode} is for, and the logic stayed at "
        f"{min(logic):.2f}-{max(logic):.2f}/s throughout"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
