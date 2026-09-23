"""The census client must read every object the runtime counted, or refuse."""

from __future__ import annotations

import pytest
from wiiuport.census import ObjectCensus

from wiiuport.control import ControlUnavailable

URL = "http://127.0.0.1:0/objects"


def outcomes(**counts: int) -> dict[str, int]:
    return {
        name: counts.get(name, 0)
        for name in ("blended", "held", "unmatched", "unverified", "outside", "shading")
    }


def payload(**overrides: object) -> dict:
    body = {
        "frames": 1,
        "objects": 4,
        "outcomes": outcomes(blended=1, held=1, unmatched=2),
        "shaders": 2,
        "rows": [
            {
                "stageIndex": 0,
                "baseHash": 0xEEEE,
                "auxHash": 0,
                "draws": 2,
                "outcomes": outcomes(unmatched=2),
                "mostValues": 3,
                "withoutBlocks": 0,
            }
        ],
    }
    body.update(overrides)
    return body


def test_a_census_reads_its_rows_and_says_which_are_unblended():
    census = ObjectCensus.parse(URL, payload())
    assert census.rows[0].unblended == 2
    text = census.render()
    assert "4 objects under 2 shaders" in text
    assert "2 un-blended" in text
    assert "000000000000eeee" in text


def test_outcomes_that_do_not_add_up_to_the_objects_are_refused():
    with pytest.raises(ControlUnavailable, match="exactly one"):
        ObjectCensus.parse(URL, payload(objects=5))


def test_an_outcome_this_client_does_not_know_is_refused():
    with pytest.raises(ControlUnavailable, match="named outcomes"):
        ObjectCensus.parse(URL, payload(outcomes={**outcomes(blended=4), "vanished": 0}))


def test_a_row_missing_a_field_is_refused():
    body = payload()
    del body["rows"][0]["draws"]
    with pytest.raises(ControlUnavailable, match="draws"):
        ObjectCensus.parse(URL, body)
