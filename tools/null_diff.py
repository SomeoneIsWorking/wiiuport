#!/usr/bin/env python3
"""Compare what a replay drew against the frame it recorded.

Replay has been shown to submit and to be survived. Neither says the image is
right, and an interpolated frame built on a replay that draws something else
would be wrong in a way counters cannot see. This drives the title into
gameplay, captures a presented frame, replays the recording behind it and
captures again, then reports how far apart the two images are.

At t=1 -- no transform substituted -- a faithful replay is the same image.
This tool does not assume that; it measures it and prints the distribution,
because "identical" and "nearly identical" are different findings and the
second one is the interesting one.

It measures two arms, because one alone cannot attribute anything. The title
keeps running between captures, so two frames differ whether or not a replay
happened. The control arm captures the same pair with no replay in between;
only the difference between the arms says anything about the replay.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.drive import arm_replay, press, release
from wiiuport.headless import HeadlessSession
from wiiuport.image import Image, arm_capture, read_capture
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys

from wiiuport.control import DEFAULT_PORT, ControlUnavailable, read_counters, read_transforms

ENV_CONTROL_PORT = "WIIUPORT_CONTROL_PORT"


def compare(before: Image, after: Image) -> tuple[int, int, float]:
    """Differing bytes, the largest single difference, and the mean.

    Reported together because one changed pixel and a different image are
    both "not identical" and nothing else distinguishes them.
    """
    if (before.width, before.height) != (after.width, after.height):
        raise ControlUnavailable(
            f"the two captures are {before.width}x{before.height} and "
            f"{after.width}x{after.height}; a replay that changed the resolution is a "
            "finding in itself and they cannot be compared byte for byte"
        )
    differing = 0
    largest = 0
    total = 0
    for a, b in zip(before.rgb, after.rgb, strict=True):
        delta = abs(a - b)
        if delta:
            differing += 1
            total += delta
            largest = max(largest, delta)
    return differing, largest, total / len(before.rgb)


def capture_now(port: int, settle: float = 3.0) -> Image:
    arm_capture(port)
    time.sleep(settle)
    return read_capture(port)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=90)
    parser.add_argument("--presses", type=int, default=12)
    parser.add_argument("--interval", type=int, default=8)
    parser.add_argument(
        "--gap", type=int, default=3, help="seconds between the two captures of each arm"
    )
    parser.add_argument("--out", type=Path, help="where to write the two PNGs")
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
    out = args.out or layout.activity_dir("null-diff")

    session = HeadlessSession(
        layout=layout,
        activity="null-diff",
        runtime_env={ENV_CONTROL_PORT: str(args.port)},
    )
    with session:
        session.prepare(keys_source=keys)
        with session.launch([str(binary), "--game", str(game)]) as running:
            deadline = time.monotonic() + args.boot
            ready = False
            while time.monotonic() < deadline and not ready:
                time.sleep(5)
                try:
                    read_counters(args.port)
                    ready = True
                except ControlUnavailable:
                    continue
            if not ready:
                print("refused: the channel never answered while booting.", file=sys.stderr)
                return 1
            for index in range(args.presses):
                if running.poll() is not None:
                    break
                press("a" if index % 2 == 0 else "plus", port=args.port)
                time.sleep(args.interval)
            release(port=args.port)
            time.sleep(5)

            try:
                report = read_transforms(args.port)
                # Control arm first: the same spacing, nothing replayed. Its
                # difference is what the running title does on its own, and
                # it is the only thing that makes the other arm readable.
                control_before = capture_now(args.port)
                time.sleep(args.gap)
                control_after = capture_now(args.port)
                # Replay arm: identical spacing, one replay inside it.
                before = capture_now(args.port)
                counters_before = read_counters(args.port)
                arm_replay(args.port)
                time.sleep(args.gap)
                after = capture_now(args.port)
                counters_after = read_counters(args.port)
            except ControlUnavailable as unavailable:
                print(f"refused: {unavailable}", file=sys.stderr)
                return 1
            exit_code = running.poll()

    if report.sharedAndMoving == 0:
        print(
            "refused: this run never reached a moving view, so the frames compared would "
            "be a title screen and a null diff over them proves nothing.",
            file=sys.stderr,
        )
        return 1
    if counters_after.replaysRun == counters_before.replaysRun:
        print(
            "refused: the replay never ran between the two captures, so the second image "
            "is simply a later frame and the comparison is meaningless.",
            file=sys.stderr,
        )
        return 1

    before.write_png(out / "before.png")
    after.write_png(out / "after.png")
    control_before.write_png(out / "control-before.png")
    control_after.write_png(out / "control-after.png")
    control_differing, control_largest, control_mean = compare(control_before, control_after)
    differing, largest, mean = compare(before, after)
    total = len(before.rgb)
    pixels = before.width * before.height
    print(f"{before.width}x{before.height}, {pixels} pixels, {total} bytes")
    print(
        f"replays {counters_before.replaysRun} -> {counters_after.replaysRun}, "
        f"{counters_after.replayListsSubmitted - counters_before.replayListsSubmitted} "
        f"lists submitted between the captures"
    )
    print(
        f"control (no replay): {control_differing} bytes differ "
        f"({100.0 * control_differing / total:.3f}%), largest {control_largest}, "
        f"mean {control_mean:.4f}"
    )
    print(
        f"replay:              {differing} bytes differ "
        f"({100.0 * differing / total:.3f}%), largest {largest}, mean {mean:.4f}"
    )
    print(f"wrote four PNGs to {out}")
    print(f"process exit {exit_code}")

    if control_differing == 0 and differing == 0:
        print(
            "both arms are identical, so this scene does not change between captures and "
            "the comparison has no power to detect a replay that drew something else. "
            "Re-run somewhere the view is moving."
        )
        return 1
    if differing == 0:
        print("identical across the replay, while the control moved: the replay is faithful")
        return 0
    print(
        f"not identical. Against a control that differs by {control_differing} bytes, the "
        f"replay arm differs by {differing}. A replay arm close to the control is the title "
        "advancing rather than the replay drawing something else; a much larger one is the "
        "replay. This run does not separate them on its own -- the four PNGs are the evidence."
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
