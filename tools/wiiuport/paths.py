"""The single source of truth for every path this project resolves.

No other module joins path fragments. Build outputs live under the gitignored
top-level ``build/``; run artifacts live under the gitignored ``scratch/``.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


class ProjectLayoutError(RuntimeError):
    """A required directory or file is absent, named with the path tried."""


@dataclass(frozen=True)
class Layout:
    """Resolved locations inside one wiiuport checkout."""

    root: Path
    build_type: str = "RelWithDebInfo"

    @property
    def cemu_source(self) -> Path:
        return self.root / "external" / "cemu"

    @property
    def build(self) -> Path:
        return self.root / "build"

    @property
    def cemu_build(self) -> Path:
        return self.build / "cemu"

    @property
    def cemu_binary(self) -> Path:
        """Where the runtime executable actually lands.

        Upstream's CMake puts it in the source tree's ``bin/`` rather than the
        build tree, and that is deliberate on its part: the application
        resolves its data directory as the executable's parent, and ``bin/``
        holds the tracked ``resources/`` and ``gameProfiles/default/`` it reads
        at startup. Relocating it would mean staging that data too, so the fork
        is left alone here and the path is followed instead. Object files,
        generated build files and the vcpkg tree all still live under
        ``build/``, and upstream's own .gitignore covers ``bin/Cemu_*`` so a
        built binary can never be committed to the submodule.
        """
        return self.cemu_source / "bin" / f"Cemu_{self.build_type.lower()}"

    @property
    def scratch(self) -> Path:
        return self.root / "scratch"

    @property
    def docs(self) -> Path:
        return self.root / "docs"

    def require_cemu_source(self) -> Path:
        """Return the pinned fork's source tree, or refuse by naming what is missing.

        An absent or unpopulated submodule is a refusal rather than a silent
        fall back to any other Cemu on the machine.
        """
        source = self.cemu_source
        marker = source / "CMakeLists.txt"
        if not marker.is_file():
            raise ProjectLayoutError(
                "the pinned Cemu fork is not checked out: expected "
                f"{marker}. Run: git -C {self.root} submodule update --init --recursive"
            )
        return source

    def activity_dir(self, activity: str) -> Path:
        """One fixed reusable directory per recurring probe, never a dated run."""
        path = self.scratch / activity
        path.mkdir(parents=True, exist_ok=True)
        return path


def find_layout(start: Path | None = None) -> Layout:
    """Locate the checkout root by walking up from ``start`` to a wiiuport marker."""
    current = (start or Path(__file__)).resolve()
    for candidate in (current, *current.parents):
        if (candidate / "docs" / "project-goals.md").is_file() and (
            candidate / "external"
        ).is_dir():
            return Layout(root=candidate)
    raise ProjectLayoutError(
        f"no wiiuport checkout found at or above {current}: looked for a directory "
        "holding both docs/project-goals.md and external/"
    )
