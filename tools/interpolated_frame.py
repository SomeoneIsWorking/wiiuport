#!/usr/bin/env python3
"""Draw one frame between two the title drew, and require it to be different.

This is interpolation at its smallest: the last frame's geometry, replayed
with the view transform blended between where the camera stood in that frame
and the one before it. If that works, the image must change. If it does not
change, one of four things is true and they are counted apart here -- the
replay never reached the renderer, the shaders carrying the view were not
among the draws replayed, the buffers they assembled were too short, or the
blend was written and the screen did not move.

It is also the discriminator the null diff cannot supply. A replay that
redraws the frame already in the colour buffer looks identical whether it
drew or not, so "identical" there is not evidence of a faithful replay. A
substituted camera has to change the image, which makes this the run that
says whether a replay draws at all.
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
    arm_interpolated_frame,
    bounding_box,
    compare,
    read_capture,
)
from wiiuport.paths import find_layout
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys, resolve_save

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    TransformReport,
    read_counters,
    read_frames,
    read_substitution,
    read_transforms,
    runtime_env,
)


def render_trace(trace: list[tuple[int, int, int]]) -> str:
    """How many shaders were drawing at each sample, so a run that never
    reached the world is distinguishable from one that reached it and moved
    on. A trace with one value throughout is itself the finding."""
    if not trace:
        return "no samples taken: nothing was read while the title was driven"
    widest = max(shaders for _, _, shaders in trace)
    lines = [f"shaders drawing, sampled while driving (peak {widest}):"]
    lines += [
        f"  t+{seconds:>3}s  frame {frames:>6}  {shaders:>4} shaders"
        for seconds, frames, shaders in trace
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument(
        "--save",
        type=Path,
        help="the title's save folder, copied into the session; defaults to $WIIUPORT_SAVE. "
        "Without one the title starts a new game and stops at its name-entry keyboard.",
    )
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--boot", type=int, default=90)
    parser.add_argument("--presses", type=int, default=PRESSES)
    parser.add_argument("--interval", type=float, default=INTERVAL_SECONDS)
    parser.add_argument(
        "--t",
        type=float,
        default=0.5,
        help="where between the two frames; at 1 the image must be identical to the title's",
    )
    parser.add_argument("--settle", type=int, default=5)
    parser.add_argument(
        "--reach",
        type=int,
        default=60,
        help="seconds to keep trying to arm while the title keeps drawing",
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
    if not layout.shell_binary.is_file():
        print(f"refused: no runtime at {layout.shell_binary}. Build it first.", file=sys.stderr)
        return 2
    out = args.out or layout.activity_dir("interpolated-frame")

    session = HeadlessSession(
        layout=layout,
        activity="interpolated-frame",
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
            # What the title is drawing, sampled while it is driven. Arming at
            # the end of a fixed press sequence measures whatever screen that
            # sequence happened to land on; this shows whether the world was
            # ever on screen, and when.
            trace: list[tuple[int, int, int]] = []
            started = time.monotonic()

            def sample() -> TransformReport:
                seen = read_transforms(args.port)
                trace.append(
                    (int(time.monotonic() - started), seen.framesObserved, seen.shadersInLastFrame)
                )
                return seen

            report = sample()
            press_into_world(
                args.port,
                presses=args.presses,
                interval=args.interval,
                still_running=lambda: running.poll() is None,
                after_press=lambda _index: sample(),
            )
            report = sample()

            # Keep trying while sampling: the view can only be offered while
            # the shaders that carry it are drawing, so one attempt at a fixed
            # moment arms against whatever is on screen then.
            deadline = time.monotonic() + args.reach
            slots = 0
            before = read_counters(args.port)
            while True:
                try:
                    before = read_counters(args.port)
                    slots = arm_interpolated_frame(args.port, t=args.t)
                    break
                except ControlUnavailable as refused:
                    if time.monotonic() >= deadline:
                        print(render_trace(trace))
                        print(read_frames(args.port).render())
                        # What the title was showing when it refused. A
                        # refusal that cannot say which screen the run was
                        # sitting on sends the next person back to guessing.
                        arm_capture(args.port, slot=0)
                        time.sleep(args.settle)
                        read_capture(args.port, slot=0).write_png(out / "refused.png")
                        print(f"wrote {out / 'refused.png'}")
                        print(f"refused: {refused}", file=sys.stderr)
                        return 1
                    time.sleep(2)
                    report = sample()

            try:
                time.sleep(args.settle)
                after = read_counters(args.port)
                substitution = read_substitution(args.port)
                read_window = read_frames(args.port)
                title_frame = read_capture(args.port, slot=0)
                blended_frame = read_capture(args.port, slot=1)
            except ControlUnavailable as unavailable:
                print(f"refused: {unavailable}", file=sys.stderr)
                return 1
            exit_code = running.poll()

    print(report.render())
    print(f"armed at t={args.t} over {slots} shaders carrying the view")
    if after.nullDiffsCompleted == before.nullDiffsCompleted:
        print(
            "refused: the runtime never completed the captured pair, so the two slots do "
            "not hold one frame drawn two ways.",
            file=sys.stderr,
        )
        return 1

    packets = after.runtimePacketsProcessed - before.runtimePacketsProcessed
    draws = after.runtimeDrawsIssued - before.runtimeDrawsIssued
    submissions = after.runtimeSubmissions - before.runtimeSubmissions
    assemblies = after.uniformAssembliesFromRuntime - before.uniformAssembliesFromRuntime
    lists = after.displayListsFromRuntime - before.displayListsFromRuntime
    substituted = after.assembliesSubstituted - before.assembliesSubstituted
    print(after.render())
    print(read_window.render())
    print(substitution.render())
    print(
        f"{submissions} submissions of the "
        f"{after.replayListsSubmitted - before.replayListsSubmitted} lists the replay "
        f"submitted ({after.replaysRun - before.replaysRun} replays, from a recorded frame "
        f"of {before.lastFrameDisplayLists} lists and {before.lastFrameUniformAssemblies} "
        f"assemblies in {before.lastFrameBytes} bytes): {packets} packets walked, "
        f"{draws} draws issued"
    )
    print(
        f"the replay produced {lists} display lists and {assemblies} uniform assemblies "
        f"of the runtime's own; the view was written into {substituted} of them "
        f"({after.assembliesUnknownShader - before.assembliesUnknownShader} did not carry "
        f"it, {after.assembliesTooShort - before.assembliesTooShort} were too short, "
        f"{after.assembliesUnarmed - before.assembliesUnarmed} arrived unarmed)"
    )

    title_frame.write_png(out / "title.png")
    blended_frame.write_png(out / "interpolated.png")
    differing, largest, mean = compare(title_frame, blended_frame)
    total = len(title_frame.rgb)
    print(
        f"title frame vs interpolated frame: {differing} of {total} bytes differ "
        f"({100.0 * differing / total:.3f}%), largest {largest}, mean {mean:.4f}"
    )
    print(f"wrote {out / 'title.png'} and {out / 'interpolated.png'}")
    print(f"process exit {exit_code}")

    if packets == 0:
        print(
            "refused: the command processor walked no packets of what was submitted, so "
            "the buffer was never read. Nothing downstream of that can be measured.",
            file=sys.stderr,
        )
        return 1
    if draws == 0:
        print(
            f"refused: {packets} packets were walked and no draw came out of them. What "
            "was recorded is state without geometry, or the geometry is in buffers the "
            "recording does not hold.",
            file=sys.stderr,
        )
        return 1
    if assemblies == 0:
        print(
            f"refused: {draws} draws were issued and no uniform assembly came back, so the "
            "renderer dropped them before a shader ran. Until that is fixed, no image this "
            "produces says anything about interpolation.",
            file=sys.stderr,
        )
        return 1
    if substituted == 0:
        print(
            "refused: the replay drew, but none of the draws carried the view at the "
            "offset the search found it. The blend was written nowhere.",
            file=sys.stderr,
        )
        return 1
    if args.t == 1.0:
        # The null: blended all the way to the title's own frame, the replay
        # must reproduce it exactly. Reached through the same substitution as
        # t=0.5 -- which differs -- so identical here is not a replay that drew
        # nothing.
        if differing != 0:
            print(
                f"refused: at t=1 the view written into {substituted} draws is the title's "
                f"own, and the image still differs in {differing} bytes at "
                f"{bounding_box(title_frame, blended_frame)}. The replay does not reproduce "
                "the frame it replays.",
                file=sys.stderr,
            )
            return 1
        print(
            f"null: at t=1, {substituted} draws substituted and the replay is byte-identical "
            "to the title's frame"
        )
        return 0
    if differing == 0:
        print(
            "refused: the view was substituted into "
            f"{substituted} draws and the image did not change by a single byte. A "
            "camera that moves and a frame that does not is the substitution reaching a "
            "buffer nothing reads.",
            file=sys.stderr,
        )
        return 1
    print(f"the interpolated frame differs at {bounding_box(title_frame, blended_frame)}")
    print("interpolated: a blended camera moved the image the title's own frame did not show")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
