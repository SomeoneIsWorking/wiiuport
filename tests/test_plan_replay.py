import json

import pytest
from wiiuport.planreplay import PlanReplayFailed, PlanReplayReport


def _line(**overrides: object) -> str:
    fields: dict[str, object] = {
        "frames": 16,
        "framesPlanned": 14,
        "outcomes": {"blended": 30, "held": 10},
        "partnersDerived": 20,
        "partnersSearched": 10,
        "partnersReidentified": 2,
        "reidentifyAttempts": 4,
        "partnerCandidates": 250,
        "nearestCandidates": 40,
        "planningNanoseconds": 8_000_000,
    }
    fields.update(overrides)
    return json.dumps(fields)


def test_a_replay_reads_as_costs_per_search_and_per_frame() -> None:
    rendered = PlanReplayReport.parse(_line()).render()
    assert "planned 14 of 16 frames: 40 objects (blended 30, held 10)" in rendered
    assert "(25.0 each)" in rendered
    assert "for identity over 4 (10.0 each)" in rendered
    assert "0.500 ms a frame" in rendered


def test_a_replay_that_planned_nothing_is_refused() -> None:
    with pytest.raises(PlanReplayFailed, match="none of the 2 frames"):
        PlanReplayReport.parse(_line(frames=2, framesPlanned=0))


def test_a_field_the_tool_does_not_know_is_refused() -> None:
    with pytest.raises(TypeError):
        PlanReplayReport.parse(_line(surprise=1))
