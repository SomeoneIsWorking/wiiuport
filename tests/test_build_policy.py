"""Build policy: Ninja is required, and the configured compiler is read back."""

from __future__ import annotations

from pathlib import Path

import pytest
from wiiuport.build import GENERATOR, BuildConfig, BuildError, configure, verify_toolchain
from wiiuport.paths import Layout

from wiiuport import build


def _tree(tmp_path: Path, cache: str | None) -> BuildConfig:
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "project-goals.md").write_text("x")
    (tmp_path / "external" / "cemu").mkdir(parents=True)
    (tmp_path / "external" / "cemu" / "CMakeLists.txt").write_text("x")
    config = BuildConfig(layout=Layout(root=tmp_path))
    if cache is not None:
        config.build_dir.mkdir(parents=True)
        (config.build_dir / "CMakeCache.txt").write_text(cache)
    return config


def test_generator_is_ninja() -> None:
    assert GENERATOR == "Ninja"


def test_configure_refuses_a_tree_left_by_another_generator(tmp_path: Path) -> None:
    config = _tree(tmp_path, "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n")
    with pytest.raises(BuildError) as raised:
        configure(config)
    assert "Unix Makefiles" in str(raised.value)
    assert str(config.build_dir) in str(raised.value)


def _detected(config: BuildConfig, compiler_id: str | None) -> BuildConfig:
    """Write the compiler id where CMake actually writes it.

    Earlier versions of these tests invented a CMAKE_CXX_COMPILER_ID cache
    entry. CMake never writes one -- it goes in CMakeCXXCompiler.cmake -- so
    the tests agreed with a production bug that read the cache, and every
    configure died early enough that no real tree ever contradicted them.
    """
    detected = config.build_dir / "CMakeFiles" / "3.31.0" / "CMakeCXXCompiler.cmake"
    detected.parent.mkdir(parents=True, exist_ok=True)
    body = 'set(CMAKE_CXX_COMPILER "/usr/bin/clang++")\n'
    if compiler_id is not None:
        body += f'set(CMAKE_CXX_COMPILER_ID "{compiler_id}")\n'
    body += 'set(CMAKE_CXX_COMPILER_VERSION "18.1.8")\n'
    detected.write_text(body, encoding="utf-8")
    return config


def test_unconfigured_tree_is_not_accepted_as_evidence(tmp_path: Path) -> None:
    config = _tree(tmp_path, None)
    with pytest.raises(BuildError, match="does not record a C\\+\\+ compiler id"):
        verify_toolchain(config)


def test_a_configured_tree_that_never_detected_a_compiler_is_refused(
    tmp_path: Path,
) -> None:
    """The case that actually happened in CI: a complete 1089-entry cache whose
    compiler id simply is not a cache variable."""
    config = _tree(tmp_path, "CMAKE_GENERATOR:INTERNAL=Ninja\n")
    with pytest.raises(BuildError) as raised:
        verify_toolchain(config)
    message = str(raised.value)
    assert "compiler detection never completed" in message
    assert "CMAKE_GENERATOR:INTERNAL=Ninja" in message


def test_a_tree_configured_with_another_compiler_is_refused(tmp_path: Path) -> None:
    config = _detected(_tree(tmp_path, "CMAKE_GENERATOR:INTERNAL=Ninja\n"), "GNU")
    with pytest.raises(BuildError) as raised:
        verify_toolchain(config)
    assert "GNU" in str(raised.value) and "Clang" in str(raised.value)


def test_a_clang_tree_is_accepted(tmp_path: Path) -> None:
    """The positive this check had never once shown before CI produced a real
    configured tree."""
    verify_toolchain(_detected(_tree(tmp_path, "CMAKE_GENERATOR:INTERNAL=Ninja\n"), "Clang"))


def test_a_detection_file_without_an_id_is_refused(tmp_path: Path) -> None:
    config = _detected(_tree(tmp_path, "CMAKE_GENERATOR:INTERNAL=Ninja\n"), None)
    with pytest.raises(BuildError) as raised:
        verify_toolchain(config)
    assert "records no compiler id" in str(raised.value)


def test_a_failed_command_inlines_its_log_tail(tmp_path: Path) -> None:
    """A refusal that only names a path prints nothing wherever the tree does
    not outlive the run, which is every hosted job."""
    log = tmp_path / "configure.log"
    with pytest.raises(BuildError) as refusal:
        build._run(["false"], log=log, what="configure")
    message = str(refusal.value)
    assert str(log) in message
    assert "--- last " in message


def test_a_failed_command_without_a_log_still_refuses(tmp_path: Path) -> None:
    with pytest.raises(BuildError) as refusal:
        build._run(["false"], log=None, what="configure")
    assert "configure failed" in str(refusal.value)
    assert "--- last " not in str(refusal.value)


def test_a_missing_cache_and_a_stalled_cache_refuse_differently(tmp_path: Path) -> None:
    """The two causes of "no compiler id" must not read identically: cmake
    never wrote a cache, or it wrote one and stopped before compiler detection."""
    missing = build._cache_evidence(tmp_path)
    assert "no cache at all" in missing

    cache = tmp_path / "CMakeCache.txt"
    cache.write_text(
        "# comment\n//doc\nCMAKE_GENERATOR:INTERNAL=Ninja\nSOMETHING:BOOL=ON\n",
        encoding="utf-8",
    )
    stalled = build._cache_evidence(tmp_path)
    assert "2 entries" in stalled
    assert "CMAKE_GENERATOR:INTERNAL=Ninja" in stalled
    assert "no cache at all" not in stalled


def test_a_cache_without_generator_entries_says_so(tmp_path: Path) -> None:
    (tmp_path / "CMakeCache.txt").write_text("SOMETHING:BOOL=ON\n", encoding="utf-8")
    assert "(none of them are set)" in build._cache_evidence(tmp_path)


def test_the_vcpkg_environment_does_not_add_force_system_binaries(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Applying it to every host was unexplained policy; upstream does not.

    The assertion is that the build tool does not introduce it, not that the
    ambient environment lacks it -- a caller who exports it deliberately is
    still obeyed, and a bare "not in" check would pass or fail by accident
    depending on the shell the tests were started from.
    """
    monkeypatch.delenv("VCPKG_FORCE_SYSTEM_BINARIES", raising=False)
    assert "VCPKG_FORCE_SYSTEM_BINARIES" not in build._vcpkg_environment()

    monkeypatch.setenv("VCPKG_FORCE_SYSTEM_BINARIES", "1")
    assert build._vcpkg_environment()["VCPKG_FORCE_SYSTEM_BINARIES"] == "1"


def test_every_project_in_the_build_records_paths_relative_to_the_checkout() -> None:
    config = BuildConfig(layout=Layout(root=Path(__file__).resolve().parent.parent))
    command = build.configure_command(config, config.layout.cemu_source)
    assert f"-DCMAKE_PROJECT_INCLUDE={config.reproducible_paths}" in command
    included = config.reproducible_paths.read_text()
    assert "-ffile-prefix-map=${WIIUPORT_CHECKOUT}/=wiiuport/" in included
    assert "include_guard(GLOBAL)" in included
