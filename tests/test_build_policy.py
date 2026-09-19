"""Build policy: Ninja is required, and the configured compiler is read back."""

from __future__ import annotations

from pathlib import Path

import pytest

from wiiuport.build import GENERATOR, BuildConfig, BuildError, configure, verify_toolchain
from wiiuport.paths import Layout


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


def test_unconfigured_tree_is_not_accepted_as_evidence(tmp_path: Path) -> None:
    config = _tree(tmp_path, None)
    with pytest.raises(BuildError, match="not configured"):
        verify_toolchain(config)


def test_a_tree_configured_with_another_compiler_is_refused(tmp_path: Path) -> None:
    config = _tree(tmp_path, "CMAKE_CXX_COMPILER_ID:INTERNAL=GNU\n")
    with pytest.raises(BuildError) as raised:
        verify_toolchain(config)
    assert "GNU" in str(raised.value) and "Clang" in str(raised.value)


def test_a_clang_tree_is_accepted(tmp_path: Path) -> None:
    verify_toolchain(_tree(tmp_path, "CMAKE_CXX_COMPILER_ID:INTERNAL=Clang\n"))
