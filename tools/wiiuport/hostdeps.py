"""Host package requirements, and an actionable refusal when one is absent.

Never installs anything. A missing system package is reported by exact name with
the exact privileged command the user runs, as required by the project's
platform policy.
"""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from pathlib import Path

LIBRARY_DIRECTORIES: tuple[str, ...] = (
    "/usr/lib64",
    "/usr/lib",
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib/aarch64-linux-gnu",
    "/usr/local/lib64",
    "/usr/local/lib",
)
"""Where a distribution may put libraries. Fedora uses ``/usr/lib64`` while
Debian and Ubuntu use a triplet directory, so a requirement that named one
absolute path would be a check that only works on the machine it was written
on."""


@dataclass(frozen=True)
class Requirement:
    """One host capability, and how to prove it is present without a package query.

    Probing for real files and executables rather than package names keeps the
    check honest across distributions that split or rename packages.
    ``dnf_packages`` is only used to build the refusal message.

    ``libraries`` names library files to find in any standard library
    directory, rather than at one absolute path, so the same requirement holds
    on Fedora and on Debian-derived hosts.
    """

    name: str
    dnf_packages: tuple[str, ...]
    files: tuple[str, ...] = ()
    executables: tuple[str, ...] = ()
    libraries: tuple[str, ...] = ()

    def satisfied(self) -> bool:
        return not self.missing_parts()

    def missing_parts(self) -> list[str]:
        """Exactly what is absent, so a refusal can say more than the name."""
        absent = [f for f in self.files if not Path(f).exists()]
        absent += [e for e in self.executables if shutil.which(e) is None]
        absent += [lib for lib in self.libraries if find_library(lib) is None]
        return absent


def find_library(name: str) -> Path | None:
    """Locate a library file in any standard library directory."""
    for directory in LIBRARY_DIRECTORIES:
        candidate = Path(directory) / name
        if candidate.exists():
            return candidate
    return None


# Derived from the pinned fork's BUILD.md Fedora list, expressed as capabilities.
# Fedora 44 satisfies zlib through zlib-ng-compat and the perl-core content
# through the base perl split packages, so a package-name check would refuse a
# host that is in fact ready.
CEMU_REQUIREMENTS: tuple[Requirement, ...] = (
    Requirement("C++ compiler (clang)", ("clang",), executables=("clang", "clang++")),
    Requirement("CMake", ("cmake",), executables=("cmake",)),
    Requirement("Ninja", ("ninja-build",), executables=("ninja",)),
    Requirement("nasm", ("nasm",), executables=("nasm",)),
    Requirement("perl", ("perl-core",), executables=("perl",)),
    Requirement("pkg-config", ("pkgconf-pkg-config",), executables=("pkg-config",)),
    Requirement("zlib headers", ("zlib-devel",), files=("/usr/include/zlib.h",)),
    # Cemu makes libpng an empty vcpkg package on Linux
    # (dependencies/vcpkg_overlay_ports_linux/libpng) so the distro's libpng is
    # used. Fedora's libpng-devel then ships a CMake config declaring
    # PNG::png_static -> /usr/lib64/libpng16.a, but that archive comes from the
    # separate libpng-static subpackage. Without it find_package(PNG) fails
    # outright, so the static archive -- not the header -- is what must be
    # probed.
    Requirement(
        "libpng, including the static archive its CMake config declares",
        ("libpng-devel", "libpng-static"),
        files=("/usr/include/png.h",),
        libraries=("libpng16.a",),
    ),
    Requirement("GTK 3", ("gtk3-devel",), files=("/usr/include/gtk-3.0/gtk/gtk.h",)),
    Requirement("glm", ("glm-devel",), files=("/usr/include/glm/glm.hpp",)),
    Requirement("libsecret", ("libsecret-devel",), files=("/usr/include/libsecret-1/libsecret/secret.h",)),
    Requirement("libgcrypt", ("libgcrypt-devel",), executables=("libgcrypt-config",)),
    Requirement("libusb", ("libusb1-devel",), files=("/usr/include/libusb-1.0/libusb.h",)),
    Requirement("bluez", ("bluez-libs-devel",), files=("/usr/include/bluetooth/bluetooth.h",)),
    Requirement("systemd", ("systemd-devel",), files=("/usr/include/systemd/sd-bus.h",)),
    Requirement("freeglut", ("freeglut-devel",), files=("/usr/include/GL/freeglut.h",)),
    Requirement("wayland-protocols", ("wayland-protocols-devel",),
                files=("/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml",)),
)


class MissingHostPackages(RuntimeError):
    """Named missing host capabilities plus the exact command to install them."""


def check(requirements: tuple[Requirement, ...] = CEMU_REQUIREMENTS) -> None:
    """Refuse by exact name if any requirement is absent; otherwise return.

    The negative case is explicit on purpose: it names every capability that was
    probed and how many passed, so a silent pass cannot be mistaken for a check
    that never ran.
    """
    missing = [r for r in requirements if not r.satisfied()]
    if not missing:
        return
    packages = sorted({p for r in missing for p in r.dnf_packages})
    lines = [
        f"{len(missing)} of {len(requirements)} host requirements are not satisfied:",
        *(
            f"  - {r.name}\n      absent: {', '.join(r.missing_parts())}"
            for r in missing
        ),
        "",
        "Install them and re-run. On Fedora:",
        "  sudo dnf install " + " ".join(packages),
    ]
    raise MissingHostPackages("\n".join(lines))


def report(requirements: tuple[Requirement, ...] = CEMU_REQUIREMENTS) -> str:
    """A full present/absent listing, so a clean host still prints what was checked."""
    rows = [f"{'OK ' if r.satisfied() else 'MISSING'}  {r.name}" for r in requirements]
    satisfied = sum(1 for r in requirements if r.satisfied())
    return "\n".join([*rows, f"-- {satisfied} of {len(requirements)} satisfied"])
