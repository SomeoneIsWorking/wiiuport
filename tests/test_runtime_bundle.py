"""The runtime bundle: what it carries, what it leaves to the host, what it refuses."""

from __future__ import annotations

import subprocess
from collections.abc import Sequence
from pathlib import Path

import pytest
from wiiuport.runtime_bundle import BundleRefused

from wiiuport import runtime_bundle

LDD_OUTPUT = """\
\tlinux-vdso.so.1 (0x00007ffc5a1f2000)
\tlibbluetooth.so.3 => /lib64/libbluetooth.so.3 (0x00007f0e4c000000)
\tlibstdc++.so.6 => /lib64/libstdc++.so.6 (0x00007f0e4bc00000)
\tlibc.so.6 => /lib64/libc.so.6 (0x00007f0e4b800000)
\t/lib64/ld-linux-x86-64.so.2 (0x00007f0e4c200000)
"""


def test_ldd_output_is_read_into_every_library() -> None:
    libraries = runtime_bundle.parse_ldd(LDD_OUTPUT)
    assert [library.name for library in libraries] == [
        "linux-vdso.so.1",
        "libbluetooth.so.3",
        "libstdc++.so.6",
        "libc.so.6",
        "ld-linux-x86-64.so.2",
    ]
    assert libraries[1].path == Path("/lib64/libbluetooth.so.3")


def test_only_what_a_desktop_cannot_be_assumed_to_have_is_bundled() -> None:
    bundled = runtime_bundle.libraries_to_bundle(runtime_bundle.parse_ldd(LDD_OUTPUT))
    assert [library.name for library in bundled] == ["libbluetooth.so.3", "libstdc++.so.6"]


@pytest.mark.parametrize(
    ("output", "reason"),
    [
        ("\tlibmissing.so.1 => not found\n", "not installed"),
        ("\tstatically linked, somehow\n", "cannot read"),
        ("\n\n", "no libraries"),
    ],
)
def test_ldd_output_that_would_ship_a_broken_package_is_refused(output: str, reason: str) -> None:
    with pytest.raises(BundleRefused, match=reason):
        runtime_bundle.parse_ldd(output)


def test_the_glibc_floor_is_the_newest_version_named() -> None:
    symbols = "memcpy GLIBC_2.14\nfoo GLIBC_2.39\nbar GLIBC_2.2.5\nbaz GLIBC_2.9\n"
    assert runtime_bundle.glibc_floor(symbols) == "2.39"
    with pytest.raises(BundleRefused):
        runtime_bundle.glibc_floor("no versions here")


def test_an_executable_naming_a_private_path_is_refused(tmp_path: Path) -> None:
    home = Path("/home/maintainer")
    binary = tmp_path / "wiiuport"
    binary.write_bytes(b"\x7fELF...wiiuport/src/Main.cpp\x00/build/wiiuport/lib\x00")
    runtime_bundle.refuse_build_paths(binary, (home,))
    binary.write_bytes(b"\x7fELF.../home/maintainer/wiiuport/src/Main.cpp\x00")
    with pytest.raises(BundleRefused, match="names /home/maintainer 1 times"):
        runtime_bundle.refuse_build_paths(binary, (home,))


def _fake_tools(library: Path, private: bytes) -> runtime_bundle.Runner:
    def run(command: Sequence[str]) -> subprocess.CompletedProcess[str]:
        if command[0] == "ldd":
            out = f"\tlibc.so.6 => /lib/libc.so.6 (0x1)\n\tlibfoo.so.1 => {library} (0x2)\n"
        elif command[0] == "objdump":
            out = "x GLIBC_2.34\ny GLIBC_2.39\n"
        else:
            Path(command[2]).write_bytes(b"ELF" + private)
            out = ""
        return subprocess.CompletedProcess(list(command), 0, out, "")

    return run


def _built_runtime(tmp_path: Path) -> tuple[Path, Path]:
    binary = tmp_path / "bin" / "wiiuport"
    for data in runtime_bundle.DATA_DIRECTORIES:
        (binary.parent / data).mkdir(parents=True)
        (binary.parent / data / "file").write_text(data)
    binary.write_bytes(b"ELF")
    library = tmp_path / "libfoo.so.1"
    library.write_bytes(b"lib")
    return binary, library


def test_a_bundle_carries_the_executable_its_data_and_what_the_host_lacks(tmp_path: Path) -> None:
    binary, library = _built_runtime(tmp_path)
    bundle = tmp_path / "bundle"
    manifest = runtime_bundle.stage(binary, bundle, (), _fake_tools(library, b""))
    assert manifest["glibc_floor"] == "2.39"
    assert manifest["bundled"] == ["libfoo.so.1"]
    assert (bundle / "usr/lib/libfoo.so.1").read_bytes() == b"lib"
    assert (bundle / "usr/bin/resources/file").read_text() == "resources"
    assert runtime_bundle.read_manifest(bundle) == manifest


def test_a_bundle_is_not_staged_over_another_or_with_a_private_path(tmp_path: Path) -> None:
    binary, library = _built_runtime(tmp_path)
    existing = tmp_path / "existing"
    existing.mkdir()
    with pytest.raises(BundleRefused, match="already exists"):
        runtime_bundle.stage(binary, existing, (), _fake_tools(library, b""))
    with pytest.raises(BundleRefused, match="names /home/me"):
        runtime_bundle.stage(
            binary, tmp_path / "bundle", (Path("/home/me"),), _fake_tools(library, b"/home/me/x")
        )
    with pytest.raises(BundleRefused, match="not a staged runtime bundle"):
        runtime_bundle.read_manifest(existing)


def test_no_library_family_is_split_between_the_host_and_the_bundle() -> None:
    for family in runtime_bundle.LIBRARY_FAMILIES:
        from_host = family & runtime_bundle.HOST_PROVIDED
        assert from_host in (frozenset(), family), sorted(family)
