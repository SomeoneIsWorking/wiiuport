"""Path resolution refuses by naming what it tried."""

from __future__ import annotations

from pathlib import Path

import pytest
from wiiuport.paths import Layout, ProjectLayoutError, find_layout


def test_missing_submodule_is_refused_with_the_recovery_command(tmp_path: Path) -> None:
    layout = Layout(root=tmp_path)
    with pytest.raises(ProjectLayoutError) as raised:
        layout.require_cemu_source()
    message = str(raised.value)
    assert str(tmp_path / "external" / "cemu" / "CMakeLists.txt") in message
    assert "submodule update --init --recursive" in message


def test_find_layout_refuses_outside_a_checkout(tmp_path: Path) -> None:
    with pytest.raises(ProjectLayoutError, match="no wiiuport checkout"):
        find_layout(tmp_path)


def test_find_layout_locates_the_real_checkout() -> None:
    assert (find_layout().root / "docs" / "project-goals.md").is_file()


def test_build_outputs_stay_under_the_top_level_build_directory(tmp_path: Path) -> None:
    layout = Layout(root=tmp_path)
    assert layout.cemu_build.is_relative_to(layout.build)
    assert not layout.cemu_build.is_relative_to(layout.scratch)


def test_binary_path_follows_upstreams_data_root_layout(tmp_path: Path) -> None:
    """Upstream resolves its data directory as the executable's parent, so the
    binary must sit beside bin/resources rather than in the build tree."""
    layout = Layout(root=tmp_path)
    assert layout.cemu_binary == tmp_path / "external" / "cemu" / "bin" / "Cemu_relwithdebinfo"
    assert layout.cemu_binary.parent.name == "bin"


def test_build_type_selects_the_binary_name(tmp_path: Path) -> None:
    assert Layout(root=tmp_path, build_type="Debug").cemu_binary.name == "Cemu_debug"
