"""The release build: its image, and the committed copy it is built from."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest
from wiiuport.paths import Layout
from wiiuport.release import ReleaseRefused

from wiiuport import hostdeps, release


def _git(*arguments: str | Path) -> str:
    return subprocess.run(
        ["git", "-c", "protocol.file.allow=always", *map(str, arguments)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout


def _repository(path: Path, content: str) -> Path:
    path.mkdir(parents=True)
    _git("-C", path, "init", "-q", "-b", "main")
    _git("-C", path, "config", "user.email", "t@example.invalid")
    _git("-C", path, "config", "user.name", "t")
    (path / "file").write_text(content)
    _git("-C", path, "add", "file")
    _git("-C", path, "commit", "-q", "-m", "one")
    return path


def test_the_image_installs_what_the_host_requirements_name_on_a_pinned_base() -> None:
    text = release.containerfile()
    assert text.startswith("FROM docker.io/library/ubuntu:24.04@sha256:")
    for package in (*hostdeps.apt_packages(), "git", "build-essential"):
        assert f"    {package} \\\n" in text
    assert f"uv=={release.UV_VERSION}" in text


def test_a_changed_image_definition_is_a_new_image(monkeypatch: pytest.MonkeyPatch) -> None:
    before = release.image_tag()
    monkeypatch.setattr(release, "BUILD_TOOLS", (*release.BUILD_TOOLS, "nasm-extra"))
    assert release.image_tag() != before


def test_a_release_is_refused_from_a_tree_with_changes(tmp_path: Path) -> None:
    source = _repository(tmp_path / "source", "one")
    commit = release.refuse_uncommitted(source)
    assert commit == _git("-C", source, "rev-parse", "HEAD").strip()
    (source / "file").write_text("changed")
    with pytest.raises(ReleaseRefused, match="has changes"):
        release.refuse_uncommitted(source)


def test_the_copy_is_the_commit_with_submodules_cloned_from_the_local_checkouts(
    tmp_path: Path,
) -> None:
    module = _repository(tmp_path / "upstream-module", "module one")
    source = _repository(tmp_path / "source", "one")
    _git("-C", source, "submodule", "add", "-q", module, "external/module")
    _git("-C", source, "commit", "-q", "-m", "module")
    # The source's submodule moves on locally; the copy must follow the source,
    # not the submodule's recorded upstream.
    checked_out = source / "external" / "module"
    _git("-C", checked_out, "config", "user.email", "t@example.invalid")
    _git("-C", checked_out, "config", "user.name", "t")
    (checked_out / "file").write_text("module two")
    _git("-C", checked_out, "commit", "-q", "-am", "two")
    _git("-C", source, "commit", "-q", "-am", "bump")
    copy = tmp_path / "copy"
    commit = release.refuse_uncommitted(source)
    release.sync_checkout(source, copy, commit)
    assert (copy / "external" / "module" / "file").read_text() == "module two"
    (source / "file").write_text("two")
    _git("-C", source, "commit", "-q", "-am", "two")
    release.sync_checkout(source, copy, release.refuse_uncommitted(source))
    assert (copy / "file").read_text() == "two"


def test_the_container_builds_at_a_path_that_names_no_one(tmp_path: Path) -> None:
    layout = Layout(root=tmp_path / "checkout")
    command = release.container_command("tag", layout, (Path("/home/me"),))
    assert f"{layout.release}:/build:Z" in command
    assert command[command.index("--workdir") + 1] == "/build/wiiuport"
    assert "--private-path /home/me" in command[-1]
    assert command[-1].index("build_runtime.py") < command[-1].index("stage_runtime.py")
