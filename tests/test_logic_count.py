"""The logic rate a run reports, and which counter it came from.

`probe_run.py` reports a logic rate. That number decides whether the guest path
holds the logic at thirty hertz while the picture runs at sixty, and it comes
from one of three counters that can each be zero for a completely different
reason. The rule that picks between them is therefore the measurement: get it
wrong and a healthy simulation reports zero, or a dead one reports thirty.

The cases are the three counters, and each is paired with the report the run
would print for it, so a change of counter shows as a change of the sentence
rather than only of a number.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
from typing import Any

import pytest

from wiiuport.control import GateState

_ROOT = Path(__file__).resolve().parents[1]
_probe_run = _ROOT / "tools" / "probe_run.py"
_spec = importlib.util.spec_from_file_location("probe_run_under_test", _probe_run)
assert _spec is not None and _spec.loader is not None
probe_run = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(probe_run)


def gate(**overrides: object) -> GateState:
    """A gate report with the fields the rule reads, and sensible rest."""
    fields: dict[str, object] = {
        "tick": 0x025D42EC,
        "enabled": True,
        "calls": 0,
        "ticks": 0,
        "refusal": "",
        "holding_probe": "installed",
        "calls_at_probe": 0,
        "through": False,
    }
    fields.update(overrides)
    return GateState(**fields)  # type: ignore[arg-type]


class Answer:
    """A control channel that answers `/logic` and nothing else."""

    def __init__(self, report: GateState) -> None:
        self.report = report
        self.asked: list[str] = []

    def read_gate(self, port: int = 0, timeout: float = 0.0) -> GateState:
        self.asked.append("/logic")
        return self.report


@pytest.fixture
def counts(monkeypatch: pytest.MonkeyPatch) -> Any:
    """Run the rule against a given report, and say what it chose."""

    def ask(report: GateState) -> tuple[int, str]:
        channel = Answer(report)
        monkeypatch.setattr(probe_run, "read_gate", channel.read_gate)
        return probe_run._logic_count(21337)

    return ask


def test_the_gates_own_counter_is_used_when_it_is_counting(counts: Any) -> None:
    count, source = counts(gate(calls=900, ticks=450, calls_at_probe=900))
    assert count == 450
    assert "tick counter" in source


def test_a_zero_counter_falls_through_to_the_probe_rather_than_reporting_zero(
    counts: Any,
) -> None:
    """The whole point: zero from a counter that was never counting is not a rate."""
    count, source = counts(gate(calls=0, ticks=0, calls_at_probe=2148))
    assert count == 2148
    assert "not ticks" in source
    assert "at zero" in source


def test_the_pass_through_control_is_named_as_the_control(counts: Any) -> None:
    """Both states are 'enabled'; only one of them is the gate."""
    _, source = counts(gate(through=True, calls_at_probe=900))
    assert "pass-through control" in source


def test_a_gate_with_no_probe_at_all_says_so(counts: Any) -> None:
    count, source = counts(gate(calls_at_probe=None))
    assert count == 0
    assert "nothing counted" in source


def test_a_gate_that_is_out_reports_the_calls_the_probe_saw(counts: Any) -> None:
    """Out of the gate the probe still counts, and the sentence says which it is."""
    count, source = counts(gate(enabled=False, calls_at_probe=2027))
    assert count == 2027
    assert "not ticks" in source


def test_the_probe_label_carries_the_address_it_counted(counts: Any) -> None:
    """A call rate and a tick rate are different measurements; the label says so."""
    _, source = counts(gate(calls_at_probe=100))
    assert "0x025d42ec" in source
