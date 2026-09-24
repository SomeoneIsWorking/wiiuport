#!/usr/bin/env python3
"""Build the release runtime in its container and stage its bundle. Maintainer tool.

The bundle lands in build/release/bundle, with runtime.json naming the glibc it
needs and the libraries it carries. A title project packages it.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wiiuport.paths import ProjectLayoutError, find_layout
from wiiuport.release import ReleaseRefused, build_release_runtime
from wiiuport.runtime_bundle import BundleRefused, read_manifest


def main() -> int:
    try:
        layout = find_layout()
        log = layout.activity_dir("release") / "container.log"
        print(f"building the release runtime in its container (log: {log})", flush=True)
        # The maintainer's home and checkout are the paths a player must not see.
        bundle = build_release_runtime(layout, (Path.home(), layout.root.resolve()), log)
        manifest = read_manifest(bundle)
    except (ProjectLayoutError, ReleaseRefused, BundleRefused) as refused:
        print(f"refused: {refused}", file=sys.stderr)
        return 2
    print(f"staged {bundle}")
    print(f"needs glibc {manifest['glibc_floor']} or newer")
    print(f"bundled {len(manifest['bundled'])} of {manifest['linked']} libraries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
