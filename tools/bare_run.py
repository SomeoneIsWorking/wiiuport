"""Does the product stay up on its own, and does the channel answer?

The measurement run reported the product exiting cleanly seconds after the title
started, before any window. That is either the product or the harness, and the two
are told apart by running the product with no harness around it at all: no presses,
no census, no gate, just the binary, the disc and a bounded wait.

It reports what it saw either way: the exit code if it exited, the channel's answer
if it answered, and the product's own last words, because "it exited 0" says nothing
about why and the log is where the why is.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.drive import press
from wiiuport.headless import Display, HeadlessSession
from wiiuport.paths import find_layout

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    read_paint,
    wait_for_channel,
)
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True, help="the player's disc image")
    parser.add_argument(
        "--keys", type=Path, default=None, help="keys.txt; defaults to $WIIUPORT_KEYS"
    )
    parser.add_argument(
        "--save", type=Path, default=None, help="a save to stage, so the run is not the keyboard"
    )
    parser.add_argument("--seconds", type=float, default=90.0, help="how long to wait")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument(
        "--display",
        choices=[d.value for d in Display],
        default=Display.GPU.value,
        help="where the run presents: gamescope on the GPU, or Xvfb",
    )
    parser.add_argument(
        "--icd",
        type=Path,
        default=None,
        help="a Vulkan ICD to force, by its json; used to tell a driver crash from "
        "a product crash, and refused as evidence either way",
    )
    parser.add_argument(
        "--presses",
        type=int,
        default=0,
        help="how many button presses to inject, and how long between them. A run that "
        "survives without them and dies with them is telling the truth about itself, and "
        "this is the only difference between the two that the product can see",
    )
    parser.add_argument("--press-interval", type=float, default=8.0)
    parser.add_argument(
        "--boot", type=int, default=120, help="seconds to wait for the channel before pressing"
    )
    args = parser.parse_args(argv)

    layout = find_layout()
    if not layout.shell_binary.is_file():
        print(f"refused: no runtime at {layout.shell_binary}. Build it first.", file=sys.stderr)
        return 1
    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 1
    print(f"game {game}")

    runtime_env = {"VK_ICD_FILENAMES": str(args.icd)} if args.icd else {}
    with (
        HeadlessSession(
            layout=layout,
            activity="bare-run",
            display_server=Display(args.display),
            runtime_env=runtime_env,
        ) as session,
        session.launch(layout.shell_command(game)) as running,
    ):
        # The keys, or the disc cannot be decrypted and the product refuses it by
        # name -- which reads as a mount failure and is not one. This cost a run:
        # the refusal is on the product's stdout, not in its log, and the segfault
        # that followed it in the renderer's teardown looked like a crash in
        # graphics init until the two were told apart.
        session.prepare(keys_source=keys, save_source=args.save)
        print(f"pid {running.pid}, waiting up to {args.seconds:.0f}s", flush=True)
        # Presses go in one at a time and each is reported, because a run that
        # dies "during the press phase" has told you nothing: this says which press,
        # or that it died before the first. They wait for the channel first, because
        # a press sent into a refused connection is a refusal of the tool's own
        # making and looks like a run that could not be driven.
        if args.presses > 0 and not wait_for_channel(args.port, args.boot):
            print(
                "refused: the channel never answered, so nothing could be pressed", file=sys.stderr
            )
            return 1
        for index in range(args.presses):
            if running.poll() is not None:
                print(f"  the product was gone before press {index}", flush=True)
                break
            try:
                press("a" if index % 2 == 0 else "plus", port=args.port)
            except ControlUnavailable as unavailable:
                print(f"  press {index} refused: {str(unavailable)[:80]}", flush=True)
                break
            print(f"  press {index} sent", flush=True)
            time.sleep(args.press_interval)
        deadline = time.monotonic() + args.seconds
        answered_at: float | None = None
        paints_at_10s = -1
        while time.monotonic() < deadline and running.poll() is None:
            time.sleep(2.0)
            try:
                paints = read_paint(args.port).paints
            except ControlUnavailable:
                continue
            if answered_at is None:
                answered_at = time.monotonic() - (deadline - args.seconds)
                print(
                    f"  the channel answered after {answered_at:.0f}s: {paints} paints",
                    flush=True,
                )
            if paints_at_10s < 0 and answered_at is not None:
                paints_at_10s = paints
        code = running.poll()
        if code is None:
            print(f"still running after {args.seconds:.0f}s", flush=True)
        else:
            print(
                f"exited with {code} after "
                f"{args.seconds - max(deadline - time.monotonic(), 0.0):.0f}s",
                flush=True,
            )
        log = session.session_dir / "data" / "Cemu" / "log.txt"
        if log.is_file():
            lines = [l for l in log.read_text(errors="replace").splitlines() if l.strip()]
            print("  its own last words:")
            for line in lines[-6:]:
                print(f"    {line[:150]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
