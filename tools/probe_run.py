"""Does the request pattern kill it, or the product?

The measurement run's product dies about thirteen seconds in, while it is
translating code hard, and a hand-launched product with the same environment
survives a hundred seconds. The difference between the two is not the environment
-- it is what the run *asks*: a boot poll, a button press, and a settle poll.

So this launches the product exactly as a run does and then makes only those
requests, one at a time, saying which one it is making and whether the product was
still there afterwards. That names the request rather than the phase, which is the
difference between a finding and a rerun.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

from wiiuport.drive import press
from wiiuport.headless import Display, HeadlessSession
from wiiuport.paths import find_layout

from wiiuport import control
from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    read_blocks,
    read_callers,
    read_gate,
    read_paint,
    set_gate,
    set_paint,
    wait_for_channel,
)
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys


def _logic_count(port: int) -> tuple[int, str]:
    """The tick count and where it came from.

    The gate's own counter when the gate is in, because the gate holds the tick's
    entry and a probe on that entry sees nothing; the caller's census otherwise.
    Which one is used is reported with the number, because a rate from a counter
    the gate owns and a rate from a probe are not the same measurement.
    """
    try:
        gate = read_gate(port)
    except ControlUnavailable:
        gate = None
    if gate is not None and gate.enabled and gate.ticks is not None:
        return gate.ticks, "the logic gate's own tick counter"
    try:
        for entry in read_callers(port):
            if entry.entry == 0x025D42EC:
                return entry.calls, f"the caller census on {entry.entry:#010x}"
    except ControlUnavailable:
        pass
    return 0, "nothing counted the tick"


def measure(
    running: subprocess.Popen[bytes], args: argparse.Namespace, say: Callable[[str], None]
) -> None:
    """Alternate the mod off and on and report both rates with their counts.

    The off windows are the control, in the same scene and the same run, so a rate
    that did not move is a rate that did not move rather than a title that was
    busy. Every number is a count over a stated number of seconds, and the logic
    rate says which counter it came from.
    """
    for name in ("off", "on", "off", "on")[: max(1, args.windows)]:
        if running.poll() is not None:
            say("  the product went before a window could be measured")
            return
        on = name == "on"
        try:
            set_paint(on, port=args.port, mode=args.mode)
            set_gate(on, port=args.port)
        except ControlUnavailable as unavailable:
            print(f"  refused to arm {name}: {str(unavailable)[:80]}", flush=True)
            return
        print(f"  window {name}: mod {'on ' if on else 'off'}", flush=True)
        # What the gate says about itself, every window: its probe's installation
        # and the words at the tick's entry and the one after it. A rate of zero
        # with the gate in is either "no calls came" or "the gate is not wired to
        # the tick", and only these three say which.
        try:
            gate = read_gate(args.port)
            print(
                f"    gate {'in ' if gate.enabled else 'out'}: "
                f"{gate.probe}, block {gate.block}, at the tick's entry {gate.word_at_entry}, "
                f"at the word after it {gate.word_at_body}, "
                f"calls {gate.calls}, ticks {gate.ticks}, "
                f"entries {gate.gate_entries} through a probe {gate.gate_probe}",
                flush=True,
            )
        except ControlUnavailable as unavailable:
            print(f"    the gate did not answer: {str(unavailable)[:60]}", flush=True)
        time.sleep(1.0)
        paints_before = read_paint(args.port).paints
        ticks_before, _ = _logic_count(args.port)
        time.sleep(args.window)
        paints_after = read_paint(args.port).paints
        ticks_after, source = _logic_count(args.port)
        print(
            f"    {(paints_after - paints_before) / args.window:6.2f} paints/s, "
            f"{(ticks_after - ticks_before) / args.window:6.2f} logic/s over "
            f"{args.window:.1f}s ({paints_after - paints_before} paints, "
            f"{ticks_after - ticks_before} ticks; logic from {source})",
            flush=True,
        )
    for turn_off in (set_paint, set_gate):
        try:
            turn_off(False, port=args.port)
        except ControlUnavailable:
            pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--keys", type=Path, default=None)
    parser.add_argument("--save", type=Path, default=None)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument(
        "--trace",
        type=Path,
        default=None,
        help="write every channel request and its answer here; two runs that differ "
        "only in the product's fate differ in this file first",
    )
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=60)
    parser.add_argument("--presses", type=int, default=3)
    parser.add_argument(
        "--measure",
        action="store_true",
        help="measure the presentation rate instead of only surviving: arm the paint "
        "stand-in and the logic gate, read both rates over alternating off and on "
        "windows, and report the counts each rate came from",
    )
    parser.add_argument(
        "--mode",
        type=int,
        default=6,
        help="which stand-in to install: 1 paints once, 2 twice, 3 twice at two vblanks a "
        "flip, 6 twice at one",
    )
    parser.add_argument("--window", type=float, default=6.0, help="seconds per window")
    parser.add_argument("--windows", type=int, default=3, help="how many, alternating off and on")
    parser.add_argument(
        "--activity",
        default="probe-run",
        help="which session directory to use. A measurement run keeps one, and a "
        "difference in what a run inherits from its last run is invisible until this "
        "points at the same one",
    )
    parser.add_argument(
        "--census",
        default=None,
        help="the caller census address:environment=entry:firstInstruction a run sets",
    )
    args = parser.parse_args(argv)

    if args.trace is not None:
        control.TRACE_PATH = args.trace
        args.trace.write_text("")

    layout = find_layout()
    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 1

    runtime_env = {
        "WIIUPORT_CONTROL_PORT": str(args.port),
        "WIIUPORT_INTERPOLATION": "0",
    }
    if args.census:
        runtime_env["WIIUPORT_CALLER_CENSUS"] = args.census
    with (
        HeadlessSession(
            layout=layout,
            activity=args.activity,
            display_server=Display.GPU,
            runtime_env=runtime_env,
        ) as session,
        session.launch(layout.shell_command(game)) as running,
    ):
        session.prepare(keys_source=keys, save_source=args.save)
        started = time.monotonic()

        def alive() -> bool:
            return running.poll() is None

        def say(what: str) -> None:
            lived = time.monotonic() - started
            state = "alive" if alive() else f"GONE (exit {running.returncode})"
            print(f"  {lived:6.1f}s  {what:<34} {state}", flush=True)

        say("launched")
        if not wait_for_channel(args.port, args.boot):
            print("refused: the channel never answered", file=sys.stderr)
            return 1
        say("channel answered")
        for index in range(args.presses):
            try:
                press("a" if index % 2 == 0 else "plus", port=args.port)
            except ControlUnavailable as unavailable:
                print(f"  press {index} refused: {str(unavailable)[:70]}", flush=True)
                break
            say(f"press {index}")
            time.sleep(4)
        deadline = time.monotonic() + args.seconds
        paints = -1
        while time.monotonic() < deadline and alive():
            time.sleep(3)
            try:
                paints = read_paint(args.port).paints
            except ControlUnavailable:
                pass
        say(f"after settling, last paints {paints}")
        # The caller census, which is the instrument the logic rate rests on, and
        # so has to be shown rather than trusted: a census that counts nothing is
        # indistinguishable from a call that does not happen, and only the
        # addresses it was given can tell the two apart.
        try:
            for entry in read_callers(args.port):
                print(
                    f"  census {entry.entry:#010x} {entry.installation}: {entry.calls} calls",
                    flush=True,
                )
        except ControlUnavailable as unavailable:
            print(f"  the caller census did not answer: {str(unavailable)[:70]}", flush=True)
        if args.measure and alive():
            measure(running, args, say)
        if alive():
            # The census, because a measurement run reads it here and this tool
            # exists to say which request a run's product dies on. A route that
            # only fails under a measurement run is still a fault in the route.
            try:
                census = read_blocks(args.port, timeout=10.0)
                say(
                    f"census: {census.bindings} bindings, cursors {census.cursors}, "
                    f"switches {census.cursor_switches}/{census.cursor_compared}"
                )
            except ControlUnavailable as unavailable:
                print(f"  census refused: {str(unavailable)[:90]}", flush=True)
                say("after the census")
        if alive():
            print(f"  survived {args.seconds:.0f}s of polling", flush=True)
        return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
