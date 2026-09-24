#!/usr/bin/env python3
"""Prove the first-run setup screen on both of its classes.

A packaged product has no command line, so the question is not "does the
screen compile" but "does a player with nothing configured get asked, and does
a player who already answered get their game". Those are opposite branches of
the same decision and a run that only exercises one proves nothing about it.

Positive: an isolated installation with nothing remembered must put the setup
screen on the display and say, over the control channel, what it is waiting
for. Negative: the same installation with a remembered title must never show
the screen and must reach the title instead. Each branch refuses by name when
it is not observed.

With ``--title-id``, the product is launched as a consuming product launches
it, and ``--refuse-as`` adds a third branch: the remembered disc, launched by a
product made for another title, must be refused by name and the player asked
again.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from wiiuport.headless import Display, HeadlessError, HeadlessSession
from wiiuport.paths import Layout, find_layout
from wiiuport.screenshot import ScreenshotUnavailable, capture_display, spread
from wiiuport.title import TitleUnavailable, resolve_game, resolve_keys

from wiiuport.control import (
    DEFAULT_PORT,
    ControlUnavailable,
    read_counters,
    read_setup,
    runtime_env,
)

RECORD_NAME = "selected-title.txt"


def poll(port: int, seconds: float, predicate):
    """Ask the running product until it answers the way we are looking for."""
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        time.sleep(2)
        try:
            last = predicate()
        except ControlUnavailable:
            continue
        if last is not None:
            return last
    return None


# Below this, what is on the display is one flat colour with a little noise:
# a window that never drew, a document that failed to load, or a font engine
# with nothing to draw with. The screen this product shows is text on panels.
MINIMUM_COLOURS = 16


def photograph(session: HeadlessSession) -> int:
    """Look at what the screen actually drew, not at what it says it did."""
    try:
        image = capture_display(session.display)
    except ScreenshotUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2
    measured = spread(image)
    shot = image.write_png(session.session_dir / "setup-screen.png")
    print(f"the screen drew {measured.render()}; written to {shot}")
    if measured.distinct_colours < MINIMUM_COLOURS:
        print(
            f"refused: the setup screen reports itself as shown and the display holds "
            f"{measured.distinct_colours} colours, which is a window that drew nothing",
            file=sys.stderr,
        )
        return 1
    return 0


def show_the_screen(
    session: HeadlessSession,
    layout: Layout,
    port: int,
    seconds: int,
    title_id: str | None,
    product: Path | None,
) -> int:
    """With nothing remembered, the player must be asked."""
    record = session.config_home / "Cemu" / RECORD_NAME
    if record.exists():
        record.unlink()
    # No disc named: the packaged product's own path.
    with session.launch(layout.shell_command(title_id=title_id, product=product)) as running:

        def asked():
            status = read_setup(port)
            return status if status.hostReports and status.shown else None

        status = poll(port, seconds, asked)
        if status is None:
            exited = running.poll()
            print(
                f"refused: nothing was remembered and the setup screen was not shown "
                f"within {seconds}s"
                + (f"; the product exited with {exited}" if exited is not None else ""),
                file=sys.stderr,
            )
            return 1
        print(f"with nothing remembered: {status.render()}")
        if status.selectionsOffered != 0:
            print(
                f"refused: the screen reports {status.selectionsOffered} selections before "
                "anything was chosen, so its counters cannot be trusted",
                file=sys.stderr,
            )
            return 1
        return photograph(session)


def start_from_the_record(
    session: HeadlessSession,
    layout: Layout,
    game: Path,
    port: int,
    seconds: int,
    title_id: str | None,
    product: Path | None,
) -> int:
    """With a title remembered, the player must not be asked again."""
    record = session.config_home / "Cemu" / RECORD_NAME
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(f"{game}\n")
    with session.launch(layout.shell_command(title_id=title_id, product=product)) as running:

        def started():
            if read_counters(port).framesObserved > 0:
                return read_setup(port)
            return None

        status = poll(port, seconds, started)
        if status is None:
            exited = running.poll()
            print(
                f"refused: a remembered title did not reach a frame within {seconds}s"
                + (f"; the product exited with {exited}" if exited is not None else ""),
                file=sys.stderr,
            )
            return 1
        if status.hostReports:
            print(
                f"refused: the title is running and the setup screen is still registered "
                f"({status.render()}), so the two states cannot be told apart",
                file=sys.stderr,
            )
            return 1
        print(f"with a remembered title: it started without asking; {status.render()}")
    return 0


def refuse_another_title(
    session: HeadlessSession, layout: Layout, game: Path, port: int, seconds: int, other: str
) -> int:
    """A remembered disc of another title than the product runs must be
    refused by name, and the player asked again."""
    record = session.config_home / "Cemu" / RECORD_NAME
    record.parent.mkdir(parents=True, exist_ok=True)
    record.write_text(f"{game}\n")
    with session.launch(layout.shell_command(title_id=other)) as running:

        def asked():
            status = read_setup(port)
            return status if status.hostReports and status.shown else None

        status = poll(port, seconds, asked)
        if status is None:
            exited = running.poll()
            print(
                f"refused: a product made for {other} was not asked again for its game "
                f"within {seconds}s"
                + (f"; the product exited with {exited}" if exited is not None else ""),
                file=sys.stderr,
            )
            return 1
    log = (session.session_dir / "run.log").read_text(errors="replace")
    reason = next(
        (line for line in log.splitlines() if "not the game this product runs" in line), None
    )
    if reason is None:
        print(
            "refused: the screen was shown again but the log never names why the "
            "remembered disc was refused",
            file=sys.stderr,
        )
        return 1
    print(f"with another title remembered: asked again; {reason.strip()}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, help="disc image; defaults to $WIIUPORT_GAME")
    parser.add_argument("--keys", type=Path, help="keys.txt; defaults to $WIIUPORT_KEYS")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--ask", type=int, default=60, help="seconds to wait for the screen")
    parser.add_argument("--boot", type=int, default=180, help="seconds to let the title boot")
    parser.add_argument("--title-id", help="the title the product runs, as a consumer names it")
    parser.add_argument(
        "--refuse-as",
        help="a title ID the disc is not: its record must then be refused and setup shown",
    )
    parser.add_argument(
        "--product",
        type=Path,
        help="a packaged product (an AppImage) to launch in place of this checkout's binary",
    )
    args = parser.parse_args(argv)
    if args.product is not None and args.refuse_as is not None:
        parser.error(
            "--refuse-as names another title on the command line, which a packaged product "
            "that fixes its own refuses before any record is read"
        )

    try:
        game = resolve_game(args.game)
        keys = resolve_keys(args.keys)
    except TitleUnavailable as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2

    layout = find_layout()
    binary = layout.shell_binary if args.product is None else args.product
    if not binary.is_file():
        print(f"refused: no product binary at {binary}; build it first", file=sys.stderr)
        return 2

    session = HeadlessSession(
        layout=layout,
        activity="setup-screen",
        runtime_env=runtime_env(args.port),
        # It screenshots the X root window, which only Xvfb exposes.
        display_server=Display.XVFB,
    )
    try:
        with session:
            session.prepare(keys_source=keys)
            failed = show_the_screen(
                session, layout, args.port, args.ask, args.title_id, args.product
            )
            if failed:
                return failed
            failed = start_from_the_record(
                session, layout, game, args.port, args.boot, args.title_id, args.product
            )
            if failed or args.refuse_as is None:
                return failed
            return refuse_another_title(session, layout, game, args.port, args.ask, args.refuse_as)
    except HeadlessError as unavailable:
        print(f"refused: {unavailable}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
