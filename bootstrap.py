#!/usr/bin/env python3
"""The runtime's launcher: build it, then start it.

``run.sh`` hands control here. With no arguments the runtime opens the title
the player chose last time, or asks for one on its setup screen; any argument
is the runtime's own (a disc image, ``--title-id``, ``--width``...). It never
runs tests, lint or checks -- those are ``tools/verify.py``.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))

from wiiuport.build import BuildConfig, BuildError, build_product
from wiiuport.paths import ProjectLayoutError, find_layout

from wiiuport import hostdeps


def main(argv: list[str]) -> int:
    try:
        layout = find_layout()
        hostdeps.check()
        print("bringing the runtime up to date (the first build takes a while)", flush=True)
        binary = build_product(
            BuildConfig(layout=layout),
            layout.activity_dir("build"),
            lambda line: print(line, flush=True),
        )
    except (ProjectLayoutError, hostdeps.MissingHostPackages) as refused:
        print(f"refused: {refused}", file=sys.stderr)
        return 2
    except BuildError as failed:
        print(f"refused: {failed}", file=sys.stderr)
        return 1
    os.execv(binary, [str(binary), *argv])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
