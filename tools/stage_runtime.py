#!/usr/bin/env python3
"""Stage the built runtime as a relocatable bundle. Maintainer tool.

Run where the runtime was built -- for a release, inside the container of
tools/build_release_runtime.py -- because the libraries it bundles are that
host's.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wiiuport.paths import ProjectLayoutError, find_layout
from wiiuport.runtime_bundle import BundleRefused, stage


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="where to stage; must not exist")
    parser.add_argument(
        "--private-path",
        type=Path,
        action="append",
        default=[],
        help="a path the executable must not name (repeatable)",
    )
    args = parser.parse_args(argv)
    try:
        layout = find_layout()
        manifest = stage(layout.shell_binary, args.bundle, args.private_path)
    except (ProjectLayoutError, BundleRefused) as refused:
        print(f"refused: {refused}", file=sys.stderr)
        return 2
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
