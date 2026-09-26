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
from wiiuport.paths import Layout, find_layout

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    capture_frame,
    compare_bytes,
    compare_images,
    dump_guest,
    read_blocks,
    read_callers,
    read_gate,
    read_paint,
    runtime_env,
    set_gate,
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
#
# The bound is a shade wider than the 29.9 to 30.0 the mechanism is aimed at,
# because this counts calls over a six-second window and the title's own pacing
# jitters: measured windows come in between 30.00 and 30.17 a second, on both
# sides of the change. So the check is "not doubled and not moved", which is what
# a doubling of the simulation would break first, and every window's count and
# rate is printed so the spread is visible rather than asserted.
LOGIC_RANGE = (29.5, 30.5)

# The logic's rate is counted at the tick, and with the gate in the tick is
# replaced -- so the rate comes from the gate's own counter of the ticks it let
# through. Both counts are read and the run says which it used, because a rate
# taken from a counter the gate owns and a rate taken from a probe are not the
# same measurement and the report should not read as though they were.

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

    source: str = "the caller census on fapGm_Execute"

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


def _logic_source(port: int) -> tuple[int, str]:
    """The logic's tick count and where it came from.

    The gate's own counter when the gate is in, because the gate has replaced
    the tick the probe watches; the probe's call count otherwise.
    """
    try:
        gate = read_gate(port)
    except ControlUnavailable:
        return _logic_calls(port), "the caller census on fapGm_Execute"
    if gate.enabled and gate.ticks is not None:
        return gate.ticks, "the logic gate's own tick counter"
    return _logic_calls(port), "the caller census on fapGm_Execute"


def save_block_dumps(layout: Layout, block: int, before: bytes, after: bytes) -> tuple[Path, Path]:
    """The two dumps, on disk, in one activity directory of their own.

    The bytes are the only copy of the title's per-tick state that a run will
    ever have: the game's next tick overwrites them and nothing records what they
    were. So they are written out, with the block's address in the name, and the
    report says where they are -- a measurement nobody can look at again is a
    measurement one has to take again.
    """
    directory = layout.activity_dir("block-dump")
    first = directory / f"block-{block:08x}-before.bin"
    second = directory / f"block-{block:08x}-after.bin"
    first.write_bytes(before)
    second.write_bytes(after)
    return first, second


def _window(name: str, port: int, seconds: float, before: tuple[int, int]) -> Window:
    time.sleep(seconds)
    # One reading of each counter, so the two rates come from the same moment.
    paints = read_paint(port).paints
    ticks, source = _logic_source(port)
    elapsed = seconds
    return Window(
        name=name,
        installed=name == "on",
        seconds=elapsed,
        source=source,
        paints_per_second=(paints - before[0]) / elapsed,
        logic_per_second=(ticks - before[1]) / elapsed,
        paints=paints - before[0],
        logic_calls=ticks - before[1],
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
        "--gate",
        action="store_true",
        help="gate the title's logic alongside the stand-in, so the picture runs at "
        "the display's rate and the logic keeps its own",
    )
    parser.add_argument(
        "--captures",
        type=int,
        default=0,
        help="how many consecutive frames to capture in the on window and compare "
        "byte by byte; 0 skips the comparison",
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
    comparison = ""
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
                    # The gate with it: the paint rate and the logic rate are the
                    # same measurement until something separates them, and the
                    # gate is what separates them.
                    if args.gate:
                        gate = set_gate(name == "on", port=args.port)
                        print(
                            f"  gate {'in' if gate.enabled else 'out'}: {gate.render()}", flush=True
                        )
                    # One settle second, then the window's own start reading, so
                    # the first second's paints are not counted twice.
                    time.sleep(1.0)
                    start = (read_paint(args.port).paints, _logic_calls(args.port))
                    window = _window(name, args.port, args.window, start)
                    windows.append(window)
                    # Printed as it is measured. A run whose product dies before
                    # the end -- which this one has done twice, once on a lost
                    # display and once for reasons the log does not name -- would
                    # otherwise throw away the numbers it had already taken.
                    print(window.render(), flush=True)
                    if name == "on" and args.captures >= 2:
                        # Two paints in a row, compared byte by byte. With the
                        # logic at 30 and the picture at 60 the two are the same
                        # world twice, which is the null case a blend has to beat
                        # -- and the one thing a rate alone cannot show.
                        first = capture_frame(args.port, 0)
                        second = capture_frame(args.port, 1)
                        comparison = compare_images(first, second)
                try:
                    state = read_paint(args.port)
                except ControlUnavailable as unavailable:
                    print(
                        f"the product stopped answering before the end: {unavailable}",
                        file=sys.stderr,
                    )
            except ControlUnavailable as unavailable:
                print(f"refused: {unavailable}", file=sys.stderr)
                return 1
            try:
                set_paint(False, port=args.port)
                if args.gate:
                    set_gate(False, port=args.port)
            except ControlUnavailable:
                pass
            try:
                release(port=args.port)
            except ControlUnavailable:
                pass

    if not windows:
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
        for binding in census.examples[:2]:
            print(
                f"  a binding read cursor {binding.cursor}, offset {binding.offset}, "
                f"size {binding.size}, entry "
                + ", ".join(
                    f"{word}:{value:#010x}" for word, value in sorted(binding.entry.items())
                )
            )
            block = binding.block()
            other = binding.other_block()
            if block is None:
                print("    no single word of the entry names a readable block, so no address")
                continue
            # Both slots, twice, half a tick apart. The bound one says what the
            # tick is drawing from now; the other one says whether the previous
            # tick's values are still in memory when it does, which is the whole
            # question a blend has to answer before it can be built on this ring.
            # Their difference is the per-tick pose, and it is a range, not a
            # guess: the bytes that move are the bytes a blend would write.
            before_bytes = dump_guest(args.port, block, binding.size)
            before_other = dump_guest(args.port, other, binding.other_size) if other else None
            time.sleep(0.3)
            after_bytes = dump_guest(args.port, block, binding.size)
            after_other = dump_guest(args.port, other, binding.other_size) if other else None
            print(
                f"    slot {binding.cursor} at {block:#010x}: {compare_bytes(before_bytes, after_bytes)}"
            )
            if other is not None:
                print(
                    f"    slot {binding.other_cursor} at {other:#010x}: "
                    f"{compare_bytes(before_other, after_other)}"
                )
                print(
                    "    and the two slots against each other, right now: "
                    f"{compare_bytes(before_bytes, before_other)}"
                )
                saved = save_block_dumps(layout, block, before_bytes, after_bytes)
                other_saved = save_block_dumps(layout, other, before_other, after_other)
                print(
                    f"    saved {saved[0].name} and {other_saved[0].name} for tools/block_pose.py"
                )
    except ControlUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
    if state is not None:
        print(state.render())
    if comparison:
        print(f"  two consecutive paints: {comparison}")

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
    if state is not None and state.probe != "installed":
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
