import json

import pytest
from wiiuport.planreplay import PlanReplayFailed, PlanReplayReport


def _shader(base: str, unverified: int, compared: int) -> dict[str, object]:
    return {
        "baseHash": base,
        "auxHash": "0000000000000000",
        "stageIndex": 0,
        "outcomes": {"blended": 1, "unmatched": 0, "unverified": unverified},
        "compared": compared,
    }


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
        "shaders": [
            _shader("1557c18f92f3bcb9", unverified=9, compared=5),
            _shader("b7252004aba21c10", unverified=0, compared=200),
        ],
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


def test_a_replay_names_the_shaders_that_cost_and_the_ones_unverified() -> None:
    rendered = PlanReplayReport.parse(_line()).render().splitlines()
    costly = rendered.index("most compared of 2 shaders:")
    unverified = rendered.index("most unverified:")
    assert rendered[costly + 1].startswith("  b7252004: 1 objects")
    assert rendered[costly + 2].startswith("  1557c18f: 10 objects, 9 unverified")
    # A shader with none unverified is not ranked among them.
    assert rendered[unverified + 1 :] == [
        "  1557c18f: 10 objects, 9 unverified, 0 unmatched, 5 draws compared"
    ]


def test_a_ranking_nothing_reaches_says_so() -> None:
    rendered = PlanReplayReport.parse(_line(shaders=[])).render()
    assert "most unverified:\n  (none)" in rendered
