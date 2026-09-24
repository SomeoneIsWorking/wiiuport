"""Build the release runtime in a pinned Ubuntu container.

A runtime built on the maintainer's own host needs that host's glibc and names
its home in the paths some dependencies compile in. The release runtime is
built instead from a clean copy of the committed checkout, inside a container
on an older, widely installed glibc, at ``/build`` -- a path that names no one.

The copy is cloned from the local checkouts, submodules included, so a release
is built from exactly what is committed and needs no network for the sources.
The container installs the packages the host requirements name for Debian, so
the release and a maintainer's host are checked against one list.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
from collections.abc import Callable, Sequence
from pathlib import Path, PurePosixPath

from . import hostdeps
from .paths import Layout

BASE_IMAGE = (
    "docker.io/library/ubuntu:24.04"
    "@sha256:496754492fb28b4d3049432f2ca787449331e23fb14f0dd3fffea86bf5a93eb4"
)
"""Ubuntu 24.04, glibc 2.39: the oldest release whose own compiler library has
<format>, which the runtime uses. Pinned by digest."""

UV_VERSION = "0.10.12"
"""The uv the container runs the project's tools with, pinned."""

BUILD_TOOLS: tuple[str, ...] = (
    "autoconf",
    "autoconf-archive",
    "automake",
    "bison",
    "build-essential",
    "ca-certificates",
    "curl",
    "file",
    "flex",
    "git",
    "libltdl-dev",
    "libtool",
    "python3",
    "python3-pip",
    "tar",
    "unzip",
    "zip",
)
"""What a bare image lacks that vcpkg's port builds and the tools assume."""

CONTAINER_ROOT = PurePosixPath("/build")
CONTAINER_CHECKOUT = CONTAINER_ROOT / "wiiuport"
CONTAINER_BUNDLE = CONTAINER_ROOT / "bundle"
CONTAINER_HOME = CONTAINER_ROOT / "home"
"""vcpkg's binary cache lives under HOME, so HOME is kept between releases."""


class ReleaseRefused(RuntimeError):
    """The release runtime cannot be built as it must be; the message says why."""


Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


def _run(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(command), capture_output=True, text=True, check=False)


def _checked(command: Sequence[str], run: Runner) -> str:
    result = run(command)
    if result.returncode != 0:
        raise ReleaseRefused(
            f"{' '.join(command)} failed ({result.returncode}):\n{result.stdout}{result.stderr}"
        )
    return result.stdout


def containerfile() -> str:
    packages = sorted({*BUILD_TOOLS, *hostdeps.apt_packages()})
    return (
        f"FROM {BASE_IMAGE}\n"
        "RUN apt-get update \\\n"
        " && DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y \\\n"
        + "".join(f"    {package} \\\n" for package in packages)
        + " && rm -rf /var/lib/apt/lists/*\n"
        f"RUN python3 -m pip install --break-system-packages uv=={UV_VERSION}\n"
    )


def image_tag() -> str:
    """Named by its definition, so a changed package list is a new image."""
    digest = hashlib.sha256(containerfile().encode()).hexdigest()[:16]
    return f"localhost/wiiuport-release:{digest}"


def ensure_image(context: Path, run: Runner = _run) -> str:
    tag = image_tag()
    if run(["podman", "image", "exists", tag]).returncode == 0:
        return tag
    context.mkdir(parents=True, exist_ok=True)
    (context / "Containerfile").write_text(containerfile())
    _checked(["podman", "build", "--tag", tag, str(context)], run)
    return tag


def refuse_uncommitted(root: Path, run: Runner = _run) -> str:
    """The commit a release is built from; refused if the tree differs from it."""
    changes = _checked(
        ["git", "-C", str(root), "status", "--porcelain", "--ignore-submodules=none"], run
    )
    if changes.strip():
        raise ReleaseRefused(
            "a release is built from what is committed, and this checkout has changes:\n"
            + changes.rstrip()
        )
    return _checked(["git", "-C", str(root), "rev-parse", "HEAD"], run).strip()


def _submodules(repository: Path, run: Runner) -> list[tuple[str, str]]:
    """(name, path) of each submodule a repository declares."""
    if not (repository / ".gitmodules").is_file():
        return []
    listed = _checked(
        [
            "git",
            "-C",
            str(repository),
            "config",
            "--file",
            ".gitmodules",
            "--get-regexp",
            r"^submodule\..*\.path$",
        ],
        run,
    )
    found: list[tuple[str, str]] = []
    for line in listed.splitlines():
        key, path = line.split(" ", 1)
        found.append((key.removeprefix("submodule.").removesuffix(".path"), path))
    return found


def _sync_submodules(copy: Path, source: Path, run: Runner) -> None:
    """Each submodule the source has checked out, cloned from the source's own copy.

    A submodule the source never initialised is left uninitialised: the copy
    builds from what the maintainer's checkout builds from, nothing more.
    """
    for name, path in _submodules(copy, run):
        if not (source / path / ".git").exists():
            continue
        _checked(
            ["git", "-C", str(copy), "config", f"submodule.{name}.url", str(source / path)], run
        )
        _checked(
            [
                "git",
                "-C",
                str(copy),
                "-c",
                "protocol.file.allow=always",
                "submodule",
                "update",
                "--init",
                "--force",
                "--",
                path,
            ],
            run,
        )
        _sync_submodules(copy / path, source / path, run)


def sync_checkout(source: Path, copy: Path, commit: str, run: Runner = _run) -> None:
    """Bring the release copy to ``commit`` of ``source``, submodules included."""
    if not (copy / ".git").exists():
        copy.parent.mkdir(parents=True, exist_ok=True)
        _checked(["git", "clone", "--no-checkout", str(source), str(copy)], run)
    _checked(["git", "-C", str(copy), "fetch", str(source), commit], run)
    _checked(["git", "-C", str(copy), "checkout", "--force", "--detach", commit], run)
    _sync_submodules(copy, source, run)


def container_command(tag: str, layout: Layout, private_paths: Sequence[Path]) -> list[str]:
    """Build the runtime in the copy and stage its bundle, inside the container."""
    stage = [
        "uv",
        "run",
        "--frozen",
        "python",
        "tools/stage_runtime.py",
        str(CONTAINER_BUNDLE),
        *(argument for path in private_paths for argument in ("--private-path", str(path))),
    ]
    script = "uv run --frozen python tools/build_runtime.py && " + " ".join(stage)
    return [
        "podman",
        "run",
        "--rm",
        "--volume",
        f"{layout.release}:{CONTAINER_ROOT}:Z",
        "--env",
        f"HOME={CONTAINER_HOME}",
        "--workdir",
        str(CONTAINER_CHECKOUT),
        tag,
        "sh",
        "-c",
        script,
    ]


def build_release_runtime(
    layout: Layout, private_paths: Sequence[Path], log: Path, run: Runner = _run
) -> Path:
    """The staged release bundle, built from the committed checkout."""
    if shutil.which("podman") is None:
        raise ReleaseRefused(
            "the release runtime is built in a container and podman is not installed; "
            "on Fedora: sudo dnf install podman"
        )
    commit = refuse_uncommitted(layout.root, run)
    sync_checkout(layout.root, layout.release_checkout, commit, run)
    (layout.release / CONTAINER_HOME.name).mkdir(parents=True, exist_ok=True)
    tag = ensure_image(layout.release / "image", run)
    if layout.release_bundle.exists():
        shutil.rmtree(layout.release_bundle)
    with log.open("w") as sink:
        built = subprocess.run(
            container_command(tag, layout, private_paths),
            stdout=sink,
            stderr=subprocess.STDOUT,
            check=False,
        )
    if built.returncode != 0:
        tail = log.read_text(errors="replace").splitlines()[-40:]
        raise ReleaseRefused(
            f"the container build failed ({built.returncode}); log: {log}\n" + "\n".join(tail)
        )
    return layout.release_bundle
