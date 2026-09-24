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
    ``dnf_packages`` and ``apt_packages`` name what provides it on Fedora and on
    Debian-derived hosts: the refusal message on each, and the release build's
    Ubuntu container, install from them.

    ``libraries`` names library files to find in any standard library
    directory, rather than at one absolute path, so the same requirement holds
    on Fedora and on Debian-derived hosts.
    """

    name: str
    dnf_packages: tuple[str, ...]
    apt_packages: tuple[str, ...]
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
GATE_REQUIREMENTS: tuple[Requirement, ...] = (
    Requirement("C++ compiler (clang)", ("clang",), ("clang",), executables=("clang", "clang++")),
    Requirement("CMake", ("cmake",), ("cmake",), executables=("cmake",)),
    Requirement("Ninja", ("ninja-build",), ("ninja-build",), executables=("ninja",)),
)
"""What the C++ gates need. Smaller than the runtime's list: a gate compiles
first-party code and parses it, and needs none of Cemu's dependencies."""

CEMU_REQUIREMENTS: tuple[Requirement, ...] = (
    Requirement("C++ compiler (clang)", ("clang",), ("clang",), executables=("clang", "clang++")),
    Requirement("CMake", ("cmake",), ("cmake",), executables=("cmake",)),
    Requirement("Ninja", ("ninja-build",), ("ninja-build",), executables=("ninja",)),
    # The fork builds Release and RelWithDebInfo with link-time optimisation,
    # and clang's LTO objects are LLVM bitcode that only LLVM's archiver can
    # index. Ubuntu's clang package leaves it out; without it CMake writes
    # CMAKE_C_COMPILER_AR-NOTFOUND into every static-library rule.
    Requirement(
        "LLVM archiver (clang link-time optimisation)",
        ("llvm",),
        ("llvm",),
        executables=("llvm-ar", "llvm-ranlib"),
    ),
    Requirement("nasm", ("nasm",), ("nasm",), executables=("nasm",)),
    Requirement("perl", ("perl-core",), ("perl",), executables=("perl",)),
    Requirement(
        "pkg-config", ("pkgconf-pkg-config",), ("pkg-config",), executables=("pkg-config",)
    ),
    Requirement("zlib headers", ("zlib-devel",), ("zlib1g-dev",), files=("/usr/include/zlib.h",)),
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
        ("libpng-dev",),
        files=("/usr/include/png.h",),
        libraries=("libpng16.a",),
    ),
    Requirement(
        "GTK 3", ("gtk3-devel",), ("libgtk-3-dev",), files=("/usr/include/gtk-3.0/gtk/gtk.h",)
    ),
    Requirement("glm", ("glm-devel",), ("libglm-dev",), files=("/usr/include/glm/glm.hpp",)),
    # The setup screen's font engine. setup-ui refuses without it rather than
    # drawing a screen with no text, and the refusal is easier to act on here,
    # before a build that takes hours.
    Requirement(
        "freetype",
        ("freetype-devel",),
        ("libfreetype-dev",),
        files=("/usr/include/freetype2/ft2build.h",),
    ),
    # cairo is the fourth of Cemu's empty Linux overlay ports (with gtk3, glm and
    # libpng), so vcpkg deliberately takes it from the distribution. It arrives as a
    # GTK 3 dependency on both Fedora and Debian, which is exactly why an undeclared
    # requirement like this stays invisible until something stops pulling it in.
    Requirement(
        "cairo", ("cairo-devel",), ("libcairo2-dev",), files=("/usr/include/cairo/cairo.h",)
    ),
    Requirement(
        "libsecret",
        ("libsecret-devel",),
        ("libsecret-1-dev",),
        files=("/usr/include/libsecret-1/libsecret/secret.h",),
    ),
    Requirement(
        "libgcrypt", ("libgcrypt-devel",), ("libgcrypt20-dev",), executables=("libgcrypt-config",)
    ),
    Requirement(
        "libusb",
        ("libusb1-devel",),
        ("libusb-1.0-0-dev",),
        files=("/usr/include/libusb-1.0/libusb.h",),
    ),
    Requirement(
        "bluez",
        ("bluez-libs-devel",),
        ("libbluetooth-dev",),
        files=("/usr/include/bluetooth/bluetooth.h",),
    ),
    Requirement(
        "systemd", ("systemd-devel",), ("libsystemd-dev",), files=("/usr/include/systemd/sd-bus.h",)
    ),
    # udev is probed separately from systemd although Fedora ships both in
    # systemd-devel, because a vcpkg port configures with udev support and fails on
    # "checking for libudev.h... no". Probing sd-bus.h passed here while the header
    # the build actually consumes went unchecked -- the same shape of gap as libpng.
    Requirement("udev", ("systemd-devel",), ("libudev-dev",), files=("/usr/include/libudev.h",)),
    Requirement(
        "freeglut", ("freeglut-devel",), ("freeglut3-dev",), files=("/usr/include/GL/freeglut.h",)
    ),
    # cubeb's audio backend on Linux; the runtime plays through ALSA.
    Requirement(
        "ALSA", ("alsa-lib-devel",), ("libasound2-dev",), files=("/usr/include/alsa/asoundlib.h",)
    ),
    Requirement(
        "wayland-protocols",
        ("wayland-protocols-devel",),
        ("wayland-protocols",),
        files=("/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml",),
    ),
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
    dnf = sorted({p for r in missing for p in r.dnf_packages})
    apt = sorted({p for r in missing for p in r.apt_packages})
    lines = [
        f"{len(missing)} of {len(requirements)} host requirements are not satisfied:",
        *(f"  - {r.name}\n      absent: {', '.join(r.missing_parts())}" for r in missing),
        "",
        "Install them and re-run. On Fedora:",
        "  sudo dnf install " + " ".join(dnf),
        "On Debian or Ubuntu:",
        "  sudo apt install " + " ".join(apt),
    ]
    raise MissingHostPackages("\n".join(lines))


def apt_packages(requirements: tuple[Requirement, ...] = CEMU_REQUIREMENTS) -> list[str]:
    """Every Debian package the requirements name, for a host built from scratch."""
    return sorted({package for r in requirements for package in r.apt_packages})


def report(requirements: tuple[Requirement, ...] = CEMU_REQUIREMENTS) -> str:
    """A full present/absent listing, so a clean host still prints what was checked."""
    rows = [f"{'OK ' if r.satisfied() else 'MISSING'}  {r.name}" for r in requirements]
    satisfied = sum(1 for r in requirements if r.satisfied())
    return "\n".join([*rows, f"-- {satisfied} of {len(requirements)} satisfied"])
