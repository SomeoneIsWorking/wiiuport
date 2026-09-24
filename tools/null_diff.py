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

Both images come from one frame. An earlier version captured them seconds
apart and measured the scene advancing: 0.122% of bytes differed with no
replay at all, against 0.173% with one. The runtime now arms both halves
around a single frame boundary -- the title's present just before it, the
replay's just after.

That is still not enough on its own, so this runs a control first: the same
sequence with nothing replayed, which re-presents the colour buffer the title
just presented and must therefore be byte-identical. If it is not, the
capture or present path is the difference and nothing the replay arm reports
can be believed. That control exists because the first same-frame run
reported exactly the 7,604 differing bytes an unrelated earlier measurement
had, which is not how a real difference behaves.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.gameplay import INTERVAL_SECONDS, PRESSES, press_into_world
from wiiuport.headless import HeadlessSession
from wiiuport.image import (
    arm_capture,
    arm_null_diff,
    bounding_box,
    compare,
    read_capture,
)
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys, resolve_save

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    read_counters,
    read_transforms,
    runtime_env,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=90)
    parser.add_argument(
        "--save",
        type=Path,
        help="the title's save folder, copied into the session; defaults to $WIIUPORT_SAVE. "
        "Without one the title starts a new game and stops at its name-entry keyboard.",
    )
    parser.add_argument("--presses", type=int, default=PRESSES)
    parser.add_argument("--interval", type=float, default=INTERVAL_SECONDS)
    parser.add_argument(
        "--settle", type=int, default=5, help="seconds to wait for both captures to land"
    )
    parser.add_argument("--out", type=Path, help="where to write the two PNGs")
    args = parser.parse_args(argv)

    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
        save = resolve_save(args.save)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2

    layout = find_layout()
    binary = layout.shell_binary
    if not binary.is_file():
        print(f"refused: no runtime at {binary}. Build it first.", file=sys.stderr)
        return 2
    out = args.out or layout.activity_dir("null-diff")

    session = HeadlessSession(
        layout=layout,
        activity="null-diff",
        runtime_env=runtime_env(args.port, continuous=False),
    )
    with session:
        session.prepare(keys_source=keys, save_source=save)
        with session.launch(layout.shell_command(game)) as running:
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
            press_into_world(
                args.port,
                presses=args.presses,
                interval=args.interval,
                still_running=lambda: running.poll() is None,
            )

            try:
                report = read_transforms(args.port)
                # The control: present the same buffer again, replaying
                # nothing. Anything but zero here is the instrument.
                arm_null_diff(args.port, redraw=False)
                time.sleep(args.settle)
                control_title = read_capture(args.port, slot=0)
                control_replay = read_capture(args.port, slot=1)
                control_counters = read_counters(args.port)

                # What the title alone changes between two of its own
                # presents. If the control above matches this, the second
                # capture landed on a guest frame rather than on the
                # runtime's present, and the fault is in the capture path.
                arm_capture(args.port, slot=0)
                time.sleep(args.settle)
                arm_capture(args.port, slot=1)
                time.sleep(args.settle)
                guest_first = read_capture(args.port, slot=0)
                guest_second = read_capture(args.port, slot=1)

                counters_before = read_counters(args.port)
                arm_null_diff(args.port, redraw=True)
                time.sleep(args.settle)
                counters_after = read_counters(args.port)
                # Slot 0 is the frame as the title presented it, slot 1 the
                # same frame as the replay redrew it. Which image landed
                # where was decided when each was armed, so the order the
                # renderer's detached threads delivered them cannot matter.
                title_frame = read_capture(args.port, slot=0)
                replay_frame = read_capture(args.port, slot=1)
                # The same recording replayed again over the first: what the
                # renderer does not draw the same way twice.
                second_replay = read_capture(args.port, slot=2)
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
    pixels_hint = len(control_title.rgb)
    if counters_after.nullDiffsCompleted == counters_before.nullDiffsCompleted:
        print(
            "refused: the runtime never completed a null diff, so the two slots do not "
            "hold two views of one frame and comparing them would measure nothing.",
            file=sys.stderr,
        )
        return 1

    control_differing, control_largest, control_mean = compare(control_title, control_replay)
    if control_differing != 0:
        control_title.write_png(out / "control-title.png")
        control_replay.write_png(out / "control-replay.png")
        guest_differing, guest_largest, _ = compare(guest_first, guest_second)
        print(
            f"refused: the control re-presented the title's own colour buffer and "
            f"{control_differing} of {len(control_title.rgb)} bytes still differed "
            f"(largest {control_largest}, mean {control_mean:.4f}), at "
            f"{bounding_box(control_title, control_replay)}. Nothing was replayed.",
            file=sys.stderr,
        )
        print(
            f"  two of the title's own presents differ by {guest_differing} bytes "
            f"(largest {guest_largest}) at {bounding_box(guest_first, guest_second)}",
            file=sys.stderr,
        )
        print(
            f"  runtime presents submitted {control_counters.presentsSubmitted}, "
            f"observed {control_counters.presentsObserved} "
            f"({control_counters.presentsObservedTv} TV, "
            f"{control_counters.presentsObservedDrc} GamePad), images received "
            f"{control_counters.imagesReceived}",
            file=sys.stderr,
        )
        print(
            "  a control that matches the title's own frame-to-frame change means the "
            "second capture landed on a guest frame, not on the runtime's present.",
            file=sys.stderr,
        )
        print(
            f"  wrote {out / 'control-title.png'} and {out / 'control-replay.png'}", file=sys.stderr
        )
        return 1
    print(f"control: re-presenting the same buffer is byte-identical over {pixels_hint} bytes")

    title_frame.write_png(out / "title.png")
    replay_frame.write_png(out / "replay.png")
    differing, largest, mean = compare(title_frame, replay_frame)
    total = len(title_frame.rgb)
    pixels = title_frame.width * title_frame.height
    lists = counters_after.replayListsSubmitted - counters_before.replayListsSubmitted
    print(f"{title_frame.width}x{title_frame.height}, {pixels} pixels, {total} bytes")
    print(
        f"null diffs {counters_before.nullDiffsCompleted} -> "
        f"{counters_after.nullDiffsCompleted}, {lists} lists replayed, "
        f"{counters_after.presentsSubmitted - counters_before.presentsSubmitted} "
        f"runtime presents"
    )
    print(
        f"title frame vs replay of the same frame: {differing} bytes differ "
        f"({100.0 * differing / total:.3f}%), largest {largest}, mean {mean:.4f}"
    )
    again, again_largest, _ = compare(replay_frame, second_replay)
    print(
        f"the replay vs the same recording replayed again: {again} bytes differ, "
        f"largest {again_largest}"
    )
    print(f"wrote {out / 'title.png'} and {out / 'replay.png'}")
    print(f"process exit {exit_code}")

    if differing == 0:
        print("identical: the replay reproduced the frame it recorded, byte for byte")
        return 0
    print(
        "not identical. Both images are one frame -- the title's present and the replay's "
        "present of the same recording -- so this difference is the replay, not the scene "
        "advancing. The two PNGs show where. Where two replays of the recording differ as "
        "much, the renderer does not draw the same commands the same way twice."
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
