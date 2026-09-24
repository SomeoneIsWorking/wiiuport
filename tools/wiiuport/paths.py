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
    def wiiuport_build(self) -> Path:
        """The first-party library and test build tree."""
        return self.build / "wiiuport"

    @property
    def cemu_build(self) -> Path:
        return self.build / "cemu"

    @property
    def release(self) -> Path:
        """The release build's own tree, mounted in its container at ``/build``."""
        return self.build / "release"

    @property
    def release_checkout(self) -> Path:
        """A clean copy of the committed checkout the release runtime is built from."""
        return self.release / "wiiuport"

    @property
    def release_bundle(self) -> Path:
        """The staged runtime bundle a title project packages."""
        return self.release / "bundle"

    @property
    def shell_binary(self) -> Path:
        """Where the product's executable actually lands.

        Upstream's CMake puts it in the source tree's ``bin/`` rather than the
        build tree, and that is deliberate on its part: the application
        resolves its data directory as the executable's parent, and ``bin/``
        holds the tracked ``resources/`` and ``gameProfiles/default/`` it reads
        at startup. Relocating it would mean staging that data too, so the fork
        is left alone here and the path is followed instead. Object files,
        generated build files and the vcpkg tree all still live under
        ``build/``, and the fork's .gitignore covers this name so a built
        binary can never be committed to the submodule.
        """
        return self.cemu_source / "bin" / "wiiuport"

    def shell_command(
        self,
        game: Path | None = None,
        title_id: str | None = None,
        product: Path | None = None,
    ) -> list[str]:
        """How a maintainer tool launches the product.

        One owner, because the product's command line changed when it stopped
        being Cemu's front end: the title is a positional argument and an
        unknown option is refused outright. Five tools kept passing the
        ``--game`` flag it no longer takes, and each one failed at launch with
        an exit code that looked like the title not loading.

        No argument at all is the packaged product's own path: the remembered
        title, or the setup screen. ``title_id`` is what a consuming product
        passes to have any other title refused. ``product`` launches a packaged
        product (a consumer's AppImage) in place of this checkout's binary.
        """
        command = [str(self.shell_binary if product is None else product)]
        if title_id is not None:
            command += ["--title-id", title_id]
        if game is not None:
            command.append(str(game))
        return command

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
