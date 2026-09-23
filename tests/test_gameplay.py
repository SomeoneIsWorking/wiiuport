from __future__ import annotations

import pytest

from wiiuport import gameplay


@pytest.fixture
def pressed(monkeypatch: pytest.MonkeyPatch) -> list[str]:
    buttons: list[str] = []
    monkeypatch.setattr(gameplay, "press", lambda button, port: buttons.append(button))
    monkeypatch.setattr(gameplay, "release", lambda port: buttons.append("release"))
    return buttons


def test_alternates_a_and_plus_then_backs_out_of_a_pause(pressed: list[str]) -> None:
    made = gameplay.press_into_world(1, presses=3, interval=0, sleep=lambda _s: None)
    assert made == 3
    assert pressed == ["a", "plus", "a", "release", "b"]


def test_a_runtime_that_exited_stops_the_presses_and_says_how_many(pressed: list[str]) -> None:
    alive = iter([True, True, False])
    made = gameplay.press_into_world(
        1, presses=5, interval=0, still_running=lambda: next(alive), sleep=lambda _s: None
    )
    assert made == 2
    assert pressed == ["a", "plus"]
