"""The C++ formatter and linter, pinned in ``uv.lock`` rather than taken from the host.

Their output changes between releases: clang-format reflows code differently,
and clang-tidy gains checks and configuration keys (``ExcludeHeaderFilterRegex``
is unknown to the clang-tidy 18 that Ubuntu 24.04 ships). Taken from the host,
the same tree passes on one machine and fails on the next. Pinned as dev
dependencies, every host and CI run the same release, installed beside the
interpreter by ``uv run --frozen``.
"""

from __future__ import annotations

import sys
from pathlib import Path

LOCKED_BIN = Path(sys.executable).parent
"""Where the locked environment installs executables. Not resolved: the
interpreter is a link into uv's Python, and the tools are beside the link."""


class LockedToolMissing(RuntimeError):
    """A pinned tool is not in the environment running this, so nothing was checked."""


def locked_tool(name: str, bin_dir: Path = LOCKED_BIN) -> Path:
    tool = bin_dir / name
    if not tool.is_file():
        raise LockedToolMissing(
            f"{name} is not at {tool}. It is a locked dev dependency (pyproject.toml); "
            "run this through `uv run --frozen`, which installs it."
        )
    return tool
