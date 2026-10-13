"""Waiting on the title's frames rather than the wall clock."""

from __future__ import annotations

from collections.abc import Iterator

import pytest

from wiiuport import control
from wiiuport.control import GateState


def gate(enabled: bool, ticks: int, calls: int) -> GateState:
    return GateState(
        entry=0x0203593C,
        enabled=enabled,
        calls=calls,
        ticks=ticks,
        refusal="",
        calls_at_probe=calls,
    )


def answering(monkeypatch: pytest.MonkeyPatch, reports: list[GateState]) -> list[int]:
    served: list[int] = []
    sequence: Iterator[GateState] = iter(reports)

    def read(port: int = 0, timeout: float = 0.0) -> GateState:
        report = next(sequence)
        served.append(report.ticks or 0)
        return report

    monkeypatch.setattr(control, "read_gate", read)
    monkeypatch.setattr(control.time, "sleep", lambda _: None)
    return served


def test_a_slow_title_is_waited_on_frame_by_frame(monkeypatch: pytest.MonkeyPatch) -> None:
    served = answering(monkeypatch, [gate(True, t, 2 * t) for t in (10, 10, 10, 11, 12, 14, 99)])
    control.wait_game_frames(4)
    assert len(served) == 6


def test_the_gate_going_in_does_not_count_the_change_of_counter(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reports = [gate(False, 0, 500), gate(False, 0, 502), gate(True, 0, 504), gate(True, 3, 510)]
    served = answering(monkeypatch, reports + [gate(True, 99, 999)])
    control.wait_game_frames(5)
    assert len(served) == 4
