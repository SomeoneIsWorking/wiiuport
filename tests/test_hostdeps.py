"""The dependency check must refuse, and must say what it looked at."""

from __future__ import annotations

import pytest
from wiiuport.hostdeps import (
    CEMU_REQUIREMENTS,
    MissingHostPackages,
    Requirement,
    apt_packages,
    check,
    report,
)

ABSENT = Requirement(
    "imaginary library",
    ("imaginary-devel",),
    ("libimaginary-dev",),
    files=("/nonexistent/imaginary.h",),
)
PRESENT = Requirement("python interpreter", ("python3",), ("python3",), executables=("python3",))


def test_refuses_and_names_the_package_and_the_command() -> None:
    with pytest.raises(MissingHostPackages) as raised:
        check((PRESENT, ABSENT))
    message = str(raised.value)
    assert "imaginary library" in message
    assert "sudo dnf install imaginary-devel" in message
    assert "sudo apt install libimaginary-dev" in message
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
    assert "libpng16.a" in libpng.libraries
    assert "libpng-static" in libpng.dnf_packages


def test_no_requirement_hardcodes_a_distribution_specific_library_path() -> None:
    """Fedora uses /usr/lib64 and Debian a triplet directory. A requirement
    that named one absolute library path would be a check that only works on
    the machine it was written on -- which is how the first version of the
    libpng requirement passed locally and failed in CI."""
    for requirement in CEMU_REQUIREMENTS:
        for path in requirement.files:
            assert not path.endswith((".a", ".so")), (
                f"{requirement.name} names a library by absolute path; "
                "use libraries= so it is found in any standard library directory"
            )


def test_a_refusal_says_which_part_is_absent_not_only_the_name() -> None:
    with pytest.raises(MissingHostPackages) as raised:
        check((ABSENT,))
    assert "/nonexistent/imaginary.h" in str(raised.value)


def test_libraries_are_found_in_any_standard_directory() -> None:
    from wiiuport.hostdeps import LIBRARY_DIRECTORIES, find_library

    assert "/usr/lib64" in LIBRARY_DIRECTORIES
    assert "/usr/lib/x86_64-linux-gnu" in LIBRARY_DIRECTORIES
    assert find_library("definitely-not-a-real-library.a") is None


def test_every_requirement_names_what_provides_it_on_both_distributions() -> None:
    """The Ubuntu release container installs from the apt names, so one left
    out is a release build that fails hours in, or links without it."""
    for requirement in CEMU_REQUIREMENTS:
        assert requirement.dnf_packages and requirement.apt_packages, requirement.name
    assert "libasound2-dev" in apt_packages()
