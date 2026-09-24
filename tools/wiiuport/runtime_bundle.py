"""A relocatable copy of the runtime: what any title's package is made from.

The bundle holds the stripped executable with the data it reads beside it, the
libraries a desktop cannot be assumed to have, and a manifest naming the glibc
it needs and what was bundled. It is title-neutral: a title project adds its
own launcher, name and icon around it.

It is staged where the runtime was built, because the libraries it bundles are
that host's. A release is built in the Ubuntu container of ``release.py`` so
the glibc floor is an old one and no compiled path names the person who built
it.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

MANIFEST = "runtime.json"
"""Written at the bundle's root; read by the title project that packages it."""

EXECUTABLE = Path("usr/bin/wiiuport")
LIBRARIES = Path("usr/lib")
DATA_DIRECTORIES: tuple[str, ...] = ("resources", "gameProfiles")
"""Read at startup from beside the executable."""

HOST_PROVIDED: frozenset[str] = frozenset(
    {
        # The C library and its loader: the one thing a package must take from the host.
        "linux-vdso.so.1",
        "ld-linux-x86-64.so.2",
        "libc.so.6",
        "libm.so.6",
        "libgcc_s.so.1",
        # The display, input and font stack every desktop session already runs; a
        # bundled copy can disagree with the host's server or drivers.
        "libX11.so.6",
        "libxcb.so.1",
        "libXau.so.6",
        "libXdmcp.so.6",
        "libXrender.so.1",
        "libwayland-client.so.0",
        "libffi.so.8",
        "libudev.so.1",
        "libasound.so.2",
        "libfreetype.so.6",
        "libharfbuzz.so.0",
        "libgraphite2.so.3",
        "libpng16.so.16",
        "libbrotlidec.so.1",
        "libbrotlicommon.so.1",
        "libbz2.so.1",
        "libbz2.so.1.0",
        "libz.so.1",
        "libglib-2.0.so.0",
        "libpcre2-8.so.0",
        "libbsd.so.0",
        "libmd.so.0",
    }
)
"""Libraries left to the host. Everything else the executable links is bundled,
so a new dependency is carried by default rather than missing on a player's
machine."""

_LDD_LINE = re.compile(r"^\s*(?P<name>\S+)(?: => (?P<path>\S+|not found))?(?: \(0x[0-9a-f]+\))?$")


class BundleRefused(RuntimeError):
    """The bundle could not be made as it must be; the message says why."""


@dataclass(frozen=True)
class Library:
    name: str
    path: Path


def parse_ldd(output: str) -> list[Library]:
    """Every library `ldd` resolved. A library it could not find, or a line it
    did not recognise, refuses: either would ship a package that fails to
    start on the player's machine."""
    libraries: list[Library] = []
    for line in output.splitlines():
        if not line.strip():
            continue
        match = _LDD_LINE.match(line)
        if match is None:
            raise BundleRefused(f"ldd printed a line this cannot read: {line.strip()}")
        name, path = match.group("name"), match.group("path")
        if path == "not found":
            raise BundleRefused(f"{name} is not installed, so the executable cannot start")
        libraries.append(Library(Path(name).name, Path(path) if path else Path(name)))
    if not libraries:
        raise BundleRefused("ldd listed no libraries at all, which no dynamic executable has")
    return libraries


def libraries_to_bundle(libraries: Sequence[Library]) -> list[Library]:
    return [library for library in libraries if library.name not in HOST_PROVIDED]


def glibc_floor(binary_symbols: str) -> str:
    """The newest GLIBC symbol version the executable needs, from `objdump -T`."""
    versions = {
        tuple(int(part) for part in found.split("."))
        for found in re.findall(r"GLIBC_(\d+(?:\.\d+)+)", binary_symbols)
    }
    if not versions:
        raise BundleRefused("the executable names no glibc version, so its floor is unknown")
    return ".".join(str(part) for part in max(versions))


def refuse_build_paths(binary: Path, private_paths: Sequence[Path]) -> None:
    """Refuse an executable that names a private build path.

    A path compiled in -- ``__FILE__`` in an assert or a log line, a library's
    configured install directory -- would hand every player the builder's
    home. The runtime's build maps its own sources relative to its checkout;
    dependencies that record their install prefix must be built somewhere
    that names no one.
    """
    contents = binary.read_bytes()
    for path in private_paths:
        found = contents.count(str(path).encode())
        if found:
            raise BundleRefused(
                f"{binary} names {path} {found} times; build the release runtime in the "
                "container (tools/build_release_runtime.py), where no path names its builder"
            )


Runner = Callable[[Sequence[str]], subprocess.CompletedProcess[str]]


def _run(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(command), capture_output=True, text=True, check=False)


def _output_of(command: Sequence[str], run: Runner) -> str:
    result = run(command)
    if result.returncode != 0:
        raise BundleRefused(f"{command[0]} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def stage(
    product_binary: Path,
    bundle: Path,
    private_paths: Sequence[Path],
    run: Runner = _run,
) -> dict[str, object]:
    """Lay the bundle out at ``bundle`` (which must not exist) and return its manifest."""
    if bundle.exists():
        raise BundleRefused(f"{bundle} already exists; stage into a fresh directory")
    libraries = parse_ldd(_output_of(["ldd", str(product_binary)], run))
    bundled = libraries_to_bundle(libraries)
    floor = glibc_floor(_output_of(["objdump", "-T", str(product_binary)], run))
    executable = bundle / EXECUTABLE
    library_dir = bundle / LIBRARIES
    executable.parent.mkdir(parents=True)
    library_dir.mkdir(parents=True)
    _output_of(["strip", "-o", str(executable), str(product_binary)], run)
    refuse_build_paths(executable, private_paths)
    for data in DATA_DIRECTORIES:
        source = product_binary.parent / data
        if not source.is_dir():
            raise BundleRefused(f"the runtime's {data} directory is missing at {source}")
        shutil.copytree(source, executable.parent / data)
    for library in bundled:
        shutil.copy2(library.path, library_dir / library.name)
    manifest: dict[str, object] = {
        "executable": EXECUTABLE.as_posix(),
        "libraries": LIBRARIES.as_posix(),
        "glibc_floor": floor,
        "bundled": [library.name for library in bundled],
        "linked": len(libraries),
    }
    (bundle / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def read_manifest(bundle: Path) -> dict[str, object]:
    """The manifest of a staged bundle, refusing a directory that is not one."""
    path = bundle / MANIFEST
    if not path.is_file():
        raise BundleRefused(f"{bundle} is not a staged runtime bundle: {path} is missing")
    return json.loads(path.read_text())
