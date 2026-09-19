#!/usr/bin/env python3
"""Run every gate. Maintainer tool; verification never routes through a launcher."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wiiuport.paths import ProjectLayoutError, find_layout
from wiiuport.verify import run_all


def main() -> int:
    try:
        layout = find_layout()
    except ProjectLayoutError as error:
        print(f"refused: {error}", file=sys.stderr)
        return 2
    results = run_all(layout)
    for result in results:
        print(result.render())
        print()
    failed = [r.name for r in results if not r.passed]
    print(f"{len(results) - len(failed)} of {len(results)} gates passed")
    if failed:
        print("failed: " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
