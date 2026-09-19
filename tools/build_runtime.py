#!/usr/bin/env python3
"""Build the pinned Cemu fork. Maintainer tool; not a player entry point."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wiiuport.build import BuildConfig, BuildError, compile_all, configure
from wiiuport.paths import ProjectLayoutError, find_layout

from wiiuport import hostdeps


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-type", default="RelWithDebInfo")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument(
        "--check-deps-only",
        action="store_true",
        help="Report host requirements and exit without building.",
    )
    args = parser.parse_args(argv)

    try:
        layout = find_layout()
    except ProjectLayoutError as error:
        print(f"refused: {error}", file=sys.stderr)
        return 2

    print(hostdeps.report())
    if args.check_deps_only:
        return 0
    try:
        hostdeps.check()
    except hostdeps.MissingHostPackages as error:
        print(f"\nrefused:\n{error}", file=sys.stderr)
        return 2

    config = BuildConfig(layout=layout, build_type=args.build_type)
    logs = layout.activity_dir("build")
    try:
        print(f"configuring {config.build_dir} (log: {logs / 'configure.log'})")
        configure(config, log=logs / "configure.log")
        if args.configure_only:
            return 0
        print(f"compiling (log: {logs / 'compile.log'})")
        binary = compile_all(config, log=logs / "compile.log")
    except BuildError as error:
        print(f"\nrefused: {error}", file=sys.stderr)
        return 1
    print(f"built: {binary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
