"""The dependency check must refuse, and must say what it looked at."""

from __future__ import annotations

import pytest
from wiiuport.hostdeps import (
    CEMU_REQUIREMENTS,
    MissingHostPackages,
    Requirement,
    check,
    report,
)

ABSENT = Requirement(
    "imaginary library", ("imaginary-devel",), files=("/nonexistent/imaginary.h",)
)
PRESENT = Requirement("python interpreter", ("python3",), executables=("python3",))


def test_refuses_and_names_the_package_and_the_command() -> None:
    with pytest.raises(MissingHostPackages) as raised:
        check((PRESENT, ABSENT))
    message = str(raised.value)
    assert "imaginary library" in message
    assert "sudo dnf install imaginary-devel" in message
    assert "1 of 2" in message, "the refusal must state how many were probed"


def test_passes_when_every_requirement_is_satisfied() -> None:
    check((PRESENT,))


def test_report_lists_every_requirement_including_the_satisfied_ones() -> None:
    """A silent pass must be distinguishable from a check that never ran."""
    text = report((PRESENT, ABSENT))
    assert "OK   python interpreter" in text
    assert "MISSING  imaginary library" in text
    assert "1 of 2 satisfied" in text


def test_real_requirements_probe_files_or_executables_not_package_names() -> None:
    """Fedora 44 renames zlib and splits perl-core; a package-name check would
    refuse a host that is actually ready, so every requirement must probe."""
    for requirement in CEMU_REQUIREMENTS:
        assert requirement.files or requirement.executables, requirement.name


def test_libpng_requirement_probes_the_static_archive_not_just_the_header() -> None:
    """Fedora's libpng CMake config declares PNG::png_static pointing at an
    archive from a different subpackage, and find_package(PNG) fails without
    it. A header-only probe would pass on a host that cannot configure."""
    libpng = next(r for r in CEMU_REQUIREMENTS if "libpng" in r.name)
    assert "/usr/lib64/libpng16.a" in libpng.files
    assert "libpng-static" in libpng.dnf_packages
