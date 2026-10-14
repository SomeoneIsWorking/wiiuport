from __future__ import annotations

import pytest

from wiiuport import gameplay


@pytest.fixture
def pressed(monkeypatch: pytest.MonkeyPatch) -> list[str]:
    buttons: list[str] = []
    monkeypatch.setattr(gameplay, "press", lambda button, port: buttons.append(button))
    return buttons


def _no_wait(_frames: int, _port: int) -> None:
    pass


def test_presses_a_until_the_stage_leaves_the_front_end(pressed: list[str]) -> None:
    stages = iter(["", "sea_T", "sea_T", "sea"])
    reached = gameplay.press_into_world(
        1, read_stage=lambda _port: next(stages), wait_frames=_no_wait
    )
    assert reached == "sea"
    assert pressed == ["a", "a", "a", "a"]


def test_name_entry_is_still_the_front_end(pressed: list[str]) -> None:
    reached = gameplay.press_into_world(
        1, presses=3, read_stage=lambda _port: "Name", wait_frames=_no_wait
    )
    assert reached is None
    assert pressed == ["a", "a", "a"]


def test_a_runtime_that_exited_stops_the_presses(pressed: list[str]) -> None:
    alive = iter([True, False])
    reached = gameplay.press_into_world(
        1,
        still_running=lambda: next(alive),
        read_stage=lambda _port: "",
        wait_frames=_no_wait,
    )
    assert reached is None
    assert pressed == ["a"]
