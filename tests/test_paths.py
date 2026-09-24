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


def test_binary_path_follows_the_data_root_layout(tmp_path: Path) -> None:
    """The core resolves its data directory as the executable's parent, so the
    binary must sit beside bin/resources rather than in the build tree."""
    layout = Layout(root=tmp_path)
    assert layout.shell_binary == tmp_path / "external" / "cemu" / "bin" / "wiiuport"
    assert layout.shell_binary.parent.name == "bin"


def test_the_binary_name_does_not_depend_on_the_build_type(tmp_path: Path) -> None:
    """One product, one name: a tool that finds no binary must be looking at
    the build, not at a name it guessed from a configuration."""
    assert Layout(root=tmp_path, build_type="Debug").shell_binary.name == "wiiuport"


def test_the_product_takes_its_title_as_a_positional_argument(tmp_path: Path) -> None:
    """The product refuses an unknown option, so a flag here fails at launch.

    This is the shape five maintainer tools got wrong at once when the shell
    replaced Cemu's front end: each kept passing ``--game`` and each exited
    with a code that read as the title failing to load.
    """
    layout = Layout(root=tmp_path)
    command = layout.shell_command(Path("/games/title.wux"))
    assert command == [str(layout.shell_binary), "/games/title.wux"]
    assert not any(argument.startswith("-") for argument in command[1:])


def test_the_product_launches_with_no_arguments_at_all(tmp_path: Path) -> None:
    """A packaged player runs it with nothing; that path has to be reachable."""
    layout = Layout(root=tmp_path)
    assert layout.shell_command() == [str(layout.shell_binary)]


def test_a_consumers_title_is_named_as_the_product_takes_it(tmp_path: Path) -> None:
    layout = Layout(root=tmp_path)
    command = layout.shell_command(Path("/games/title.wux"), title_id="0005000010143500")
    assert command == [
        str(layout.shell_binary),
        "--title-id",
        "0005000010143500",
        "/games/title.wux",
    ]


def test_no_save_named_is_a_legitimate_answer_and_a_wrong_one_is_not(tmp_path) -> None:
    """Absent means "start a new game"; a typo must not read as absent."""
    import pytest
    from wiiuport.title import TitleUnavailable, resolve_save

    assert resolve_save(None) is None
    with pytest.raises(TitleUnavailable, match="is not a directory"):
        resolve_save(tmp_path / "typo")
    real = tmp_path / "10143500"
    real.mkdir()
    assert resolve_save(real) == real
