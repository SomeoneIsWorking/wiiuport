#!/usr/bin/env python3
"""Play the title with continuous interpolation on, and count what it did.

Boots the product headless with the staged save, presses through to gameplay,
then walks the player around while reading the interpolator's counters. The
run fails when no tick was interpolated, whatever else happened: a run where
the in-between frame never fired must not read as a run where it did.

It also takes a snapshot of consecutive recorded frames, for the offline
question the counters cannot answer -- which submitted values belong to the
same object from one tick to the next.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.census import read_census, request_census
from wiiuport.draws import read_draws, read_vertex_census, request_vertex_census
from wiiuport.drive import left_stick, release
from wiiuport.gameplay import INTERVAL_SECONDS, PRESSES, press_into_world
from wiiuport.headless import HeadlessSession
from wiiuport.image import arm_capture, read_capture
from wiiuport.interpolation import (
    Interpolation,
    parse_recordings,
    read_interpolation,
    restart_pacing,
    set_continuous,
    take_recordings,
)
from wiiuport.paired import PairedRates
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys, resolve_save

from wiiuport import neighbour_check, restore_check
from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    runtime_env,
    wait_for,
    wait_for_channel,
)

# Where the stick points while walking, in turn: a camera that only ever moves
# one way is a narrower test of the blend than one that turns and reverses.
WALK_DIRECTIONS = ((0.0, 1.0), (1.0, 0.0), (0.0, -1.0), (-1.0, 0.0))

# Windows of the paired comparison and how long each lasts: short enough that
# the two settings see the same places, long enough to hold tens of ticks.
PAIRED_WINDOWS = 8
PAIRED_SECONDS = 3

# Frames the object census adds up: one frame's objects vary too much from
# the next to rank shaders by.
CENSUS_FRAMES = 16


def render_trace(trace: list[tuple[str, Interpolation]]) -> str:
    """Ticks and interpolated frames at each sample, so a run that stalled,
    never reached the world, or stopped interpolating shows where."""
    lines = ["samples:"]
    for label, seen in trace:
        reasons = ", ".join(f"{name} {count}" for name, count in seen.skipped.items() if count)
        lines.append(
            f"  {label}: {seen.ticks} ticks, {seen.framesInterpolated} interpolated; {reasons}"
        )
    return "\n".join(lines)


def walk_paired(port: int, windows: int) -> PairedRates:
    """Walks with interpolation off and on by turns, and leaves it on."""
    rates = PairedRates()
    for index in range(windows):
        on = index % 2 == 1
        set_continuous(on, port=port)
        x, y = WALK_DIRECTIONS[index % len(WALK_DIRECTIONS)]
        left_stick(x, y, port=port)
        start = read_interpolation(port)
        started = time.monotonic()
        time.sleep(PAIRED_SECONDS)
        end = read_interpolation(port)
        rates.add(on, end.ticks - start.ticks, time.monotonic() - started)
    release(port=port)
    set_continuous(True, port=port)
    return rates


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--save", type=Path, help="save folder; defaults to $WIIUPORT_SAVE")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=90)
    parser.add_argument("--presses", type=int, default=PRESSES)
    parser.add_argument("--interval", type=int, default=INTERVAL_SECONDS)
    parser.add_argument("--walk", type=int, default=24, help="seconds to walk while measuring")
    parser.add_argument(
        "--paired",
        type=int,
        default=PAIRED_WINDOWS,
        help="alternating windows, off then on, that compare the title's rate in the same scenes",
    )
    parser.add_argument("--snapshot", type=int, default=16, help="consecutive frames to keep")
    args = parser.parse_args(argv)
    if args.paired < 2:
        parser.error("--paired needs at least 2 windows: one with interpolation off, one on")

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
    out = layout.activity_dir("continuous-run")

    session = HeadlessSession(
        layout=layout,
        activity="continuous-run",
        runtime_env=runtime_env(args.port, continuous=True),
    )
    with session:
        session.prepare(keys_source=keys, save_source=save)
        with session.launch(layout.shell_command(game)) as running:
            if not wait_for_channel(args.port, args.boot):
                print("refused: the channel never answered while booting.", file=sys.stderr)
                return 1
            trace: list[tuple[str, Interpolation]] = []
            started = time.monotonic()

            def sample(label: str) -> Interpolation:
                seen = read_interpolation(args.port)
                trace.append((f"t+{int(time.monotonic() - started):>3}s {label}", seen))
                return seen

            try:
                press_into_world(
                    args.port,
                    presses=args.presses,
                    interval=args.interval,
                    still_running=lambda: running.poll() is None,
                    after_press=lambda index: sample(f"press {index}"),
                )

                restart_pacing(args.port)
                before = sample("walk start")
                drawn_before = read_draws(port=args.port)
                walk_started = time.monotonic()
                samples: list[Interpolation] = []
                step = max(1, args.walk // len(WALK_DIRECTIONS))
                for x, y in WALK_DIRECTIONS:
                    left_stick(x, y, port=args.port)
                    time.sleep(step)
                    samples.append(sample(f"walk {x:+.0f},{y:+.0f}"))
                # Which shaders drew what was not blended, over the frames
                # planned while the snapshot fills.
                request_census(CENSUS_FRAMES, port=args.port)
                # And whether the draws that read uniforms rewrite their
                # vertex bytes, over as many frames.
                request_vertex_census(CENSUS_FRAMES, port=args.port)
                body = take_recordings(args.snapshot, port=args.port)
                census = wait_for(lambda: read_census(port=args.port), seconds=60)
                vertex_census = wait_for(lambda: read_vertex_census(port=args.port), seconds=60)
                # Still walking: the control only differs on a moving scene.
                restored = restore_check.take(args.port, in_between=False)
                control = restore_check.take(args.port, in_between=True)
                between = neighbour_check.take(args.port)
                release(port=args.port)
                after = sample("walk end")
                drawn_after = read_draws(port=args.port)
                walk_seconds = time.monotonic() - walk_started
                frames = parse_recordings(body)
                # What was on screen, so a run that never reached the world
                # says so in a picture rather than in a zero.
                arm_capture(args.port, slot=0)
                time.sleep(3)
                screen = read_capture(args.port, slot=0)
                paired = walk_paired(args.port, args.paired)
            except (
                ControlUnavailable,
                restore_check.RestoreCheckRefused,
                neighbour_check.NeighbourCheckRefused,
            ) as unavailable:
                print(render_trace(trace))
                print(f"refused: {unavailable}", file=sys.stderr)
                return 1
            exit_code = running.poll()
        # A frame time on a software rasteriser says nothing about the
        # product, so a run that fell back to one is refused rather than
        # reported beside the numbers it would pollute.
        device = session.rendered_on()
        software = session.is_software_rendered()

    print(render_trace(trace))
    print(f"rendered on {device}")
    window = after.since(before)
    rate = window.ticks / walk_seconds
    print(f"title rate while walking: {rate:.2f} Hz ({window.ticks} ticks in {walk_seconds:.1f} s)")
    try:
        print(drawn_after.since(drawn_before).render())
        print(vertex_census.render())
    except ControlUnavailable as unreported:
        print(f"refused: {unreported}", file=sys.stderr)
        return 1
    print("while walking:")
    print(window.render())
    for index, sample in enumerate(samples):
        part = sample.since(before if index == 0 else samples[index - 1])
        print(f"  leg {index}: {part.framesInterpolated} of {part.ticks} ticks interpolated")
    print(census.render())
    print(restored.render())
    print(control.render())
    print(between.render())
    # Each check's pair comes from one tick, and the two checks from two.
    restored.guest.write_png(out / "restore-title.png")
    restored.other.write_png(out / "restore-restored.png")
    control.guest.write_png(out / "control-title.png")
    control.other.write_png(out / "control-in-between.png")
    between.before.write_png(out / "neighbour-before.png")
    between.between.write_png(out / "neighbour-in-between.png")
    between.after.write_png(out / "neighbour-after.png")
    print(paired.render())
    snapshot = out / "recordings.bin"
    snapshot.write_bytes(body)
    print(
        f"snapshot: {len(frames)} consecutive frames, "
        f"{sum(len(frame.assemblies) for frame in frames)} assemblies "
        f"({sum(not frame.complete for frame in frames)} incomplete)"
    )
    screen.write_png(out / "screen.png")
    print(f"wrote {snapshot} and {out / 'screen.png'}")
    print(f"process exit {exit_code}")

    problems = restore_check.judge(restored, control) + neighbour_check.judge(between)
    for problem in problems:
        print(f"refused: {problem}", file=sys.stderr)
    if problems:
        return 1
    if software:
        print(
            f"refused: the runtime rendered on {device}, a software rasteriser; these "
            "timings describe the CPU, not the product.",
            file=sys.stderr,
        )
        return 1
    if window.ticks == 0:
        print(
            "refused: no tick was counted while walking; the title drew nothing.", file=sys.stderr
        )
        return 1
    if window.framesInterpolated == 0:
        print(
            f"refused: {window.ticks} ticks and not one interpolated. The reasons above "
            "are why; none of them is a pass.",
            file=sys.stderr,
        )
        return 1
    print(
        f"interpolated {window.framesInterpolated} of {window.ticks} ticks "
        f"({100.0 * window.framesInterpolated / window.ticks:.1f}%)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
