"""The offscreen harness must isolate, must time out, and must not kill itself."""

from __future__ import annotations

import os
from pathlib import Path

import pytest
from wiiuport.headless import HeadlessSession, LogType, _own_process_group, log_flags
from wiiuport.paths import Layout


@pytest.fixture
def session(tmp_path: Path) -> HeadlessSession:
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "project-goals.md").write_text("x")
    (tmp_path / "external").mkdir()
    made = HeadlessSession(layout=Layout(root=tmp_path), display=96, activity="test")
    made.prepare()
    return made


def test_our_own_process_group_is_never_returned_for_signalling() -> None:
    """The bug this guards against once killed the tool that launched the child."""
    assert _own_process_group(os.getpid()) is None


def test_environment_is_isolated_from_the_operators_installation(
    session: HeadlessSession,
) -> None:
    env = session.environment()
    assert env["XDG_CONFIG_HOME"] == str(session.config_home)
    assert env["XDG_DATA_HOME"] == str(session.data_home)
    assert env["DISPLAY"] == ":96"
    assert "WAYLAND_DISPLAY" not in env, "a run must not reach the operator's compositor"
    for value in (env["XDG_CONFIG_HOME"], env["XDG_DATA_HOME"], env["XDG_CACHE_HOME"]):
        assert str(Path.home() / ".local" / "share" / "Cemu") != value


def test_written_settings_are_silent_and_vulkan(session: HeadlessSession) -> None:
    settings = (session.config_home / "Cemu" / "settings.xml").read_text()
    assert "<TVDevice></TVDevice>" in settings, "an unwatched run must make no sound"
    assert "<PadDevice></PadDevice>" in settings
    assert "<api>1</api>" in settings, "interpolation substitutes in the Vulkan backend"


def test_missing_keys_are_refused_rather_than_silently_skipped(
    session: HeadlessSession, tmp_path: Path
) -> None:
    from wiiuport.headless import HeadlessError

    with pytest.raises(HeadlessError, match="does not exist"):
        session.prepare(keys_source=tmp_path / "absent-keys.txt")


def test_keys_are_linked_not_copied(session: HeadlessSession, tmp_path: Path) -> None:
    """Restricted data stays the operator's; a copy could be mistaken for ours."""
    keys = tmp_path / "keys.txt"
    keys.write_text("# key material\n")
    session.prepare(keys_source=keys)
    linked = session.data_home / "Cemu" / "keys.txt"
    assert linked.is_symlink()
    assert linked.resolve() == keys.resolve()


def test_a_run_that_overruns_is_reported_as_timed_out(session: HeadlessSession) -> None:
    result = session.run(["sh", "-c", "sleep 30"], timeout_seconds=1.0)
    assert result.timed_out
    assert result.exit_code is None
    assert result.reached_the_binary, "a timeout is a result, not an absence of one"


def test_a_run_that_finishes_reports_its_code(session: HeadlessSession) -> None:
    result = session.run(["sh", "-c", "exit 3"], timeout_seconds=20.0)
    assert not result.timed_out
    assert result.exit_code == 3


def test_log_flags_are_bit_positions_not_values() -> None:
    """logflag is a mask over LogType positions, so GX2 (1) is 2 and the
    capture type (27) is 1 << 27. Writing the position itself would silently
    enable a different log type."""
    assert log_flags(LogType.GX2) == 2
    assert log_flags(LogType.UNIFORM_CAPTURE) == 1 << 27
    assert log_flags(LogType.GX2, LogType.UNIFORM_CAPTURE) == 2 + (1 << 27)
    assert log_flags() == 0


def test_the_written_settings_carry_the_requested_log_flags(tmp_path: Path) -> None:
    layout = Layout(root=tmp_path)
    session = HeadlessSession(layout=layout, logflag=log_flags(LogType.UNIFORM_CAPTURE))
    session.prepare()
    settings = (session.config_home / "Cemu" / "settings.xml").read_text()
    assert f"<logflag>{1 << 27}</logflag>" in settings


def test_a_default_session_enables_no_logging(tmp_path: Path) -> None:
    session = HeadlessSession(layout=Layout(root=tmp_path))
    session.prepare()
    settings = (session.config_home / "Cemu" / "settings.xml").read_text()
    assert "<logflag>0</logflag>" in settings
